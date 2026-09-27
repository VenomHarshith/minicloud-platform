#include "minicloud/controller/api.hpp"

#include "minicloud/common/config.hpp"
#include "minicloud/common/metrics.hpp"
#include "minicloud/controller/repository.hpp"
#include "minicloud/controller/valkey_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace minicloud::controller {
namespace {

namespace http = common::http;

common::HttpResponse error_response(const common::HttpRequest& request,
                                    const http::status status,
                                    const std::string& message) {
  return common::json_response(status, nlohmann::json{{"error", message}}.dump(),
                               request.version(), request.keep_alive());
}

std::pair<std::string, std::string> split_target(const std::string& target) {
  const std::size_t query = target.find('?');
  if (query == std::string::npos) return {target, ""};
  return {target.substr(0, query), target.substr(query + 1)};
}

std::vector<std::string> path_segments(const std::string& path) {
  std::vector<std::string> result;
  std::size_t cursor = 0;
  while (cursor < path.size()) {
    while (cursor < path.size() && path[cursor] == '/') ++cursor;
    if (cursor >= path.size()) break;
    const std::size_t next = path.find('/', cursor);
    result.push_back(path.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor));
    if (next == std::string::npos) break;
    cursor = next + 1;
  }
  return result;
}

bool valid_service_name(const std::string& value) {
  static const std::regex expression("^[a-z][a-z0-9-]{0,62}$");
  return std::regex_match(value, expression);
}

bool valid_health_path(const std::string& value) {
  if (value.empty() || value.size() > 256 || value.front() != '/') return false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character < 0x21 || character > 0x7E || character == '?' || character == '#') return false;
  }
  return true;
}

bool valid_uuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
      continue;
    }
    const char character = value[index];
    const bool hexadecimal = (character >= '0' && character <= '9') ||
                             (character >= 'a' && character <= 'f') ||
                             (character >= 'A' && character <= 'F');
    if (!hexadecimal) return false;
  }
  return true;
}

bool safe_printable(const std::string& value, const std::size_t maximum,
                    const bool allow_space = false) {
  if (value.empty() || value.size() > maximum) return false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character > 0x7EU || character < (allow_space ? 0x20U : 0x21U)) return false;
  }
  return true;
}

template <typename T>
T bounded_integer(const nlohmann::json& json, const char* key, T minimum, T maximum) {
  if (!json.contains(key) || !json.at(key).is_number_integer()) {
    throw std::invalid_argument(std::string(key) + " must be an integer");
  }
  const auto value = json.at(key).get<std::int64_t>();
  if (value < minimum || value > maximum) {
    throw std::invalid_argument(std::string(key) + " is outside the supported range");
  }
  return static_cast<T>(value);
}

void reject_unknown(const nlohmann::json& object, const std::set<std::string>& accepted) {
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (!accepted.contains(iterator.key())) {
      throw std::invalid_argument("unknown field: " + iterator.key());
    }
  }
}

ServiceInput parse_service(const std::string& body) {
  const auto json = nlohmann::json::parse(body);
  if (!json.is_object()) throw std::invalid_argument("request body must be a JSON object");
  reject_unknown(json, {"name", "image", "replicas", "cpuMillis", "memoryMb",
                        "containerPort", "healthPath", "environment", "placement"});
  ServiceInput input;
  input.name = json.value("name", "");
  input.image = json.value("image", "");
  input.replicas = bounded_integer<std::int32_t>(json, "replicas", 0, 50);
  input.cpu_millis = bounded_integer<std::int32_t>(json, "cpuMillis", 10, 128000);
  input.memory_mb = bounded_integer<std::int32_t>(json, "memoryMb", 16, 1048576);
  input.container_port = bounded_integer<std::int32_t>(json, "containerPort", 1, 65535);
  input.health_path = json.value("healthPath", "/health");
  input.environment = json.value("environment", nlohmann::json::object());
  input.placement = json.value("placement", nlohmann::json::object());
  if (!valid_service_name(input.name)) {
    throw std::invalid_argument("name must be a lowercase DNS label");
  }
  if (!safe_printable(input.image, 512)) {
    throw std::invalid_argument("image must contain 1-512 safe characters");
  }
  if (!valid_health_path(input.health_path)) throw std::invalid_argument("healthPath is invalid");
  if (!input.environment.is_object() || input.environment.dump().size() > 16 * 1024) {
    throw std::invalid_argument("environment must be a JSON object no larger than 16 KiB");
  }
  static const std::regex env_name("^[A-Za-z_][A-Za-z0-9_]{0,127}$");
  for (auto iterator = input.environment.begin(); iterator != input.environment.end(); ++iterator) {
    if (!std::regex_match(iterator.key(), env_name) || !iterator.value().is_string() ||
        iterator.value().get_ref<const std::string&>().size() > 4096 ||
        iterator.value().get_ref<const std::string&>().find('\0') != std::string::npos) {
      throw std::invalid_argument("environment variables must have valid names and string values");
    }
  }
  if (!input.placement.is_object() || input.placement.dump().size() > 8192) {
    throw std::invalid_argument("placement must be a JSON object no larger than 8 KiB");
  }
  reject_unknown(input.placement, {"requiredLabels", "antiAffinityGroup"});
  const auto labels = input.placement.value("requiredLabels", nlohmann::json::object());
  if (!labels.is_object() || labels.size() > 32) {
    throw std::invalid_argument("placement.requiredLabels must be an object with at most 32 entries");
  }
  static const std::regex label_name("^[A-Za-z0-9][A-Za-z0-9._/-]{0,127}$");
  for (auto iterator = labels.begin(); iterator != labels.end(); ++iterator) {
    if (!std::regex_match(iterator.key(), label_name) || !iterator.value().is_string() ||
        !safe_printable(iterator.value().get_ref<const std::string&>(), 256, true)) {
      throw std::invalid_argument(
          "placement.requiredLabels must contain valid names and printable string values");
    }
  }
  if (input.placement.contains("antiAffinityGroup")) {
    if (!input.placement.at("antiAffinityGroup").is_string() ||
        !valid_service_name(input.placement.at("antiAffinityGroup").get_ref<const std::string&>())) {
      throw std::invalid_argument("placement.antiAffinityGroup must be a lowercase DNS label");
    }
  }
  return input;
}

std::size_t tail_from_query(const std::string& query) {
  if (query.rfind("tail=", 0) != 0) return 200;
  std::size_t result{};
  const std::string value = query.substr(5);
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return 200;
  return std::clamp<std::size_t>(result, 1, 1000);
}

}  // namespace

Api::Api(Repository& repository, ValkeyStore& valkey,
         common::MetricsRegistry& metrics, std::string api_token)
    : repository_(repository), valkey_(valkey), metrics_(metrics),
      api_token_(std::move(api_token)) {
  if (api_token_.size() < 32) throw std::invalid_argument("API token must have at least 32 characters");
}

common::HttpResponse Api::handle(const common::HttpRequest& request) {
  metrics_.increment("minicloud_controller_http_requests_total");
  if (request.target().size() > 2048) {
    return error_response(request, http::status::uri_too_long, "request target is too long");
  }
  const auto [path, query] = split_target(std::string(request.target()));
  const auto segments = path_segments(path);
  if (request.method() == http::verb::get && path == "/health") {
    return common::json_response(http::status::ok, "{\"status\":\"ok\"}",
                                 request.version(), request.keep_alive());
  }
  if (request.method() == http::verb::get && path == "/api/v1/snapshot") {
    if (!authorized(request)) {
      return error_response(request, http::status::unauthorized,
                            "valid bearer token required");
    }
    return snapshot(request);
  }
  if (request.method() == http::verb::post && path == "/api/v1/services") {
    if (!authorized(request)) return error_response(request, http::status::unauthorized, "valid bearer token required");
    return create_service(request);
  }
  if (segments.size() == 5 && segments[0] == "api" && segments[1] == "v1" &&
      segments[2] == "services" && request.method() == http::verb::post) {
    if (!authorized(request)) return error_response(request, http::status::unauthorized, "valid bearer token required");
    return service_action(request, segments[3], segments[4]);
  }
  if (segments.size() == 4 && segments[0] == "api" && segments[1] == "v1" &&
      segments[2] == "services" && request.method() == http::verb::delete_) {
    if (!authorized(request)) return error_response(request, http::status::unauthorized, "valid bearer token required");
    return service_action(request, segments[3], "delete");
  }
  if (segments.size() == 5 && segments[0] == "api" && segments[1] == "v1" &&
      segments[2] == "allocations" && segments[4] == "logs" && request.method() == http::verb::get) {
    if (!authorized(request)) return error_response(request, http::status::unauthorized, "valid bearer token required");
    return allocation_logs(request, segments[3], query);
  }
  return error_response(request, http::status::not_found, "route not found");
}

bool Api::authorized(const common::HttpRequest& request) const {
  const auto found = request.find(http::field::authorization);
  if (found == request.end()) return false;
  constexpr std::string_view prefix = "Bearer ";
  const std::string value(found->value());
  return value.rfind(prefix, 0) == 0 &&
         common::constant_time_equal(value.substr(prefix.size()), api_token_);
}

common::HttpResponse Api::snapshot(const common::HttpRequest& request) {
  const ClusterSnapshot state = repository_.snapshot();
  std::size_t desired = 0;
  std::size_t ready = 0;
  std::size_t ready_nodes = 0;
  std::size_t pending = 0;
  std::size_t restarts = 0;
  std::map<std::string, std::size_t> ready_by_service;
  for (const auto& service : state.services) desired += static_cast<std::size_t>(service.replicas);
  for (const auto& node : state.nodes) if (node.status == "ready") ++ready_nodes;
  for (const auto& allocation : state.allocations) {
    if (allocation.observed_state == "running" && allocation.endpoint) {
      ++ready;
      ++ready_by_service[allocation.service_name];
    }
    if (allocation.observed_state == "pending") ++pending;
    restarts += static_cast<std::size_t>(std::max(0, allocation.restart_count));
  }
  nlohmann::json services = nlohmann::json::array();
  for (const auto& service : state.services) {
    nlohmann::json encoded = service;
    encoded["readyReplicas"] = ready_by_service[service.name];
    services.push_back(std::move(encoded));
  }
  const nlohmann::json response = {
      {"overview", {{"healthy", true}, {"services", state.services.size()},
                    {"desiredReplicas", desired}, {"readyReplicas", ready},
                    {"readyNodes", ready_nodes}, {"totalNodes", state.nodes.size()},
                    {"pendingAllocations", pending}, {"restartCount", restarts},
                    {"schedulerLeader", "controller-0"}}},
      {"services", services}, {"nodes", state.nodes},
      {"allocations", state.allocations}, {"events", state.events}};
  return common::json_response(http::status::ok, response.dump(),
                               request.version(), request.keep_alive());
}

common::HttpResponse Api::create_service(const common::HttpRequest& request) {
  try {
    const ServiceRecord service = repository_.create_service(parse_service(request.body()));
    metrics_.increment("minicloud_controller_services_created_total");
    return common::json_response(http::status::created, nlohmann::json(service).dump(),
                                 request.version(), request.keep_alive());
  } catch (const nlohmann::json::exception& error) {
    return error_response(request, http::status::bad_request, std::string("invalid JSON: ") + error.what());
  } catch (const std::invalid_argument& error) {
    return error_response(request, http::status::bad_request, error.what());
  } catch (const ConflictError&) {
    return error_response(request, http::status::conflict, "service name already exists");
  }
}

common::HttpResponse Api::service_action(const common::HttpRequest& request,
                                         const std::string& name,
                                         const std::string& action) {
  if (!valid_service_name(name)) return error_response(request, http::status::bad_request, "invalid service name");
  try {
    if (action == "scale") {
      const auto body = nlohmann::json::parse(request.body());
      if (!body.is_object()) throw std::invalid_argument("request body must be a JSON object");
      reject_unknown(body, {"replicas"});
      const auto replicas = bounded_integer<std::int32_t>(body, "replicas", 0, 50);
      const ServiceRecord service = repository_.scale_service(name, replicas);
      return common::json_response(http::status::ok, nlohmann::json(service).dump(),
                                   request.version(), request.keep_alive());
    }
    if (action == "restart") {
      const ServiceRecord service = repository_.restart_service(name);
      return common::json_response(http::status::accepted, nlohmann::json(service).dump(),
                                   request.version(), request.keep_alive());
    }
    if (action == "delete") {
      if (!repository_.delete_service(name)) throw std::out_of_range("service not found");
      return common::json_response(http::status::accepted,
                                   nlohmann::json{{"name", name}, {"status", "draining"}}.dump(),
                                   request.version(), request.keep_alive());
    }
    return error_response(request, http::status::not_found, "unknown service action");
  } catch (const nlohmann::json::exception& error) {
    return error_response(request, http::status::bad_request, std::string("invalid JSON: ") + error.what());
  } catch (const std::invalid_argument& error) {
    return error_response(request, http::status::bad_request, error.what());
  } catch (const std::out_of_range&) {
    return error_response(request, http::status::not_found, "service not found");
  }
}

common::HttpResponse Api::allocation_logs(const common::HttpRequest& request,
                                          const std::string& allocation_id,
                                          const std::string& query) {
  if (!valid_uuid(allocation_id)) {
    return error_response(request, http::status::bad_request, "invalid allocation id");
  }
  const auto lines = valkey_.logs(allocation_id, tail_from_query(query));
  return common::json_response(http::status::ok, nlohmann::json{{"lines", lines}}.dump(),
                               request.version(), request.keep_alive());
}

}  // namespace minicloud::controller

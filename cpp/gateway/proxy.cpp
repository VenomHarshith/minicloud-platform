#include "minicloud/gateway/proxy.hpp"

#include "minicloud/common/config.hpp"
#include "minicloud/common/metrics.hpp"
#include "minicloud/controller/valkey_store.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace minicloud::gateway {
namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {
struct Upstream {
  std::string host;
  std::string port;
  std::string authority;
};

Upstream parse_endpoint(const std::string& url) {
  constexpr std::string_view prefix = "http://";
  if (url.rfind(prefix, 0) != 0 || url.size() > 512) {
    throw std::invalid_argument("gateway supports bounded HTTP endpoints only");
  }
  const std::string authority = url.substr(prefix.size());
  if (authority.empty() || authority.find_first_of("/?#@") != std::string::npos) {
    throw std::invalid_argument("upstream endpoint must contain only a host and port");
  }

  std::string host;
  std::string port;
  if (authority.front() == '[') {
    const std::size_t closing = authority.find(']');
    if (closing == std::string::npos || closing == 1 ||
        closing + 2 >= authority.size() || authority[closing + 1] != ':') {
      throw std::invalid_argument("invalid bracketed upstream endpoint");
    }
    host = authority.substr(1, closing - 1);
    port = authority.substr(closing + 2);
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size() ||
        authority.find(':') != colon) {
      throw std::invalid_argument("upstream endpoint is missing a valid port");
    }
    host = authority.substr(0, colon);
    port = authority.substr(colon + 1);
  }
  if (host.size() > 253) {
    throw std::invalid_argument("upstream host is too long");
  }
  for (const char character : host) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte <= 0x20U || byte == 0x7FU) {
      throw std::invalid_argument("upstream host contains whitespace or control characters");
    }
  }
  std::uint32_t parsed_port{};
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), parsed_port);
  if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
      parsed_port == 0 || parsed_port > 65535) {
    throw std::invalid_argument("upstream endpoint has an invalid port");
  }
  return Upstream{std::move(host), std::move(port), authority};
}

common::HttpResponse gateway_error(const common::HttpRequest& request,
                                   const http::status status,
                                   const std::string& message) {
  return common::json_response(status, nlohmann::json{{"error", message}}.dump(),
                               request.version(), request.keep_alive());
}

bool idempotent(const http::verb method) {
  return method == http::verb::get || method == http::verb::head ||
         method == http::verb::put || method == http::verb::delete_ ||
         method == http::verb::options || method == http::verb::trace;
}

common::HttpResponse forward(const common::HttpRequest& inbound,
                             const std::string& path,
                             const Upstream& upstream) {
  asio::io_context context;
  tcp::resolver resolver(context);
  beast::tcp_stream stream(context);
  stream.expires_after(std::chrono::seconds(5));
  const auto endpoints = resolver.resolve(upstream.host, upstream.port);
  stream.connect(endpoints);

  http::request<http::string_body> outbound{inbound.method(), path, 11};
  outbound.set(http::field::host, upstream.authority);
  outbound.set(http::field::user_agent, "MiniCloud-Gateway/0.1");
  if (const auto type = inbound.find(http::field::content_type); type != inbound.end()) {
    outbound.set(http::field::content_type, type->value());
  }
  if (const auto request_id = inbound.find("x-request-id"); request_id != inbound.end()) {
    outbound.set("x-request-id", request_id->value());
  } else {
    outbound.set("x-request-id", common::random_uuid());
  }
  outbound.set("x-forwarded-proto", "http");
  outbound.body() = inbound.body();
  outbound.prepare_payload();
  http::write(stream, outbound);

  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  parser.body_limit(4 * 1024 * 1024);
  parser.skip(inbound.method() == http::verb::head);
  stream.expires_after(std::chrono::seconds(15));
  http::read(stream, buffer, parser);
  const auto received = parser.release();

  common::HttpResponse response{received.result(), inbound.version()};
  response.set(http::field::server, "MiniCloud-Gateway/0.1");
  if (const auto type = received.find(http::field::content_type); type != received.end()) {
    response.set(http::field::content_type, type->value());
  }
  if (const auto request_id = outbound.find("x-request-id"); request_id != outbound.end()) {
    response.set("x-request-id", request_id->value());
  }
  response.set(http::field::cache_control, "no-store");
  response.body() = received.body();
  response.keep_alive(inbound.keep_alive());
  response.prepare_payload();
  beast::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
  return response;
}
}  // namespace

ReverseProxy::ReverseProxy(controller::ValkeyStore& discovery,
                           common::MetricsRegistry& metrics)
    : discovery_(discovery), metrics_(metrics) {}

std::size_t ReverseProxy::next_index(const std::string& service, const std::size_t size) {
  std::lock_guard lock(cursor_mutex_);
  std::size_t& cursor = cursors_[service];
  const std::size_t selected = cursor % size;
  cursor = (cursor + 1) % size;
  return selected;
}

common::HttpResponse ReverseProxy::handle(const common::HttpRequest& request) {
  metrics_.increment("minicloud_gateway_requests_total");
  if (request.method() == http::verb::get && request.target() == "/health") {
    return common::json_response(http::status::ok, "{\"status\":\"ok\"}",
                                 request.version(), request.keep_alive());
  }
  const std::string target(request.target());
  constexpr std::string_view prefix = "/services/";
  if (target.rfind(prefix, 0) != 0) {
    return gateway_error(request, http::status::not_found,
                         "use /services/<service>/<path>");
  }
  const std::size_t service_end = target.find_first_of("/?", prefix.size());
  const std::string service = target.substr(
      prefix.size(), service_end == std::string::npos ? std::string::npos
                                                       : service_end - prefix.size());
  static const std::regex service_name("^[a-z][a-z0-9-]{0,62}$");
  if (!std::regex_match(service, service_name)) {
    return gateway_error(request, http::status::bad_request, "invalid service name");
  }
  const std::string path = service_end == std::string::npos ? "/" :
      target[service_end] == '?' ? "/" + target.substr(service_end) : target.substr(service_end);
  const auto endpoints = discovery_.endpoints(service);
  if (endpoints.empty()) {
    metrics_.increment("minicloud_gateway_no_endpoint_total");
    return gateway_error(request, http::status::service_unavailable,
                         "service has no ready endpoints");
  }
  const std::size_t first = next_index(service, endpoints.size());
  const std::size_t attempts = idempotent(request.method()) ? std::min<std::size_t>(2, endpoints.size()) : 1;
  for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
    const auto& endpoint = endpoints[(first + attempt) % endpoints.size()];
    try {
      auto response = forward(request, path, parse_endpoint(endpoint.url));
      metrics_.increment("minicloud_gateway_upstream_responses_total");
      return response;
    } catch (const std::exception&) {
      metrics_.increment("minicloud_gateway_upstream_errors_total");
    }
  }
  return gateway_error(request, http::status::bad_gateway,
                       "all selected upstream connections failed");
}

}  // namespace minicloud::gateway

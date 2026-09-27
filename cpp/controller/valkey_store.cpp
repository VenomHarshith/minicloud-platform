#include "minicloud/controller/valkey_store.hpp"

#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace minicloud::controller {
namespace {

struct ParsedUrl {
  std::string host;
  int port{};
  int database{};
};

ParsedUrl parse_url(const std::string& url) {
  constexpr char prefix[] = "redis://";
  if (url.rfind(prefix, 0) != 0 || url.size() > 2048) {
    throw std::invalid_argument("Valkey URL must use redis://host:port/database");
  }
  const std::size_t slash = url.find('/', sizeof(prefix) - 1);
  const std::string authority = url.substr(sizeof(prefix) - 1, slash - (sizeof(prefix) - 1));
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
    throw std::invalid_argument("Valkey URL is missing its port");
  }
  const int port = std::stoi(authority.substr(colon + 1));
  const int database = slash == std::string::npos ? 0 : std::stoi(url.substr(slash + 1));
  if (port <= 0 || port > 65535 || database < 0 || database > 15) {
    throw std::invalid_argument("Valkey URL contains an invalid port or database");
  }
  return ParsedUrl{authority.substr(0, colon), port, database};
}

class Reply final {
 public:
  explicit Reply(redisReply* value) : value_(value) {
    if (value_ == nullptr) throw std::runtime_error("Valkey returned no reply");
    if (value_->type == REDIS_REPLY_ERROR) {
      const std::string message(value_->str == nullptr ? "Valkey error" : value_->str,
                                value_->len);
      freeReplyObject(value_);
      value_ = nullptr;
      throw std::runtime_error(message);
    }
  }
  ~Reply() { if (value_ != nullptr) freeReplyObject(value_); }
  Reply(const Reply&) = delete;
  Reply& operator=(const Reply&) = delete;
  [[nodiscard]] redisReply* get() const noexcept { return value_; }

 private:
  redisReply* value_;
};

std::int64_t unix_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string endpoint_key(const std::string& service) {
  return "minicloud:discovery:" + service;
}

std::string logs_key(const std::string& allocation) {
  return "minicloud:logs:" + allocation;
}

std::string log_batch_key(const std::string& allocation, const std::string& batch) {
  return "minicloud:log-batch:" + allocation + ":" + batch;
}

}  // namespace

class ValkeyStore::Implementation final {
 public:
  explicit Implementation(const std::string& url) : parsed_(parse_url(url)) { connect(); }
  ~Implementation() { if (context_ != nullptr) redisFree(context_); }

  void connect() {
    if (context_ != nullptr) redisFree(context_);
    context_ = redisConnect(parsed_.host.c_str(), parsed_.port);
    if (context_ == nullptr) throw std::runtime_error("unable to allocate Valkey connection");
    if (context_->err != 0) {
      const std::string message = context_->errstr;
      redisFree(context_);
      context_ = nullptr;
      throw std::runtime_error("Valkey connection failed: " + message);
    }
    if (parsed_.database != 0) {
      Reply selected(static_cast<redisReply*>(redisCommand(context_, "SELECT %d", parsed_.database)));
      (void)selected;
    }
  }

  template <typename... Args>
  Reply command(const char* format, Args... arguments) {
    if (context_ == nullptr || context_->err != 0) connect();
    redisReply* raw = static_cast<redisReply*>(redisCommand(context_, format, arguments...));
    if (raw == nullptr && context_->err != 0) {
      connect();
      raw = static_cast<redisReply*>(redisCommand(context_, format, arguments...));
    }
    return Reply(raw);
  }

  Reply command(const std::vector<std::string>& arguments) {
    if (arguments.empty() ||
        arguments.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument("invalid Valkey argument count");
    }
    std::vector<const char*> values;
    std::vector<std::size_t> lengths;
    values.reserve(arguments.size());
    lengths.reserve(arguments.size());
    for (const auto& argument : arguments) {
      values.push_back(argument.data());
      lengths.push_back(argument.size());
    }
    const auto execute = [&]() {
      return static_cast<redisReply*>(redisCommandArgv(
          context_, static_cast<int>(values.size()), values.data(), lengths.data()));
    };
    if (context_ == nullptr || context_->err != 0) connect();
    redisReply* raw = execute();
    if (raw == nullptr && context_->err != 0) {
      connect();
      raw = execute();
    }
    return Reply(raw);
  }

  ParsedUrl parsed_;
  redisContext* context_{nullptr};
  std::mutex mutex_;
};

ValkeyStore::ValkeyStore(std::string url)
    : implementation_(std::make_unique<Implementation>(std::move(url))) {}
ValkeyStore::~ValkeyStore() = default;

void ValkeyStore::ping() {
  std::lock_guard lock(implementation_->mutex_);
  Reply reply = implementation_->command("PING");
  if (reply.get()->type != REDIS_REPLY_STATUS ||
      std::string(reply.get()->str, reply.get()->len) != "PONG") {
    throw std::runtime_error("Valkey PING did not return PONG");
  }
}

void ValkeyStore::publish_endpoint(const DiscoveredEndpoint& endpoint, const int ttl_seconds) {
  if (ttl_seconds < 1 || ttl_seconds > 300) throw std::invalid_argument("invalid endpoint TTL");
  const std::string key = endpoint_key(endpoint.service_name);
  const std::string value = nlohmann::json{{"allocationId", endpoint.allocation_id},
                                           {"serviceName", endpoint.service_name},
                                           {"url", endpoint.url}}
                                .dump();
  std::lock_guard lock(implementation_->mutex_);
  Reply cleanup = implementation_->command("ZREMRANGEBYSCORE %b -inf %lld", key.data(), key.size(),
                                            static_cast<long long>(unix_seconds()));
  (void)cleanup;
  Reply existing = implementation_->command("ZRANGE %b 0 -1", key.data(), key.size());
  if (existing.get()->type == REDIS_REPLY_ARRAY) {
    for (std::size_t index = 0; index < existing.get()->elements; ++index) {
      redisReply* item = existing.get()->element[index];
      if (item == nullptr || item->type != REDIS_REPLY_STRING) continue;
      const std::string member(item->str, item->len);
      try {
        if (nlohmann::json::parse(member).value("allocationId", "") !=
            endpoint.allocation_id) continue;
      } catch (const nlohmann::json::exception&) {
        // Malformed ephemeral members are safe to remove.
      }
      Reply removed = implementation_->command("ZREM %b %b", key.data(), key.size(),
                                                member.data(), member.size());
      (void)removed;
    }
  }
  Reply added = implementation_->command("ZADD %b %lld %b", key.data(), key.size(),
                                          static_cast<long long>(unix_seconds() + ttl_seconds),
                                          value.data(), value.size());
  (void)added;
  Reply expires = implementation_->command("EXPIRE %b %d", key.data(), key.size(), ttl_seconds * 4);
  (void)expires;
}

void ValkeyStore::remove_endpoint(const std::string& service_name,
                                  const std::string& allocation_id) {
  const std::string key = endpoint_key(service_name);
  std::lock_guard lock(implementation_->mutex_);
  Reply values = implementation_->command("ZRANGE %b 0 -1", key.data(), key.size());
  if (values.get()->type != REDIS_REPLY_ARRAY) return;
  for (std::size_t index = 0; index < values.get()->elements; ++index) {
    redisReply* item = values.get()->element[index];
    if (item == nullptr || item->type != REDIS_REPLY_STRING) continue;
    const std::string value(item->str, item->len);
    try {
      if (nlohmann::json::parse(value).value("allocationId", "") == allocation_id) {
        Reply removed = implementation_->command("ZREM %b %b", key.data(), key.size(),
                                                  value.data(), value.size());
        (void)removed;
      }
    } catch (const nlohmann::json::exception&) {
      Reply removed = implementation_->command("ZREM %b %b", key.data(), key.size(),
                                                value.data(), value.size());
      (void)removed;
    }
  }
}

std::vector<DiscoveredEndpoint> ValkeyStore::endpoints(const std::string& service_name) {
  const std::string key = endpoint_key(service_name);
  std::lock_guard lock(implementation_->mutex_);
  Reply cleanup = implementation_->command("ZREMRANGEBYSCORE %b -inf %lld", key.data(), key.size(),
                                            static_cast<long long>(unix_seconds()));
  (void)cleanup;
  Reply values = implementation_->command("ZRANGEBYSCORE %b %lld +inf", key.data(), key.size(),
                                           static_cast<long long>(unix_seconds() + 1));
  std::vector<DiscoveredEndpoint> result;
  if (values.get()->type != REDIS_REPLY_ARRAY) return result;
  for (std::size_t index = 0; index < values.get()->elements; ++index) {
    redisReply* item = values.get()->element[index];
    if (item == nullptr || item->type != REDIS_REPLY_STRING) continue;
    try {
      const auto json = nlohmann::json::parse(std::string(item->str, item->len));
      result.push_back(DiscoveredEndpoint{json.at("allocationId").get<std::string>(),
                                          json.at("serviceName").get<std::string>(),
                                          json.at("url").get<std::string>()});
    } catch (const nlohmann::json::exception&) {
      // Corrupt ephemeral entries are ignored and will age out.
    }
  }
  return result;
}

bool ValkeyStore::append_logs(const std::string& allocation_id,
                             const std::string& batch_id,
                             const std::vector<std::string>& lines,
                             const std::size_t retention_lines) {
  const std::string key = logs_key(allocation_id);
  const std::string dedupe_key = log_batch_key(allocation_id, batch_id);
  const std::size_t bounded_retention = std::clamp<std::size_t>(retention_lines, 1, 10000);
  static const std::string script =
      "if redis.call('EXISTS',KEYS[2])==1 then return 0 end "
      "for i=2,#ARGV do redis.call('RPUSH',KEYS[1],ARGV[i]) end "
      "redis.call('LTRIM',KEYS[1],-tonumber(ARGV[1]),-1) "
      "redis.call('EXPIRE',KEYS[1],86400) "
      "redis.call('SET',KEYS[2],'1','EX',86400) return 1";
  std::vector<std::string> arguments{
      "EVAL", script, "2", key, dedupe_key, std::to_string(bounded_retention)};
  arguments.reserve(6 + lines.size());
  for (const auto& raw : lines) arguments.push_back(raw.substr(0, 16 * 1024));
  std::lock_guard lock(implementation_->mutex_);
  Reply result = implementation_->command(arguments);
  if (result.get()->type != REDIS_REPLY_INTEGER ||
      (result.get()->integer != 0 && result.get()->integer != 1)) {
    throw std::runtime_error("Valkey log append script returned an invalid result");
  }
  return result.get()->integer == 1;
}

std::vector<std::string> ValkeyStore::logs(const std::string& allocation_id,
                                           const std::size_t tail) {
  const std::string key = logs_key(allocation_id);
  const std::size_t bounded = std::clamp<std::size_t>(tail, 1, 1000);
  std::lock_guard lock(implementation_->mutex_);
  Reply values = implementation_->command("LRANGE %b -%lld -1", key.data(), key.size(),
                                           static_cast<long long>(bounded));
  std::vector<std::string> result;
  if (values.get()->type != REDIS_REPLY_ARRAY) return result;
  result.reserve(values.get()->elements);
  for (std::size_t index = 0; index < values.get()->elements; ++index) {
    redisReply* item = values.get()->element[index];
    if (item != nullptr && item->type == REDIS_REPLY_STRING) {
      result.emplace_back(item->str, item->len);
    }
  }
  return result;
}

}  // namespace minicloud::controller

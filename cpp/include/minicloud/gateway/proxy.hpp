#pragma once

#include "minicloud/common/http_server.hpp"

#include <map>
#include <mutex>
#include <string>

namespace minicloud::common { class MetricsRegistry; }
namespace minicloud::controller { class ValkeyStore; }

namespace minicloud::gateway {

class ReverseProxy final {
 public:
  ReverseProxy(controller::ValkeyStore& discovery,
               common::MetricsRegistry& metrics);
  [[nodiscard]] common::HttpResponse handle(const common::HttpRequest& request);

 private:
  [[nodiscard]] std::size_t next_index(const std::string& service, std::size_t size);
  controller::ValkeyStore& discovery_;
  common::MetricsRegistry& metrics_;
  std::mutex cursor_mutex_;
  std::map<std::string, std::size_t> cursors_;
};

}  // namespace minicloud::gateway

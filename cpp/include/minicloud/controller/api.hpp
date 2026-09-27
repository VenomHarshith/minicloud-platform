#pragma once

#include "minicloud/common/http_server.hpp"

#include <memory>
#include <string>

namespace minicloud::common { class MetricsRegistry; }

namespace minicloud::controller {

class Repository;
class ValkeyStore;

class Api final {
 public:
  Api(Repository& repository, ValkeyStore& valkey,
      common::MetricsRegistry& metrics, std::string api_token);
  [[nodiscard]] common::HttpResponse handle(const common::HttpRequest& request);

 private:
  [[nodiscard]] bool authorized(const common::HttpRequest& request) const;
  [[nodiscard]] common::HttpResponse snapshot(const common::HttpRequest& request);
  [[nodiscard]] common::HttpResponse create_service(const common::HttpRequest& request);
  [[nodiscard]] common::HttpResponse service_action(const common::HttpRequest& request,
                                                     const std::string& name,
                                                     const std::string& action);
  [[nodiscard]] common::HttpResponse allocation_logs(const common::HttpRequest& request,
                                                     const std::string& allocation_id,
                                                     const std::string& query);

  Repository& repository_;
  ValkeyStore& valkey_;
  common::MetricsRegistry& metrics_;
  std::string api_token_;
};

}  // namespace minicloud::controller

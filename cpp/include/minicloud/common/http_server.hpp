#pragma once

#include "minicloud/common/config.hpp"

#include <boost/beast/http.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace minicloud::common {

namespace http = boost::beast::http;
using HttpRequest = http::request<http::string_body>;
using HttpResponse = http::response<http::string_body>;
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer final {
 public:
  HttpServer(BindAddress address, HttpHandler handler, std::size_t max_body_bytes = 64 * 1024);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void start();
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(); }

 private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
  std::atomic<bool> running_{false};
};

[[nodiscard]] HttpResponse json_response(http::status status,
                                         const std::string& json,
                                         unsigned version = 11,
                                         bool keep_alive = false);
[[nodiscard]] HttpResponse text_response(http::status status,
                                         const std::string& body,
                                         const std::string& content_type,
                                         unsigned version = 11,
                                         bool keep_alive = false);

}  // namespace minicloud::common

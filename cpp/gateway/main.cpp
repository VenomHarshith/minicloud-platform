#include "minicloud/common/config.hpp"
#include "minicloud/common/http_server.hpp"
#include "minicloud/common/metrics.hpp"
#include "minicloud/controller/valkey_store.hpp"
#include "minicloud/gateway/proxy.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::atomic<bool> stopping{false};
volatile std::sig_atomic_t signal_requested = 0;
void stop(int) { signal_requested = 1; }
}

int main() {
  using namespace minicloud;
  try {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    controller::ValkeyStore valkey(common::Environment::required("MINICLOUD_VALKEY_URL"));
    valkey.ping();
    common::MetricsRegistry metrics;
    gateway::ReverseProxy proxy(valkey, metrics);
    common::HttpServer gateway_server(
        common::Environment::bind("MINICLOUD_GATEWAY_BIND", "127.0.0.1:8080"),
        [&](const common::HttpRequest& request) { return proxy.handle(request); },
        4 * 1024 * 1024);
    common::HttpServer metrics_server(
        common::Environment::bind("MINICLOUD_METRICS_BIND", "127.0.0.1:9092"),
        [&](const common::HttpRequest& request) {
          if (request.method() == common::http::verb::get && request.target() == "/metrics") {
            return common::text_response(common::http::status::ok, metrics.render(),
                                         "text/plain; version=0.0.4; charset=utf-8",
                                         request.version(), request.keep_alive());
          }
          return common::json_response(common::http::status::not_found,
                                       "{\"error\":\"route not found\"}",
                                       request.version(), request.keep_alive());
        }, 1024);
    std::exception_ptr background_error;
    std::mutex error_mutex;
    auto record_background_error = [&] {
      std::lock_guard lock(error_mutex);
      if (!background_error) background_error = std::current_exception();
      stopping.store(true);
    };
    std::thread gateway_thread([&] {
      try { gateway_server.start(); } catch (...) { record_background_error(); }
    });
    std::thread metrics_thread([&] {
      try { metrics_server.start(); } catch (...) { record_background_error(); }
    });
    std::cout << "{\"component\":\"gateway\",\"event\":\"started\"}" << std::endl;
    while (!stopping.load() && signal_requested == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    gateway_server.stop();
    metrics_server.stop();
    if (gateway_thread.joinable()) gateway_thread.join();
    if (metrics_thread.joinable()) metrics_thread.join();
    {
      std::lock_guard lock(error_mutex);
      if (background_error) std::rethrow_exception(background_error);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "gateway fatal error: " << error.what() << std::endl;
    return 1;
  }
}

#include "minicloud/common/config.hpp"
#include "minicloud/common/http_server.hpp"
#include "minicloud/common/metrics.hpp"
#include "minicloud/controller/api.hpp"
#include "minicloud/controller/grpc_service.hpp"
#include "minicloud/controller/repository.hpp"
#include "minicloud/controller/valkey_store.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t signal_stop_requested = 0;
std::atomic<bool> asynchronous_stop_requested{false};

void request_stop(int) noexcept { signal_stop_requested = 1; }

bool should_stop() noexcept {
  return signal_stop_requested != 0 ||
         asynchronous_stop_requested.load(std::memory_order_acquire);
}

std::string address_text(const minicloud::common::BindAddress& address) {
  const bool ipv6_literal = address.host.find(':') != std::string::npos;
  return (ipv6_literal ? "[" + address.host + "]" : address.host) + ":" +
         std::to_string(address.port);
}
}  // namespace

int main() {
  using namespace minicloud;
  try {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    const auto http_bind = common::Environment::bind("MINICLOUD_HTTP_BIND", "127.0.0.1:8090");
    const auto grpc_bind = common::Environment::bind("MINICLOUD_GRPC_BIND", "127.0.0.1:50051");
    const auto metrics_bind = common::Environment::bind("MINICLOUD_METRICS_BIND", "127.0.0.1:9091");
    const auto reconcile_interval = std::chrono::milliseconds(common::Environment::integer(
        "MINICLOUD_RECONCILE_INTERVAL_MS", 1000, 100, 60000));
    const auto heartbeat_timeout = static_cast<std::int32_t>(common::Environment::integer(
        "MINICLOUD_HEARTBEAT_TIMEOUT_SECONDS", 12, 5, 300));

    controller::Repository repository(common::Environment::required("MINICLOUD_DATABASE_URL"));
    repository.verify_schema();
    const auto controller_epoch_value = repository.acquire_controller_epoch();
    const auto controller_epoch = static_cast<std::uint64_t>(controller_epoch_value);
    controller::ValkeyStore valkey(common::Environment::required("MINICLOUD_VALKEY_URL"));
    valkey.ping();
    common::MetricsRegistry metrics;
    controller::Api api(repository, valkey, metrics,
                        common::Environment::required("MINICLOUD_API_TOKEN"));
    controller::ControllerGrpcService grpc_service(
        repository, valkey, metrics,
        common::Environment::required("MINICLOUD_CLUSTER_TOKEN"), controller_epoch);
    controller::GrpcServer grpc_server(address_text(grpc_bind), grpc_service);

    common::HttpServer api_server(http_bind, [&](const common::HttpRequest& request) {
      return api.handle(request);
    });
    common::HttpServer metrics_server(metrics_bind, [&](const common::HttpRequest& request) {
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
    std::mutex stop_mutex;
    std::condition_variable stop_condition;
    auto guarded = [&](auto&& function) {
      try {
        function();
      } catch (...) {
        {
          std::lock_guard lock(error_mutex);
          if (!background_error) background_error = std::current_exception();
        }
        asynchronous_stop_requested.store(true, std::memory_order_release);
        stop_condition.notify_all();
      }
    };

    std::thread api_thread;
    std::thread metrics_thread;
    std::thread grpc_thread;
    std::thread reconciler_thread;
    const auto stop_and_join = [&] {
      asynchronous_stop_requested.store(true, std::memory_order_release);
      stop_condition.notify_all();
      grpc_server.stop();
      api_server.stop();
      metrics_server.stop();
      if (reconciler_thread.joinable()) reconciler_thread.join();
      if (grpc_thread.joinable()) grpc_thread.join();
      if (api_thread.joinable()) api_thread.join();
      if (metrics_thread.joinable()) metrics_thread.join();
    };
    try {
      api_thread = std::thread([&] { guarded([&] { api_server.start(); }); });
      metrics_thread = std::thread([&] { guarded([&] { metrics_server.start(); }); });
      grpc_thread = std::thread([&] { guarded([&] { grpc_server.run(); }); });
      reconciler_thread = std::thread([&] {
        guarded([&] {
          while (!should_stop()) {
            const auto result = repository.reconcile(heartbeat_timeout);
            metrics.increment("minicloud_controller_reconcile_total");
            metrics.gauge("minicloud_controller_unschedulable_allocations",
                          static_cast<double>(result.unschedulable));
            metrics.increment("minicloud_controller_allocations_scheduled_total",
                              static_cast<double>(result.scheduled));
            std::unique_lock lock(stop_mutex);
            stop_condition.wait_for(lock, reconcile_interval, should_stop);
          }
        });
      });
    } catch (...) {
      stop_and_join();
      throw;
    }

    std::cout << "{\"component\":\"controller\",\"event\":\"started\","
              << "\"http\":\"" << address_text(http_bind) << "\","
              << "\"grpc\":\"" << address_text(grpc_bind) << "\","
              << "\"metrics\":\"" << address_text(metrics_bind) << "\","
              << "\"controllerEpoch\":" << controller_epoch << "}" << std::endl;

    while (!should_stop()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_and_join();
    {
      std::lock_guard lock(error_mutex);
      if (background_error) std::rethrow_exception(background_error);
    }
    std::cout << "{\"component\":\"controller\",\"event\":\"stopped\"}" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "controller fatal error: " << error.what() << std::endl;
    return 1;
  }
}

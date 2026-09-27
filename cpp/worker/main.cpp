#include "minicloud/common/config.hpp"
#include "minicloud/common/http_server.hpp"
#include "minicloud/runtime/docker_client.hpp"
#include "minicloud/runtime/worker_runtime.hpp"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "controller_worker.grpc.pb.h"

namespace {
namespace protocol = minicloud::controller::worker::v1;
using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t signal_stop_requested = 0;
std::atomic<bool> asynchronous_stop_requested{false};

void request_signal_stop(int) noexcept { signal_stop_requested = 1; }

[[nodiscard]] bool stop_requested() noexcept {
  return signal_stop_requested != 0 ||
         asynchronous_stop_requested.load(std::memory_order_acquire);
}

void interruptible_sleep(std::chrono::milliseconds duration) {
  const auto deadline = Clock::now() + duration;
  while (!stop_requested() && Clock::now() < deadline) {
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds(100),
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     deadline - Clock::now())));
  }
}

[[nodiscard]] std::string default_docker_endpoint() {
#ifdef _WIN32
  return "npipe:////./pipe/docker_engine";
#else
  return "unix:///var/run/docker.sock";
#endif
}

struct WorkloadMetadata {
  std::string service_name;
  std::string allocation_id;
  std::uint32_t replica{};
  std::uint32_t container_port{};
  std::uint64_t generation{};
  std::uint64_t revision{};
  std::uint64_t controller_epoch{};
};

struct WorkerLogRecord {
  minicloud::runtime::DockerLogs::Stream stream{
      minicloud::runtime::DockerLogs::Stream::Stdout};
  std::string payload;
  std::string deduplication_key;
};

struct LogCursor {
  std::deque<std::string> recent_order;
  std::unordered_set<std::string> recent;
};

struct PendingLogBatch {
  std::string batch_id;
  std::vector<WorkerLogRecord> records;
};

class ControllerRpcError final : public std::runtime_error {
public:
  ControllerRpcError(grpc::StatusCode code, const std::string &message)
      : std::runtime_error(message), code_(code) {}

  [[nodiscard]] grpc::StatusCode code() const noexcept { return code_; }

private:
  grpc::StatusCode code_;
};

class ControllerEpochMismatch final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] std::chrono::milliseconds
bounded_duration(const google::protobuf::Duration &value,
                 std::chrono::milliseconds minimum,
                 std::chrono::milliseconds maximum, const char *field) {
  if (value.seconds() < 0 || value.nanos() < 0 ||
      value.nanos() >= 1'000'000'000) {
    throw std::invalid_argument(std::string(field) + " is invalid");
  }
  const auto max_seconds = maximum.count() / 1000;
  if (value.seconds() > max_seconds + 1) {
    throw std::invalid_argument(std::string(field) + " is outside safe bounds");
  }
  const auto milliseconds =
      std::chrono::milliseconds(value.seconds() * 1000) +
      std::chrono::milliseconds((value.nanos() + 999'999) / 1'000'000);
  if (milliseconds < minimum || milliseconds > maximum) {
    throw std::invalid_argument(std::string(field) + " is outside safe bounds");
  }
  return milliseconds;
}

class ControllerClient final {
public:
  ControllerClient(std::shared_ptr<grpc::Channel> channel, std::string token)
      : stub_(protocol::ControllerWorkerService::NewStub(std::move(channel))),
        token_(std::move(token)) {}

  protocol::RegisterWorkerResponse
  register_worker(const std::string &name, const std::string &instance,
                  const std::int64_t cpu_millis, const std::int64_t memory_mb,
                  const std::map<std::string, std::string> &labels) {
    protocol::RegisterWorkerRequest request;
    request.set_worker_id(name);
    request.set_instance_id(instance);
    request.mutable_protocol()->set_major(1);
    request.mutable_protocol()->set_minor(0);
    request.mutable_capacity()->set_cpu_millis(
        static_cast<std::uint64_t>(cpu_millis));
    request.mutable_capacity()->set_memory_bytes(
        static_cast<std::uint64_t>(memory_mb) * 1024ULL * 1024ULL);
    request.set_agent_version("0.1.0");
    for (const auto &[key, value] : labels)
      (*request.mutable_labels())[key] = value;
    protocol::RegisterWorkerResponse response;
    grpc::ClientContext context;
    prepare(context, std::chrono::seconds(5));
    check(stub_->RegisterWorker(&context, request, &response),
          "register worker");
    return response;
  }

  protocol::HeartbeatResponse
  heartbeat(const protocol::WorkerIdentity &identity,
            const std::uint64_t controller_epoch, const std::uint64_t sequence,
            const std::size_t workloads) {
    protocol::HeartbeatRequest request;
    *request.mutable_identity() = identity;
    request.set_sequence(sequence);
    request.set_known_controller_epoch(controller_epoch);
    request.mutable_usage()->set_running_workloads(
        static_cast<std::uint32_t>(std::min<std::size_t>(
            workloads, std::numeric_limits<std::uint32_t>::max())));
    protocol::HeartbeatResponse response;
    grpc::ClientContext context;
    prepare(context, std::chrono::seconds(4));
    check(stub_->Heartbeat(&context, request, &response), "heartbeat");
    return response;
  }

  protocol::PollCommandsResponse
  poll(const protocol::WorkerIdentity &identity,
       const std::uint64_t controller_epoch,
       const std::chrono::milliseconds wait_timeout) {
    protocol::PollCommandsRequest request;
    *request.mutable_identity() = identity;
    request.set_known_controller_epoch(controller_epoch);
    request.set_max_commands(16);
    request.mutable_wait_timeout()->set_seconds(wait_timeout.count() / 1000);
    request.mutable_wait_timeout()->set_nanos(
        static_cast<std::int32_t>((wait_timeout.count() % 1000) * 1'000'000));
    protocol::PollCommandsResponse response;
    grpc::ClientContext context;
    prepare(context, wait_timeout + std::chrono::seconds(2));
    check(stub_->PollCommands(&context, request, &response), "poll commands");
    return response;
  }

  protocol::CompleteCommandResponse::Disposition
  complete(const protocol::WorkerIdentity &identity,
           const protocol::LeasedCommand &leased,
           const minicloud::runtime::CommandResult &result) {
    protocol::CompleteCommandRequest request;
    *request.mutable_identity() = identity;
    request.set_command_id(leased.command().command_id());
    request.set_lease_token(leased.lease_token());
    request.set_controller_epoch(leased.command().controller_epoch());
    request.set_succeeded(result.succeeded);
    request.set_retryable(result.retryable);
    request.set_error_code(result.succeeded ? "" : "runtime_error");
    request.set_error_message(result.message.substr(0, 512));
    request.mutable_result()->set_runtime_id(result.container_id);
    protocol::CompleteCommandResponse response;
    grpc::ClientContext context;
    prepare(context, std::chrono::seconds(5));
    check(stub_->CompleteCommand(&context, request, &response),
          "complete command");
    return response.disposition();
  }

  protocol::ReportWorkloadStatusResponse
  report(const protocol::WorkerIdentity &identity,
         const std::vector<protocol::WorkloadStatusReport> &reports) {
    if (reports.empty())
      return {};
    protocol::ReportWorkloadStatusResponse combined;
    for (std::size_t offset = 0; offset < reports.size(); offset += 128) {
      protocol::ReportWorkloadStatusRequest request;
      *request.mutable_identity() = identity;
      const auto end = std::min(reports.size(), offset + 128);
      for (auto index = offset; index < end; ++index)
        *request.add_reports() = reports[index];
      protocol::ReportWorkloadStatusResponse response;
      grpc::ClientContext context;
      prepare(context, std::chrono::seconds(5));
      check(stub_->ReportWorkloadStatus(&context, request, &response),
            "report status");
      for (const auto &receipt : response.receipts())
        *combined.add_receipts() = receipt;
    }
    return combined;
  }

  protocol::LogIngestSummary logs(const protocol::WorkerIdentity &identity,
                                  const WorkloadMetadata &metadata,
                                  const std::string &batch_id,
                                  const std::vector<WorkerLogRecord> &records,
                                  const std::uint64_t first_sequence) {
    if (records.empty())
      return {};
    protocol::LogIngestSummary response;
    grpc::ClientContext context;
    prepare(context, std::chrono::seconds(10));
    auto writer = stub_->StreamLogs(&context, &response);
    protocol::LogBatch batch;
    *batch.mutable_identity() = identity;
    batch.mutable_ref()->set_workload_id(metadata.service_name);
    batch.mutable_ref()->set_allocation_id(metadata.allocation_id);
    batch.mutable_ref()->set_replica_index(metadata.replica);
    batch.mutable_ref()->set_workload_generation(metadata.generation);
    batch.mutable_ref()->set_allocation_revision(metadata.revision);
    batch.mutable_ref()->set_controller_epoch(metadata.controller_epoch);
    batch.set_batch_id(batch_id);
    std::uint64_t sequence = first_sequence;
    for (const auto &record_value : records) {
      auto *record = batch.add_records();
      record->set_sequence(sequence++);
      record->set_stream(record_value.stream ==
                                 minicloud::runtime::DockerLogs::Stream::Stdout
                             ? protocol::LOG_STREAM_STDOUT
                             : protocol::LOG_STREAM_STDERR);
      record->set_payload(record_value.payload.substr(0, 16 * 1024));
      record->set_truncated(record_value.payload.size() > 16 * 1024);
    }
    if (!writer->Write(batch))
      throw std::runtime_error("controller closed log stream");
    writer->WritesDone();
    check(writer->Finish(), "stream logs");
    return response;
  }

private:
  void prepare(grpc::ClientContext &context,
               const std::chrono::milliseconds timeout) const {
    context.AddMetadata("x-minicloud-cluster-token", token_);
    context.set_deadline(std::chrono::system_clock::now() + timeout);
  }
  static void check(const grpc::Status &status, const char *operation) {
    if (!status.ok()) {
      throw ControllerRpcError(status.error_code(),
                               std::string(operation) +
                                   " failed: " + status.error_message());
    }
  }
  std::unique_ptr<protocol::ControllerWorkerService::Stub> stub_;
  std::string token_;
};

protocol::RuntimeState
protocol_state(const minicloud::runtime::WorkloadState state) {
  using minicloud::runtime::WorkloadState;
  switch (state) {
  case WorkloadState::Starting:
  case WorkloadState::Backoff:
    return protocol::RUNTIME_STATE_STARTING;
  case WorkloadState::Healthy:
    return protocol::RUNTIME_STATE_RUNNING;
  case WorkloadState::Stopping:
    return protocol::RUNTIME_STATE_STOPPING;
  case WorkloadState::Stopped:
  case WorkloadState::Removed:
    return protocol::RUNTIME_STATE_STOPPED;
  case WorkloadState::Failed:
    return protocol::RUNTIME_STATE_FAILED;
  }
  return protocol::RUNTIME_STATE_UNSPECIFIED;
}

minicloud::runtime::RuntimeCommand
translate(const protocol::LeasedCommand &leased,
          std::map<std::string, WorkloadMetadata> &metadata,
          const std::uint64_t expected_controller_epoch,
          const std::string &workload_network) {
  using namespace minicloud::runtime;
  const auto &command = leased.command();
  if (command.controller_epoch() == 0 ||
      command.controller_epoch() != expected_controller_epoch) {
    throw ControllerEpochMismatch(
        "command controller epoch does not match the registration");
  }
  if (command.has_stop_workload()) {
    const auto &reference = command.stop_workload().ref();
    if (reference.controller_epoch() != expected_controller_epoch) {
      throw ControllerEpochMismatch(
          "stop workload reference has a stale controller epoch");
    }
    if (reference.allocation_id().empty() ||
        reference.allocation_revision() == 0) {
      throw std::invalid_argument("stop workload reference is incomplete");
    }
    const auto stop_timeout = bounded_duration(
        command.stop_workload().grace_period(), std::chrono::milliseconds(0),
        std::chrono::minutes(5), "stop grace period");
    return RuntimeCommand{command.command_id(),
                          RuntimeCommandKind::Remove,
                          reference.allocation_id(),
                          reference.allocation_revision(),
                          std::nullopt,
                          std::chrono::duration_cast<std::chrono::seconds>(
                              stop_timeout + std::chrono::milliseconds(999)),
                          expected_controller_epoch};
  }
  if (!command.has_ensure_workload())
    throw std::invalid_argument("command has no operation");
  const auto &desired = command.ensure_workload().workload();
  const auto &reference = desired.ref();
  if (reference.controller_epoch() != expected_controller_epoch) {
    throw ControllerEpochMismatch(
        "ensure workload reference has a stale controller epoch");
  }
  if (reference.allocation_id().empty() || reference.workload_id().empty() ||
      reference.allocation_revision() == 0 ||
      reference.workload_generation() == 0 ||
      desired.container().image_reference().empty() ||
      desired.container().ports_size() != 1) {
    throw std::invalid_argument("ensure workload is incomplete");
  }
  const auto &port = desired.container().ports(0);
  if (port.container_port() == 0 || port.container_port() > 65535 ||
      (port.protocol() != "tcp" && !port.protocol().empty())) {
    throw std::invalid_argument("workload port must be a valid TCP port");
  }
  constexpr std::uint64_t bytes_per_megabyte = 1024ULL * 1024ULL;
  constexpr std::uint64_t maximum_memory_megabytes = 1'048'576ULL;
  const auto memory_bytes = desired.resources().memory_bytes();
  if (desired.resources().cpu_millis() == 0 ||
      desired.resources().cpu_millis() > 1'000'000 ||
      memory_bytes < 16ULL * bytes_per_megabyte ||
      memory_bytes > maximum_memory_megabytes * bytes_per_megabyte) {
    throw std::invalid_argument(
        "workload resources are outside runtime bounds");
  }
  ContainerSpec container;
  container.image = desired.container().image_reference();
  container.entrypoint.assign(desired.container().command().begin(),
                              desired.container().command().end());
  container.command.assign(desired.container().arguments().begin(),
                           desired.container().arguments().end());
  if (desired.container().run_as_non_root()) {
    container.user = "65532:65532";
  }
  for (const auto &item : desired.container().environment()) {
    if (!item.has_value())
      throw std::invalid_argument(
          "secret providers are not configured in local mode");
    if (!container.environment.emplace(item.name(), item.value()).second) {
      throw std::invalid_argument(
          "workload contains duplicate environment names");
    }
  }
  if (container.environment.contains("PORT") ||
      container.environment.contains("MINICLOUD_ALLOCATION_ID")) {
    throw std::invalid_argument(
        "workload attempted to override reserved environment");
  }
  container.environment["PORT"] = std::to_string(port.container_port());
  container.environment["MINICLOUD_ALLOCATION_ID"] = reference.allocation_id();
  container.working_directory = desired.container().working_directory();
  container.cpu_millis =
      static_cast<std::int64_t>(desired.resources().cpu_millis());
  container.memory_mb = static_cast<std::int64_t>(
      (memory_bytes + bytes_per_megabyte - 1) / bytes_per_megabyte);
  container.pids_limit = 256;
  container.network_mode = workload_network;
  container.read_only_root_filesystem =
      desired.container().read_only_root_filesystem();
  container.drop_all_capabilities = true;
  container.no_new_privileges = true;
  const protocol::HealthCheck *health_check = nullptr;
  if (desired.has_readiness_check()) {
    health_check = &desired.readiness_check();
  } else if (desired.has_liveness_check()) {
    health_check = &desired.liveness_check();
  }
  if (health_check != nullptr) {
    if (health_check->kind() != protocol::HealthCheck::KIND_HTTP ||
        (!health_check->port_name().empty() &&
         health_check->port_name() != port.name()) ||
        health_check->path().empty() || health_check->path().front() != '/' ||
        health_check->path().size() > 2048 ||
        health_check->path().find_first_of("\r\n") != std::string::npos ||
        health_check->failure_threshold() == 0 ||
        health_check->failure_threshold() > 100) {
      throw std::invalid_argument(
          "workload health check is unsupported or invalid");
    }
    minicloud::runtime::DockerHealthConfig health;
    health.test_argv = {
        "wget", "-qO-",
        "http://127.0.0.1:" + std::to_string(port.container_port()) +
            health_check->path()};
    health.interval =
        bounded_duration(health_check->interval(), std::chrono::milliseconds(1),
                         std::chrono::hours(24), "health interval");
    health.timeout =
        bounded_duration(health_check->timeout(), std::chrono::milliseconds(1),
                         health.interval, "health timeout");
    health.retries = static_cast<int>(health_check->failure_threshold());
    container.health = std::move(health);
  }
  WorkloadSpec spec;
  spec.workload_id = reference.allocation_id();
  spec.revision = reference.allocation_revision();
  spec.controller_epoch = expected_controller_epoch;
  spec.container = std::move(container);
  const auto termination_grace = bounded_duration(
      desired.termination_grace_period(), std::chrono::milliseconds(0),
      std::chrono::minutes(5), "termination grace period");
  spec.stop_timeout = std::chrono::duration_cast<std::chrono::seconds>(
      termination_grace + std::chrono::milliseconds(999));
  metadata[reference.allocation_id()] = WorkloadMetadata{
      reference.workload_id(),         reference.allocation_id(),
      reference.replica_index(),       port.container_port(),
      reference.workload_generation(), reference.allocation_revision(),
      expected_controller_epoch};
  return RuntimeCommand{command.command_id(),
                        RuntimeCommandKind::Ensure,
                        reference.allocation_id(),
                        reference.allocation_revision(),
                        spec,
                        std::nullopt,
                        expected_controller_epoch};
}

std::vector<WorkerLogRecord>
collect_new_log_records(const minicloud::runtime::DockerLogs &captured,
                        const LogCursor &cursor) {
  std::vector<WorkerLogRecord> records;
  std::unordered_set<std::string> selected;
  auto append = [&](const minicloud::runtime::DockerLogs::Stream stream,
                    const std::string &text) {
    std::istringstream input(text);
    for (std::string line; records.size() < 128 && std::getline(input, line);) {
      if (line.empty())
        continue;
      const std::string key =
          std::string(stream == minicloud::runtime::DockerLogs::Stream::Stdout
                          ? "stdout\n"
                          : "stderr\n") +
          line;
      if (cursor.recent.contains(key) || !selected.emplace(key).second)
        continue;
      records.push_back({stream, std::move(line), key});
    }
  };
  if (!captured.records.empty()) {
    for (const auto &record : captured.records) {
      append(record.stream, record.text);
      if (records.size() == 128)
        break;
    }
  } else {
    append(minicloud::runtime::DockerLogs::Stream::Stdout,
           captured.stdout_text);
    append(minicloud::runtime::DockerLogs::Stream::Stderr,
           captured.stderr_text);
  }
  return records;
}

void commit_log_records(LogCursor &cursor,
                        const std::vector<WorkerLogRecord> &records) {
  constexpr std::size_t retained_keys = 4096;
  for (const auto &record : records) {
    if (!cursor.recent.emplace(record.deduplication_key).second)
      continue;
    cursor.recent_order.push_back(record.deduplication_key);
    while (cursor.recent_order.size() > retained_keys) {
      cursor.recent.erase(cursor.recent_order.front());
      cursor.recent_order.pop_front();
    }
  }
}

} // namespace

int main() {
  using namespace minicloud;
  try {
    if (std::signal(SIGINT, request_signal_stop) == SIG_ERR ||
        std::signal(SIGTERM, request_signal_stop) == SIG_ERR) {
      throw std::runtime_error("failed to install shutdown signal handlers");
    }
    const std::string node_name =
        common::Environment::required("MINICLOUD_NODE_NAME");
    const std::string instance_id = common::random_uuid();
    const auto cpu = common::Environment::integer("MINICLOUD_NODE_CPU_MILLIS",
                                                  4000, 100, 128000);
    const auto memory = common::Environment::integer("MINICLOUD_NODE_MEMORY_MB",
                                                     4096, 128, 1048576);
    const std::string workload_network = common::Environment::value(
        "MINICLOUD_WORKLOAD_NETWORK", "minicloud-workloads");
    auto labels = common::Environment::labels("MINICLOUD_NODE_LABELS");
    labels["os"] = "linux-containers";
    runtime::DockerClientOptions docker_options;
    docker_options.endpoint =
        runtime::DockerEndpoint::parse(common::Environment::value(
            "MINICLOUD_DOCKER_ENDPOINT", default_docker_endpoint()));
    docker_options.allow_remote_tcp =
        common::Environment::value("MINICLOUD_DOCKER_ALLOW_REMOTE_TCP",
                                   "false") == "true";
    auto docker =
        std::make_shared<runtime::DockerClient>(std::move(docker_options));
    ControllerClient controller(
        grpc::CreateChannel(common::Environment::value("MINICLOUD_GRPC_TARGET",
                                                       "127.0.0.1:50051"),
                            grpc::InsecureChannelCredentials()),
        common::Environment::required("MINICLOUD_CLUSTER_TOKEN"));
    protocol::WorkerIdentity identity;
    std::shared_ptr<runtime::WorkerRuntime> runtime_engine;
    std::mutex runtime_engine_mutex;
    std::string runtime_worker_id;
    std::uint64_t runtime_worker_epoch = 0;
    std::uint64_t controller_epoch = 0;
    std::chrono::milliseconds heartbeat_interval = std::chrono::seconds(3);
    std::chrono::milliseconds poll_timeout = std::chrono::seconds(2);
    bool draining = false;
    std::map<std::string, WorkloadMetadata> metadata;
    std::map<std::string, LogCursor> log_cursors;
    std::map<std::string, PendingLogBatch> pending_logs;
    std::map<std::string, std::uint64_t> log_sequences;
    std::vector<protocol::WorkloadStatusReport> pending_reports;
    std::uint64_t heartbeat_sequence = 0;
    auto next_heartbeat = Clock::now();
    auto next_report = Clock::now();
    auto next_logs = Clock::now();

    common::HttpServer metrics_server(
        common::Environment::bind("MINICLOUD_METRICS_BIND", "127.0.0.1:9101"),
        [&](const common::HttpRequest &request) {
          if (request.method() == common::http::verb::get &&
              request.target() == "/metrics") {
            std::shared_ptr<runtime::WorkerRuntime> current;
            {
              std::lock_guard<std::mutex> lock(runtime_engine_mutex);
              current = runtime_engine;
            }
            const std::string body =
                current ? current->prometheus_metrics() : "";
            return common::text_response(
                common::http::status::ok, body,
                "text/plain; version=0.0.4; charset=utf-8", request.version(),
                request.keep_alive());
          }
          return common::json_response(common::http::status::not_found,
                                       "{\"error\":\"route not found\"}",
                                       request.version(), request.keep_alive());
        },
        1024);
    std::exception_ptr metrics_error;
    std::thread metrics_thread([&] {
      try {
        metrics_server.start();
      } catch (...) {
        metrics_error = std::current_exception();
        asynchronous_stop_requested.store(true, std::memory_order_release);
      }
    });

    while (!stop_requested()) {
      try {
        std::shared_ptr<runtime::WorkerRuntime> current;
        {
          std::lock_guard<std::mutex> lock(runtime_engine_mutex);
          current = runtime_engine;
        }
        const auto now = Clock::now();
        if (current)
          current->tick(now);

        if (identity.worker_id().empty()) {
          const auto registration = controller.register_worker(
              node_name, instance_id, cpu, memory, labels);
          if (registration.identity().worker_id().empty() ||
              registration.identity().worker_epoch() == 0 ||
              registration.controller_epoch() == 0) {
            throw std::runtime_error(
                "controller returned an invalid worker registration");
          }
          heartbeat_interval = bounded_duration(
              registration.heartbeat_interval(), std::chrono::milliseconds(250),
              std::chrono::minutes(5), "heartbeat interval");
          poll_timeout = bounded_duration(registration.command_poll_timeout(),
                                          std::chrono::milliseconds(100),
                                          std::chrono::seconds(30),
                                          "command poll timeout");
          if (!current) {
            current = std::make_shared<runtime::WorkerRuntime>(
                registration.identity().worker_id(),
                registration.identity().worker_epoch(), docker);
            std::lock_guard<std::mutex> lock(runtime_engine_mutex);
            runtime_engine = current;
            runtime_worker_id = registration.identity().worker_id();
            runtime_worker_epoch = registration.identity().worker_epoch();
          } else {
            if (registration.identity().worker_id() != runtime_worker_id ||
                registration.identity().worker_epoch() < runtime_worker_epoch) {
              throw std::runtime_error(
                  "controller registration moved ownership backwards");
            }
            if (registration.identity().worker_epoch() > runtime_worker_epoch) {
              current->advance_worker_epoch(
                  registration.identity().worker_epoch(), now);
              runtime_worker_epoch = registration.identity().worker_epoch();
            }
          }
          identity = registration.identity();
          if (controller_epoch != 0 &&
              controller_epoch != registration.controller_epoch()) {
            pending_reports.clear();
            pending_logs.clear();
          }
          controller_epoch = registration.controller_epoch();
          for (auto &[ignored, workload] : metadata) {
            (void)ignored;
            workload.controller_epoch = controller_epoch;
          }
          heartbeat_sequence = 0;
          draining = false;
          next_heartbeat = now;
          std::cout << "{\"component\":\"worker\",\"event\":\"registered\","
                       "\"node\":\""
                    << node_name << "\",\"workerId\":\"" << identity.worker_id()
                    << "\",\"workerEpoch\":" << identity.worker_epoch()
                    << ",\"controllerEpoch\":" << controller_epoch << "}"
                    << std::endl;
        }
        if (!current)
          throw std::runtime_error("worker runtime is unavailable");
        if (now >= next_heartbeat) {
          const auto snapshots = current->snapshots();
          const auto running = static_cast<std::size_t>(std::count_if(
              snapshots.begin(), snapshots.end(), [](const auto &snapshot) {
                return snapshot.desired_running &&
                       snapshot.state != runtime::WorkloadState::Stopped &&
                       snapshot.state != runtime::WorkloadState::Removed;
              }));
          const auto response = controller.heartbeat(
              identity, controller_epoch, ++heartbeat_sequence, running);
          if (!response.accepted() ||
              response.controller_epoch() != controller_epoch ||
              response.directive() ==
                  protocol::HeartbeatResponse::DIRECTIVE_REREGISTER) {
            identity.Clear();
            continue;
          }
          draining = response.directive() ==
                     protocol::HeartbeatResponse::DIRECTIVE_DRAIN;
          if (response.has_next_heartbeat_after()) {
            heartbeat_interval = bounded_duration(
                response.next_heartbeat_after(), std::chrono::milliseconds(250),
                std::chrono::minutes(5), "next heartbeat interval");
          }
          next_heartbeat = now + heartbeat_interval;
        }
        protocol::PollCommandsResponse commands;
        if (!draining) {
          commands = controller.poll(identity, controller_epoch, poll_timeout);
          if (commands.controller_epoch() != controller_epoch) {
            identity.Clear();
            continue;
          }
          for (const auto &leased : commands.commands()) {
            runtime::CommandResult result;
            try {
              result = current->apply(translate(
                  leased, metadata, controller_epoch, workload_network));
            } catch (const ControllerEpochMismatch &) {
              identity.Clear();
              break;
            } catch (const std::exception &error) {
              const bool ensure = leased.command().has_ensure_workload();
              const auto &reference =
                  ensure ? leased.command().ensure_workload().workload().ref()
                         : leased.command().stop_workload().ref();
              result.command_id = leased.command().command_id();
              result.workload_id = reference.allocation_id();
              result.revision = reference.allocation_revision();
              result.state = runtime::WorkloadState::Failed;
              result.message = error.what();
              if (result.message.size() > 512)
                result.message.resize(512);
              std::replace(result.message.begin(), result.message.end(), '\r',
                           ' ');
              std::replace(result.message.begin(), result.message.end(), '\n',
                           ' ');
              result.retryable = false;
            }
            const auto disposition =
                controller.complete(identity, leased, result);
            if (disposition ==
                protocol::CompleteCommandResponse::DISPOSITION_FENCED) {
              identity.Clear();
              break;
            }
          }
        }
        if (identity.worker_id().empty())
          continue;
        if (now >= next_report) {
          if (pending_reports.empty()) {
            for (const auto &snapshot : current->snapshots()) {
              const auto found = metadata.find(snapshot.workload_id);
              if (found == metadata.end() ||
                  found->second.controller_epoch != controller_epoch)
                continue;
              auto report = protocol::WorkloadStatusReport{};
              report.set_report_id(common::random_uuid());
              report.mutable_ref()->set_workload_id(found->second.service_name);
              report.mutable_ref()->set_allocation_id(
                  found->second.allocation_id);
              report.mutable_ref()->set_replica_index(found->second.replica);
              report.mutable_ref()->set_workload_generation(
                  found->second.generation);
              report.mutable_ref()->set_allocation_revision(
                  found->second.revision);
              report.mutable_ref()->set_controller_epoch(controller_epoch);
              report.set_state(protocol_state(snapshot.state));
              report.set_ready(snapshot.state ==
                               runtime::WorkloadState::Healthy);
              report.set_restart_count(
                  static_cast<std::uint32_t>(std::min<std::size_t>(
                      snapshot.restart_count,
                      std::numeric_limits<std::uint32_t>::max())));
              report.set_runtime_id(snapshot.container_id);
              report.set_message(snapshot.last_error.substr(0, 512));
              if (snapshot.last_exit_code.has_value()) {
                report.set_last_exit_code(*snapshot.last_exit_code);
              }
              if (report.ready()) {
                auto *endpoint = report.mutable_endpoint();
                endpoint->set_host(runtime::WorkerRuntime::container_name_for(
                    snapshot.workload_id));
                endpoint->set_port(found->second.container_port);
                endpoint->set_scheme("http");
                endpoint->set_ready(true);
                endpoint->set_endpoint_revision(found->second.revision);
              }
              pending_reports.push_back(std::move(report));
            }
          }
          const auto status_response =
              controller.report(identity, pending_reports);
          pending_reports.clear();
          if (std::any_of(status_response.receipts().begin(),
                          status_response.receipts().end(),
                          [](const auto &receipt) {
                            return receipt.disposition() ==
                                   protocol::StatusReceipt::DISPOSITION_FENCED;
                          })) {
            identity.Clear();
            continue;
          }
          next_report = now + std::chrono::seconds(5);
        }
        if (now >= next_logs) {
          for (const auto &snapshot : current->snapshots()) {
            if (snapshot.container_id.empty())
              continue;
            const auto found = metadata.find(snapshot.workload_id);
            if (found == metadata.end() ||
                found->second.controller_epoch != controller_epoch)
              continue;
            try {
              auto &pending = pending_logs[snapshot.workload_id];
              if (pending.records.empty()) {
                const auto captured =
                    docker->logs(snapshot.container_id, 1000, true);
                pending.records = collect_new_log_records(
                    captured, log_cursors[snapshot.workload_id]);
                if (!pending.records.empty()) {
                  pending.batch_id = common::random_uuid();
                }
              }
              if (!pending.records.empty()) {
                const auto summary = controller.logs(
                    identity, found->second, pending.batch_id, pending.records,
                    log_sequences[snapshot.workload_id]);
                const auto expected =
                    static_cast<std::uint64_t>(pending.records.size());
                if (summary.rejected_records() != 0 ||
                    summary.accepted_records() > expected ||
                    summary.duplicate_records() > expected ||
                    summary.accepted_records() + summary.duplicate_records() !=
                        expected) {
                  throw std::runtime_error(
                      "controller did not acknowledge the complete log batch");
                }
                commit_log_records(log_cursors[snapshot.workload_id],
                                   pending.records);
                log_sequences[snapshot.workload_id] +=
                    static_cast<std::uint64_t>(pending.records.size());
                pending = {};
              }
            } catch (const std::exception &error) {
              // Logs are best effort and never block health or command
              // processing.
              std::cerr << "worker log upload: " << error.what() << std::endl;
            }
          }
          next_logs = now + std::chrono::seconds(3);
        }
        if (commands.commands().empty()) {
          interruptible_sleep(std::chrono::milliseconds(300));
        }
      } catch (const ControllerRpcError &error) {
        std::cerr << "worker control RPC: " << error.what() << std::endl;
        if (error.code() == grpc::StatusCode::UNAUTHENTICATED ||
            error.code() == grpc::StatusCode::PERMISSION_DENIED ||
            error.code() == grpc::StatusCode::FAILED_PRECONDITION) {
          identity.Clear();
        }
        interruptible_sleep(std::chrono::seconds(2));
      } catch (const std::exception &error) {
        std::cerr << "worker control loop: " << error.what() << std::endl;
        interruptible_sleep(std::chrono::seconds(2));
      }
    }
    metrics_server.stop();
    if (metrics_thread.joinable())
      metrics_thread.join();
    if (metrics_error)
      std::rethrow_exception(metrics_error);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "worker fatal error: " << error.what() << std::endl;
    return 1;
  }
}

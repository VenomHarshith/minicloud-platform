#include "minicloud/controller/grpc_service.hpp"

#include "minicloud/common/config.hpp"
#include "minicloud/common/metrics.hpp"
#include "minicloud/controller/repository.hpp"
#include "minicloud/controller/valkey_store.hpp"
#include "minicloud/core/domain.hpp"

#include <google/protobuf/duration.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minicloud::controller {
namespace protocol = worker::v1;
namespace {

constexpr std::uint32_t kProtocolMajor = 1;
constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumMemoryBytes = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumSignedEpoch =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

grpc::Status unauthenticated() {
  return {grpc::StatusCode::UNAUTHENTICATED, "valid cluster credential required"};
}

grpc::Status invalid(const std::string& message) {
  return {grpc::StatusCode::INVALID_ARGUMENT, message};
}

grpc::Status internal_error() {
  return {grpc::StatusCode::INTERNAL, "controller operation failed"};
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

bool safe_text(const std::string& value, const std::size_t maximum,
               const bool allow_empty = false) {
  if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character < 0x20U || character == 0x7FU) return false;
  }
  return true;
}

bool valid_host(const std::string& value) {
  if (value.empty() || value.size() > 253) return false;
  for (const char character : value) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') ||
                         character == '.' || character == '-' || character == '_' ||
                         character == ':';
    if (!allowed) return false;
  }
  return true;
}

bool valid_identity(const protocol::WorkerIdentity& identity) {
  return valid_uuid(identity.worker_id()) && valid_uuid(identity.instance_id()) &&
         identity.worker_epoch() > 0 && identity.worker_epoch() <= kMaximumSignedEpoch;
}

std::optional<std::string> state_name(const protocol::RuntimeState state) {
  switch (state) {
    case protocol::RUNTIME_STATE_PENDING: return "pending";
    case protocol::RUNTIME_STATE_STARTING: return "starting";
    case protocol::RUNTIME_STATE_RUNNING: return "running";
    case protocol::RUNTIME_STATE_STOPPING: return "stopping";
    case protocol::RUNTIME_STATE_STOPPED: return "stopped";
    case protocol::RUNTIME_STATE_FAILED: return "failed";
    case protocol::RUNTIME_STATE_UNSPECIFIED: return std::nullopt;
    default: return std::nullopt;
  }
}

void set_duration(google::protobuf::Duration* duration, const std::int64_t seconds) {
  duration->set_seconds(seconds);
  duration->set_nanos(0);
}

void set_now(google::protobuf::Timestamp* timestamp) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
  timestamp->set_seconds(seconds.count());
  timestamp->set_nanos(static_cast<std::int32_t>(nanos.count()));
}

void fill_identity(protocol::WorkerIdentity* identity, const std::string& worker_id,
                   const std::string& instance_id, const std::uint64_t epoch) {
  identity->set_worker_id(worker_id);
  identity->set_instance_id(instance_id);
  identity->set_worker_epoch(epoch);
}

std::string endpoint_url(const protocol::EndpointStatus& endpoint) {
  std::string host = endpoint.host();
  if (host.find(':') != std::string::npos) host = "[" + host + "]";
  return endpoint.scheme() + "://" + host + ":" + std::to_string(endpoint.port());
}

}  // namespace

ControllerGrpcService::ControllerGrpcService(
    Repository& repository, ValkeyStore& valkey, common::MetricsRegistry& metrics,
    std::string cluster_token, const std::uint64_t controller_epoch)
    : repository_(repository), valkey_(valkey), metrics_(metrics),
      cluster_token_(std::move(cluster_token)), controller_epoch_(controller_epoch) {
  if (cluster_token_.size() < 32) {
    throw std::invalid_argument("cluster token must have at least 32 characters");
  }
  if (controller_epoch_ == 0 || controller_epoch_ > kMaximumSignedEpoch) {
    throw std::invalid_argument("controller epoch is outside the supported range");
  }
}

bool ControllerGrpcService::authorized(const grpc::ServerContext& context) const {
  const auto found = context.client_metadata().find("x-minicloud-cluster-token");
  if (found == context.client_metadata().end()) return false;
  return common::constant_time_equal(
      std::string(found->second.data(), found->second.length()), cluster_token_);
}

grpc::Status ControllerGrpcService::RegisterWorker(
    grpc::ServerContext* context, const protocol::RegisterWorkerRequest* request,
    protocol::RegisterWorkerResponse* response) {
  if (!authorized(*context)) return unauthenticated();
  if (request->protocol().major() != kProtocolMajor ||
      !core::IsValidResourceId(request->worker_id()) ||
      !valid_uuid(request->instance_id()) ||
      request->capacity().cpu_millis() < 10 ||
      request->capacity().cpu_millis() > 128000 ||
      request->capacity().memory_bytes() < 16ULL * kMebibyte ||
      request->capacity().memory_bytes() > kMaximumMemoryBytes ||
      request->capacity().memory_bytes() % kMebibyte != 0 ||
      request->labels_size() > 64 || !safe_text(request->agent_version(), 128)) {
    return invalid("worker registration is outside protocol limits");
  }
  nlohmann::json labels = nlohmann::json::object();
  for (const auto& [key, value] : request->labels()) {
    if (!safe_text(key, 128) || !safe_text(value, 256)) {
      return invalid("worker labels contain an invalid key or value");
    }
    labels[key] = value;
  }
  try {
    const NodeRecord node = repository_.register_node(RegisterNodeInput{
        request->worker_id(), request->instance_id(),
        static_cast<std::int32_t>(request->capacity().cpu_millis()),
        static_cast<std::int32_t>(request->capacity().memory_bytes() / kMebibyte),
        labels, labels.value("docker.version", "unknown"), request->agent_version()});
    fill_identity(response->mutable_identity(), node.id, node.instance_id,
                  static_cast<std::uint64_t>(node.agent_epoch));
    response->set_controller_epoch(controller_epoch_);
    set_duration(response->mutable_heartbeat_interval(), 3);
    set_duration(response->mutable_command_poll_timeout(), 2);
    set_now(response->mutable_controller_time());
    metrics_.increment("minicloud_controller_worker_registrations_total");
    return grpc::Status::OK;
  } catch (...) {
    return internal_error();
  }
}

grpc::Status ControllerGrpcService::Heartbeat(
    grpc::ServerContext* context, const protocol::HeartbeatRequest* request,
    protocol::HeartbeatResponse* response) {
  if (!authorized(*context)) return unauthenticated();
  response->set_controller_epoch(controller_epoch_);
  if (!valid_identity(request->identity())) return invalid("worker identity is invalid");
  if (request->known_controller_epoch() != controller_epoch_) {
    response->set_accepted(false);
    response->set_directive(protocol::HeartbeatResponse::DIRECTIVE_REREGISTER);
    set_duration(response->mutable_next_heartbeat_after(), 1);
    return grpc::Status::OK;
  }
  try {
    const auto& identity = request->identity();
    const bool accepted = repository_.heartbeat(
        identity.worker_id(), identity.instance_id(),
        static_cast<std::int64_t>(identity.worker_epoch()));
    response->set_accepted(accepted);
    response->set_directive(accepted ? protocol::HeartbeatResponse::DIRECTIVE_CONTINUE
                                     : protocol::HeartbeatResponse::DIRECTIVE_REREGISTER);
    set_duration(response->mutable_next_heartbeat_after(), accepted ? 3 : 1);
    metrics_.increment("minicloud_controller_worker_heartbeats_total");
    return grpc::Status::OK;
  } catch (...) {
    return internal_error();
  }
}

grpc::Status ControllerGrpcService::PollCommands(
    grpc::ServerContext* context, const protocol::PollCommandsRequest* request,
    protocol::PollCommandsResponse* response) {
  if (!authorized(*context)) return unauthenticated();
  response->set_controller_epoch(controller_epoch_);
  if (!valid_identity(request->identity())) return invalid("worker identity is invalid");
  if (request->known_controller_epoch() != controller_epoch_) {
    return {grpc::StatusCode::FAILED_PRECONDITION,
            "controller epoch changed; register the worker again"};
  }
  try {
    const auto& identity = request->identity();
    const auto commands = repository_.claim_commands(
        identity.worker_id(), identity.instance_id(),
        static_cast<std::int64_t>(identity.worker_epoch()),
        std::clamp<std::size_t>(static_cast<std::size_t>(request->max_commands()), 1, 32), 15);
    for (const auto& command : commands) {
      auto* leased = response->add_commands();
      leased->set_lease_token(command.lease_token);
      leased->set_delivery_attempt(command.delivery_attempt);
      set_now(leased->mutable_lease_expires_at());
      leased->mutable_lease_expires_at()->set_seconds(
          leased->lease_expires_at().seconds() + 15);
      auto* encoded = leased->mutable_command();
      encoded->set_command_id(command.id);
      encoded->set_controller_epoch(controller_epoch_);
      set_now(encoded->mutable_created_at());
      if (command.kind == "stop") {
        auto* stop = encoded->mutable_stop_workload();
        auto* reference = stop->mutable_ref();
        reference->set_allocation_id(command.allocation_id);
        reference->set_allocation_revision(
            static_cast<std::uint64_t>(command.allocation_revision));
        reference->set_workload_generation(
            static_cast<std::uint64_t>(command.service_generation));
        reference->set_controller_epoch(controller_epoch_);
        set_duration(stop->mutable_grace_period(), 10);
        stop->set_reason("desired state changed");
        continue;
      }
      if (command.kind != "ensure") {
        throw std::runtime_error("repository returned an unsupported command kind");
      }
      const nlohmann::json& payload = command.payload;
      auto* workload = encoded->mutable_ensure_workload()->mutable_workload();
      auto* reference = workload->mutable_ref();
      reference->set_workload_id(payload.value("serviceName", ""));
      reference->set_allocation_id(command.allocation_id);
      reference->set_replica_index(
          static_cast<std::uint32_t>(payload.value("replica", 0)));
      reference->set_workload_generation(
          static_cast<std::uint64_t>(command.service_generation));
      reference->set_allocation_revision(
          static_cast<std::uint64_t>(command.allocation_revision));
      reference->set_controller_epoch(controller_epoch_);
      auto* container = workload->mutable_container();
      container->set_image_reference(payload.value("image", ""));
      container->set_read_only_root_filesystem(false);
      container->set_run_as_non_root(false);
      auto* port = container->add_ports();
      port->set_name("http");
      port->set_container_port(
          static_cast<std::uint32_t>(payload.value("containerPort", 8080)));
      port->set_protocol("tcp");
      if (payload.contains("environment") && payload["environment"].is_object()) {
        for (auto iterator = payload["environment"].begin();
             iterator != payload["environment"].end(); ++iterator) {
          auto* environment = container->add_environment();
          environment->set_name(iterator.key());
          environment->set_value(iterator.value().get<std::string>());
        }
      }
      workload->mutable_resources()->set_cpu_millis(
          static_cast<std::uint64_t>(payload.value("cpuMillis", 100)));
      workload->mutable_resources()->set_memory_bytes(
          static_cast<std::uint64_t>(payload.value("memoryMb", 128)) * kMebibyte);
      auto* readiness = workload->mutable_readiness_check();
      readiness->set_kind(protocol::HealthCheck::KIND_HTTP);
      readiness->set_path(payload.value("healthPath", "/health"));
      readiness->set_port_name("http");
      set_duration(readiness->mutable_interval(), 3);
      set_duration(readiness->mutable_timeout(), 2);
      readiness->set_success_threshold(1);
      readiness->set_failure_threshold(3);
      *workload->mutable_liveness_check() = *readiness;
      set_duration(workload->mutable_termination_grace_period(), 10);
    }
    if (!commands.empty()) {
      metrics_.increment("minicloud_controller_commands_leased_total",
                         static_cast<double>(commands.size()));
    }
    return grpc::Status::OK;
  } catch (...) {
    return internal_error();
  }
}

grpc::Status ControllerGrpcService::CompleteCommand(
    grpc::ServerContext* context, const protocol::CompleteCommandRequest* request,
    protocol::CompleteCommandResponse* response) {
  if (!authorized(*context)) return unauthenticated();
  if (!valid_identity(request->identity()) || !valid_uuid(request->command_id()) ||
      !valid_uuid(request->lease_token()) || !safe_text(request->error_code(), 128, true) ||
      !safe_text(request->error_message(), 2048, true)) {
    return invalid("command completion is outside protocol limits");
  }
  if (request->controller_epoch() != controller_epoch_) {
    response->set_disposition(protocol::CompleteCommandResponse::DISPOSITION_FENCED);
    return grpc::Status::OK;
  }
  try {
    const auto& identity = request->identity();
    std::string error;
    if (!request->succeeded()) {
      error = request->error_code();
      if (!error.empty() && !request->error_message().empty()) error += ": ";
      error += request->error_message();
    }
    const CommandCompletion completion = repository_.complete_command(
        identity.worker_id(), identity.instance_id(),
        static_cast<std::int64_t>(identity.worker_epoch()), request->command_id(),
        request->lease_token(), request->succeeded(), request->retryable(), error);
    switch (completion) {
      case CommandCompletion::kCommitted:
        response->set_disposition(protocol::CompleteCommandResponse::DISPOSITION_COMMITTED);
        break;
      case CommandCompletion::kAlreadyCommitted:
        response->set_disposition(
            protocol::CompleteCommandResponse::DISPOSITION_ALREADY_COMMITTED);
        break;
      case CommandCompletion::kStale:
        response->set_disposition(
            protocol::CompleteCommandResponse::DISPOSITION_STALE_LEASE);
        break;
    }
    metrics_.increment("minicloud_controller_commands_completed_total");
    return grpc::Status::OK;
  } catch (...) {
    return internal_error();
  }
}

grpc::Status ControllerGrpcService::ReportWorkloadStatus(
    grpc::ServerContext* context,
    const protocol::ReportWorkloadStatusRequest* request,
    protocol::ReportWorkloadStatusResponse* response) {
  if (!authorized(*context)) return unauthenticated();
  if (!valid_identity(request->identity()) || request->reports_size() > 128) {
    return invalid("status report batch is outside protocol limits");
  }
  for (const auto& report : request->reports()) {
    const auto& reference = report.ref();
    const auto state = state_name(report.state());
    if (!valid_uuid(report.report_id()) || !valid_uuid(reference.allocation_id()) ||
        !core::IsValidResourceId(reference.workload_id()) ||
        reference.allocation_revision() == 0 ||
        reference.allocation_revision() > kMaximumSignedEpoch ||
        reference.workload_generation() == 0 ||
        reference.workload_generation() > kMaximumSignedEpoch || !state ||
        report.restart_count() >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        !safe_text(report.runtime_id(), 128, true) ||
        !safe_text(report.message(), 2048, true)) {
      return invalid("a workload status report is outside protocol limits");
    }
    if (report.ready()) {
      if (!report.has_endpoint() || !report.endpoint().ready() ||
          (report.endpoint().scheme() != "http" && report.endpoint().scheme() != "https") ||
          !valid_host(report.endpoint().host()) || report.endpoint().port() == 0 ||
          report.endpoint().port() > 65535 ||
          endpoint_url(report.endpoint()).size() > 512) {
        return invalid("a ready workload report has an invalid endpoint");
      }
    }
  }
  try {
    const auto& identity = request->identity();
    for (const auto& report : request->reports()) {
      auto* receipt = response->add_receipts();
      receipt->set_report_id(report.report_id());
      const auto& reference = report.ref();
      if (reference.controller_epoch() != controller_epoch_) {
        receipt->set_disposition(protocol::StatusReceipt::DISPOSITION_FENCED);
        continue;
      }
      std::optional<std::string> endpoint;
      if (report.ready()) endpoint = endpoint_url(report.endpoint());
      const ObservationResult observation = repository_.observe(
          identity.worker_id(), identity.instance_id(),
          static_cast<std::int64_t>(identity.worker_epoch()),
          AllocationObservation{
              report.report_id(),
              reference.allocation_id(),
              static_cast<std::int64_t>(reference.allocation_revision()),
              static_cast<std::int64_t>(reference.workload_generation()),
              *state_name(report.state()),
              report.runtime_id().empty()
                  ? std::nullopt
                  : std::optional<std::string>(report.runtime_id()),
              endpoint, static_cast<std::int32_t>(report.restart_count()),
              report.message().empty()
                  ? std::nullopt
                  : std::optional<std::string>(report.message())});
      if (observation.service_name && endpoint) {
        valkey_.publish_endpoint(
            DiscoveredEndpoint{reference.allocation_id(),
                               *observation.service_name, *endpoint});
      } else if (observation.service_name) {
        valkey_.remove_endpoint(*observation.service_name, reference.allocation_id());
      }
      switch (observation.disposition) {
        case ObservationDisposition::kAccepted:
          receipt->set_disposition(protocol::StatusReceipt::DISPOSITION_ACCEPTED);
          break;
        case ObservationDisposition::kDuplicate:
          receipt->set_disposition(protocol::StatusReceipt::DISPOSITION_DUPLICATE);
          break;
        case ObservationDisposition::kStale:
          receipt->set_disposition(protocol::StatusReceipt::DISPOSITION_STALE_REVISION);
          break;
        case ObservationDisposition::kConflictingReportId:
          receipt->set_disposition(protocol::StatusReceipt::DISPOSITION_FENCED);
          break;
      }
    }
    metrics_.increment("minicloud_controller_status_reports_total",
                       static_cast<double>(request->reports_size()));
    return grpc::Status::OK;
  } catch (...) {
    return internal_error();
  }
}

grpc::Status ControllerGrpcService::StreamLogs(
    grpc::ServerContext* context, grpc::ServerReader<protocol::LogBatch>* reader,
    protocol::LogIngestSummary* response) {
  if (!authorized(*context)) return unauthenticated();
  try {
    protocol::LogBatch batch;
    while (reader->Read(&batch)) {
      const auto record_count = static_cast<std::uint64_t>(batch.records_size());
      const auto& identity = batch.identity();
      const auto& reference = batch.ref();
      if (!valid_identity(identity) || !valid_uuid(batch.batch_id()) ||
          !valid_uuid(reference.allocation_id()) ||
          reference.controller_epoch() != controller_epoch_ ||
          reference.allocation_revision() == 0 ||
          reference.allocation_revision() > kMaximumSignedEpoch ||
          reference.workload_generation() == 0 ||
          reference.workload_generation() > kMaximumSignedEpoch ||
          batch.records_size() == 0 || batch.records_size() > 128) {
        response->set_rejected_records(response->rejected_records() + record_count);
        continue;
      }
      const auto owner = repository_.owns_allocation(
          identity.worker_id(), identity.instance_id(),
          static_cast<std::int64_t>(identity.worker_epoch()),
          reference.allocation_id(),
          static_cast<std::int64_t>(reference.allocation_revision()),
          static_cast<std::int64_t>(reference.workload_generation()));
      if (!owner) {
        response->set_rejected_records(response->rejected_records() + record_count);
        continue;
      }
      bool ordered = true;
      std::uint64_t expected = batch.records(0).sequence();
      for (const auto& record : batch.records()) {
        if (record.sequence() != expected ||
            expected == std::numeric_limits<std::uint64_t>::max()) {
          ordered = false;
        } else {
          ++expected;
        }
      }
      if (!ordered) {
        response->set_rejected_records(response->rejected_records() + record_count);
        continue;
      }
      std::vector<std::string> lines;
      lines.reserve(static_cast<std::size_t>(batch.records_size()));
      for (const auto& record : batch.records()) {
        if (record.payload().size() > 16 * 1024 ||
            record.stream() == protocol::LOG_STREAM_UNSPECIFIED) {
          response->set_rejected_records(response->rejected_records() + 1);
          continue;
        }
        lines.push_back(record.payload());
      }
      const bool appended = valkey_.append_logs(
          reference.allocation_id(), batch.batch_id(), lines);
      const auto valid_count = static_cast<std::uint64_t>(lines.size());
      if (appended) {
        response->set_accepted_records(response->accepted_records() + valid_count);
      } else {
        response->set_duplicate_records(response->duplicate_records() + valid_count);
      }
      (*response->mutable_next_sequence_by_allocation())[reference.allocation_id()] = expected;
    }
    metrics_.increment("minicloud_controller_log_records_total",
                       static_cast<double>(response->accepted_records()));
    return grpc::Status::OK;
  } catch (...) {
    return internal_error();
  }
}

GrpcServer::GrpcServer(std::string address, ControllerGrpcService& service)
    : address_(std::move(address)), service_(service) {}

GrpcServer::~GrpcServer() { stop(); }

void GrpcServer::run() {
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(1024 * 1024);
  builder.SetMaxSendMessageSize(1024 * 1024);
  builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
  builder.RegisterService(&service_);
  auto server = builder.BuildAndStart();
  if (!server) throw std::runtime_error("failed to start controller gRPC server");
  {
    std::lock_guard lock(server_mutex_);
    server_ = server.get();
    if (stop_requested_.load()) {
      server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
    }
  }
  server->Wait();
  {
    std::lock_guard lock(server_mutex_);
    if (server_ == server.get()) server_ = nullptr;
  }
}

void GrpcServer::stop() {
  stop_requested_.store(true);
  std::lock_guard lock(server_mutex_);
  if (server_ != nullptr) {
    server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
  }
}

}  // namespace minicloud::controller

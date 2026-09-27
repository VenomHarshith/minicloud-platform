#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace minicloud::controller {

class ConflictError final : public std::runtime_error {
 public:
  explicit ConflictError(const std::string& message) : std::runtime_error(message) {}
};

struct ServiceInput {
  std::string name;
  std::string image;
  std::int32_t replicas{};
  std::int32_t cpu_millis{};
  std::int32_t memory_mb{};
  std::int32_t container_port{};
  std::string health_path;
  nlohmann::json environment{nlohmann::json::object()};
  nlohmann::json placement{nlohmann::json::object()};
};

struct ServiceRecord : ServiceInput {
  std::string id;
  std::int64_t generation{};
  std::string created_at;
  std::string updated_at;
};

struct NodeRecord {
  std::string id;
  std::string name;
  std::string instance_id;
  std::string status;
  std::int32_t cpu_capacity_millis{};
  std::int32_t cpu_allocated_millis{};
  std::int32_t memory_capacity_mb{};
  std::int32_t memory_allocated_mb{};
  nlohmann::json labels{nlohmann::json::object()};
  std::string docker_version;
  std::string agent_version;
  std::int64_t agent_epoch{};
  std::string last_heartbeat;
};

struct AllocationRecord {
  std::string id;
  std::string service_id;
  std::string service_name;
  std::int32_t replica{};
  std::optional<std::string> node_id;
  std::optional<std::string> node_name;
  std::string desired_state;
  std::string observed_state;
  std::int64_t service_generation{};
  std::int64_t revision{};
  std::optional<std::string> container_id;
  std::optional<std::string> endpoint;
  std::int32_t restart_count{};
  std::optional<std::string> last_error;
  std::string updated_at;
};

struct EventRecord {
  std::int64_t id{};
  std::string type;
  std::string aggregate_id;
  std::string message;
  nlohmann::json payload{nlohmann::json::object()};
  std::string created_at;
};

struct CommandRecord {
  std::string id;
  std::string allocation_id;
  std::string kind;
  nlohmann::json payload;
  std::int64_t service_generation{};
  std::int64_t allocation_revision{};
  std::string lease_token;
  std::uint32_t delivery_attempt{};
};

enum class CommandCompletion {
  kCommitted,
  kAlreadyCommitted,
  kStale,
};

struct ClusterSnapshot {
  std::vector<ServiceRecord> services;
  std::vector<NodeRecord> nodes;
  std::vector<AllocationRecord> allocations;
  std::vector<EventRecord> events;
};

struct RegisterNodeInput {
  std::string name;
  std::string instance_id;
  std::int32_t cpu_capacity_millis{};
  std::int32_t memory_capacity_mb{};
  nlohmann::json labels{nlohmann::json::object()};
  std::string docker_version;
  std::string agent_version;
};

struct AllocationObservation {
  std::string report_id;
  std::string allocation_id;
  std::int64_t allocation_revision{};
  std::int64_t service_generation{};
  std::string state;
  std::optional<std::string> container_id;
  std::optional<std::string> endpoint;
  std::int32_t restart_count{};
  std::optional<std::string> error;
};

enum class ObservationDisposition {
  kAccepted,
  kDuplicate,
  kStale,
  kConflictingReportId,
};

struct ObservationResult {
  ObservationDisposition disposition{ObservationDisposition::kStale};
  std::optional<std::string> service_name;
};

class Repository final {
 public:
  explicit Repository(std::string connection_string);
  ~Repository();
  Repository(const Repository&) = delete;
  Repository& operator=(const Repository&) = delete;

  void verify_schema();
  [[nodiscard]] std::int64_t acquire_controller_epoch();
  [[nodiscard]] ServiceRecord create_service(const ServiceInput& input);
  [[nodiscard]] ServiceRecord scale_service(const std::string& name, std::int32_t replicas);
  [[nodiscard]] ServiceRecord restart_service(const std::string& name);
  [[nodiscard]] bool delete_service(const std::string& name);
  [[nodiscard]] ClusterSnapshot snapshot(std::size_t event_limit = 50);

  [[nodiscard]] NodeRecord register_node(const RegisterNodeInput& input);
  [[nodiscard]] bool heartbeat(const std::string& node_id,
                               const std::string& instance_id,
                               std::int64_t epoch);
  [[nodiscard]] std::vector<CommandRecord> claim_commands(
      const std::string& node_id, const std::string& instance_id,
      std::int64_t epoch, std::size_t limit, std::int32_t lease_seconds);
  [[nodiscard]] CommandCompletion complete_command(
      const std::string& node_id, const std::string& instance_id,
      std::int64_t epoch, const std::string& command_id,
      const std::string& lease_token, bool success, bool retryable,
      const std::string& error);
  // Returns an idempotency disposition plus the canonical service name when the
  // exact allocation revision is still owned by this worker.
  [[nodiscard]] ObservationResult observe(
      const std::string& node_id, const std::string& instance_id,
      std::int64_t epoch, const AllocationObservation& observation);
  // Returns the canonical service name when this worker owns the exact revision.
  [[nodiscard]] std::optional<std::string> owns_allocation(
      const std::string& node_id, const std::string& instance_id,
      std::int64_t epoch, const std::string& allocation_id,
      std::int64_t allocation_revision, std::int64_t service_generation);

  struct ReconcileResult {
    std::size_t allocations_created{};
    std::size_t scheduled{};
    std::size_t updates{};
    std::size_t stops{};
    std::size_t nodes_expired{};
    std::size_t unschedulable{};
  };
  [[nodiscard]] ReconcileResult reconcile(std::int32_t heartbeat_timeout_seconds);

 private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

void to_json(nlohmann::json& json, const ServiceRecord& value);
void to_json(nlohmann::json& json, const NodeRecord& value);
void to_json(nlohmann::json& json, const AllocationRecord& value);
void to_json(nlohmann::json& json, const EventRecord& value);

}  // namespace minicloud::controller

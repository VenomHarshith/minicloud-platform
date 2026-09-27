#include "minicloud/controller/repository.hpp"

#include "minicloud/common/config.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace minicloud::controller {
namespace {

template <typename T>
std::optional<T> optional_field(const pqxx::field& field) {
  if (field.is_null()) return std::nullopt;
  return field.as<T>();
}

nlohmann::json json_field(const pqxx::field& field) {
  if (field.is_null()) return nlohmann::json::object();
  return nlohmann::json::parse(field.c_str());
}

ServiceRecord service_from(const pqxx::row& row) {
  return ServiceRecord{
      ServiceInput{row["name"].as<std::string>(), row["image"].as<std::string>(),
                   row["desired_replicas"].as<std::int32_t>(),
                   row["cpu_millis"].as<std::int32_t>(),
                   row["memory_mb"].as<std::int32_t>(),
                   row["container_port"].as<std::int32_t>(),
                   row["health_path"].as<std::string>(), json_field(row["environment"]),
                   json_field(row["placement"])},
      row["service_id"].as<std::string>(), row["generation"].as<std::int64_t>(),
      row["created_at"].as<std::string>(), row["updated_at"].as<std::string>()};
}

NodeRecord node_from(const pqxx::row& row) {
  return NodeRecord{
      row["node_id"].as<std::string>(), row["name"].as<std::string>(),
      row["instance_id"].as<std::string>(), row["status"].as<std::string>(),
      row["cpu_capacity_millis"].as<std::int32_t>(),
      row["cpu_allocated_millis"].as<std::int32_t>(0),
      row["memory_capacity_mb"].as<std::int32_t>(),
      row["memory_allocated_mb"].as<std::int32_t>(0), json_field(row["labels"]),
      row["docker_version"].as<std::string>(), row["agent_version"].as<std::string>(),
      row["agent_epoch"].as<std::int64_t>(), row["last_heartbeat"].as<std::string>()};
}

AllocationRecord allocation_from(const pqxx::row& row) {
  return AllocationRecord{
      row["allocation_id"].as<std::string>(), row["service_id"].as<std::string>(),
      row["service_name"].as<std::string>(), row["replica"].as<std::int32_t>(),
      optional_field<std::string>(row["node_id"]),
      optional_field<std::string>(row["node_name"]),
      row["desired_state"].as<std::string>(), row["observed_state"].as<std::string>(),
      row["service_generation"].as<std::int64_t>(),
      row["allocation_revision"].as<std::int64_t>(),
      optional_field<std::string>(row["container_id"]),
      optional_field<std::string>(row["endpoint"]),
      row["restart_count"].as<std::int32_t>(),
      optional_field<std::string>(row["last_error"]), row["updated_at"].as<std::string>()};
}

EventRecord event_from(const pqxx::row& row) {
  return EventRecord{row["event_id"].as<std::int64_t>(), row["event_type"].as<std::string>(),
                     row["aggregate_id"].as<std::string>(), row["message"].as<std::string>(),
                     json_field(row["payload"]), row["created_at"].as<std::string>()};
}

void append_event(pqxx::transaction_base& transaction, const std::string& type,
                  const std::string& aggregate, const std::string& message,
                  const nlohmann::json& payload = nlohmann::json::object()) {
  transaction.exec_params(
      "INSERT INTO events(event_type, aggregate_id, message, payload) VALUES($1,$2,$3,$4::jsonb)",
      type, aggregate, message, payload.dump());
}

nlohmann::json command_payload(const pqxx::row& service, const std::string& allocation_id,
                               const std::int32_t replica, const std::int64_t revision) {
  return {
      {"allocationId", allocation_id},
      {"serviceId", service["service_id"].as<std::string>()},
      {"serviceName", service["name"].as<std::string>()},
      {"replica", replica},
      {"image", service["image"].as<std::string>()},
      {"cpuMillis", service["cpu_millis"].as<std::int32_t>()},
      {"memoryMb", service["memory_mb"].as<std::int32_t>()},
      {"containerPort", service["container_port"].as<std::int32_t>()},
      {"healthPath", service["health_path"].as<std::string>()},
      {"environment", json_field(service["environment"])},
      {"serviceGeneration", service["generation"].as<std::int64_t>()},
      {"allocationRevision", revision},
      {"network", "minicloud-workloads"},
  };
}

bool labels_match(const nlohmann::json& required, const nlohmann::json& actual) {
  if (!required.is_object() || !actual.is_object()) return false;
  for (auto iterator = required.begin(); iterator != required.end(); ++iterator) {
    if (!iterator.value().is_string() || !actual.contains(iterator.key()) ||
        actual[iterator.key()] != iterator.value()) return false;
  }
  return true;
}

void supersede_active_commands(pqxx::transaction_base& transaction,
                               const std::string& allocation_id,
                               const std::string& reason) {
  transaction.exec_params(
      "UPDATE commands SET status='dead',last_error=$2,completed_at=now(),updated_at=now() "
      "WHERE allocation_id=$1 AND status IN ('pending','retry','leased')",
      allocation_id, reason.substr(0, 2048));
}

}  // namespace

class Repository::Implementation final {
 public:
  explicit Implementation(std::string connection_string)
      : connection_(std::move(connection_string)) {
    if (!connection_.is_open()) throw std::runtime_error("PostgreSQL connection did not open");
  }

  pqxx::connection connection_;
  std::mutex mutex_;
};

Repository::Repository(std::string connection_string)
    : implementation_(std::make_unique<Implementation>(std::move(connection_string))) {}
Repository::~Repository() = default;

void Repository::verify_schema() {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::read_transaction transaction(implementation_->connection_);
  const auto result = transaction.exec(
      "SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 1");
  if (result.empty() || result.front()[0].as<int>() < 3) {
    throw std::runtime_error("database schema is missing; run database/migrations first");
  }
}

std::int64_t Repository::acquire_controller_epoch() {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto lease_rows = transaction.exec(
      "SELECT pg_try_advisory_lock(1296646991,1) AS acquired");
  if (lease_rows.empty() || !lease_rows.front()["acquired"].as<bool>()) {
    throw std::runtime_error("another controller currently owns the control-plane lease");
  }
  const auto rows = transaction.exec(
      "SELECT nextval('controller_epoch_sequence') AS controller_epoch");
  if (rows.empty()) throw std::runtime_error("failed to acquire a controller epoch");
  const std::int64_t epoch = rows.front()["controller_epoch"].as<std::int64_t>();
  transaction.commit();
  if (epoch <= 0) throw std::runtime_error("database returned an invalid controller epoch");
  return epoch;
}

ServiceRecord Repository::create_service(const ServiceInput& input) {
  std::lock_guard lock(implementation_->mutex_);
  try {
    pqxx::work transaction(implementation_->connection_);
    const std::string id = common::random_uuid();
    const auto rows = transaction.exec_params(
        "INSERT INTO services(service_id,name,image,desired_replicas,cpu_millis,memory_mb,"
        "container_port,health_path,environment,placement) VALUES($1,$2,$3,$4,$5,$6,$7,$8,"
        "$9::jsonb,$10::jsonb) RETURNING *,created_at::text,updated_at::text",
        id, input.name, input.image, input.replicas, input.cpu_millis, input.memory_mb,
        input.container_port, input.health_path, input.environment.dump(), input.placement.dump());
    append_event(transaction, "service.created", id, "desired service was created",
                 {{"name", input.name}, {"image", input.image}, {"replicas", input.replicas}});
    transaction.commit();
    return service_from(rows.front());
  } catch (const pqxx::unique_violation&) {
    throw ConflictError("service name already exists");
  }
}

ServiceRecord Repository::scale_service(const std::string& name, const std::int32_t replicas) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto rows = transaction.exec_params(
      "UPDATE services SET desired_replicas=$2,updated_at=now() "
      "WHERE name=$1 RETURNING *,created_at::text,updated_at::text",
      name, replicas);
  if (rows.empty()) throw std::out_of_range("service not found");
  append_event(transaction, "service.scaled", rows.front()["service_id"].as<std::string>(),
               "desired replica count changed", {{"name", name}, {"replicas", replicas}});
  transaction.commit();
  return service_from(rows.front());
}

ServiceRecord Repository::restart_service(const std::string& name) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto rows = transaction.exec_params(
      "UPDATE services SET generation=generation+1,updated_at=now() WHERE name=$1 "
      "RETURNING *,created_at::text,updated_at::text", name);
  if (rows.empty()) throw std::out_of_range("service not found");
  append_event(transaction, "service.restarted", rows.front()["service_id"].as<std::string>(),
               "a service restart was requested", {{"name", name}});
  transaction.commit();
  return service_from(rows.front());
}

bool Repository::delete_service(const std::string& name) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto rows = transaction.exec_params(
      "UPDATE services SET desired_replicas=0,updated_at=now() "
      "WHERE name=$1 RETURNING service_id", name);
  if (rows.empty()) return false;
  append_event(transaction, "service.deleted", rows.front()[0].as<std::string>(),
               "service was marked for graceful removal", {{"name", name}});
  transaction.commit();
  return true;
}

ClusterSnapshot Repository::snapshot(const std::size_t event_limit) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::read_transaction transaction(implementation_->connection_);
  ClusterSnapshot snapshot;
  for (const auto& row : transaction.exec(
           "SELECT *,created_at::text,updated_at::text FROM services ORDER BY name")) {
    snapshot.services.push_back(service_from(row));
  }
  for (const auto& row : transaction.exec(
           "SELECT n.*,n.last_heartbeat::text,COALESCE(SUM(CASE WHEN a.desired_state='running' "
           "THEN s.cpu_millis ELSE 0 END),0)::int AS cpu_allocated_millis,"
           "COALESCE(SUM(CASE WHEN a.desired_state='running' THEN s.memory_mb ELSE 0 END),0)::int "
           "AS memory_allocated_mb FROM nodes n LEFT JOIN allocations a ON a.node_id=n.node_id "
           "LEFT JOIN services s ON s.service_id=a.service_id GROUP BY n.node_id ORDER BY n.name")) {
    snapshot.nodes.push_back(node_from(row));
  }
  for (const auto& row : transaction.exec(
           "SELECT a.*,s.name AS service_name,n.name AS node_name,a.updated_at::text FROM allocations a "
           "JOIN services s ON s.service_id=a.service_id LEFT JOIN nodes n ON n.node_id=a.node_id "
           "ORDER BY s.name,a.replica")) {
    snapshot.allocations.push_back(allocation_from(row));
  }
  const auto events = transaction.exec_params(
      "SELECT event_id,event_type,aggregate_id,message,payload,created_at::text FROM events "
      "ORDER BY event_id DESC LIMIT $1", static_cast<int>(std::min<std::size_t>(event_limit, 200)));
  for (const auto& row : events) snapshot.events.push_back(event_from(row));
  return snapshot;
}

NodeRecord Repository::register_node(const RegisterNodeInput& input) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto existing = transaction.exec_params(
      "SELECT node_id,instance_id,agent_epoch FROM nodes WHERE name=$1 FOR UPDATE", input.name);
  std::string node_id = common::random_uuid();
  std::int64_t epoch = 1;
  bool process_replaced = false;
  if (!existing.empty()) {
    node_id = existing.front()["node_id"].as<std::string>();
    epoch = existing.front()["agent_epoch"].as<std::int64_t>();
    if (existing.front()["instance_id"].as<std::string>() != input.instance_id) {
      if (epoch == std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("worker epoch is exhausted");
      }
      ++epoch;
      process_replaced = true;
    }
  }
  const auto rows = transaction.exec_params(
      "INSERT INTO nodes(node_id,name,instance_id,status,cpu_capacity_millis,memory_capacity_mb,"
      "labels,docker_version,agent_version,agent_epoch,last_heartbeat) "
      "VALUES($1,$2,$3,'ready',$4,$5,$6::jsonb,$7,$8,$9,now()) "
      "ON CONFLICT(name) DO UPDATE SET instance_id=excluded.instance_id,status='ready',"
      "cpu_capacity_millis=excluded.cpu_capacity_millis,memory_capacity_mb=excluded.memory_capacity_mb,"
      "labels=excluded.labels,docker_version=excluded.docker_version,agent_version=excluded.agent_version,"
      "agent_epoch=excluded.agent_epoch,last_heartbeat=now(),updated_at=now() "
      "RETURNING *,last_heartbeat::text,0::int AS cpu_allocated_millis,"
      "0::int AS memory_allocated_mb",
      node_id, input.name, input.instance_id, input.cpu_capacity_millis,
      input.memory_capacity_mb, input.labels.dump(), input.docker_version,
      input.agent_version, epoch);
  if (process_replaced) {
    const auto assigned = transaction.exec_params(
        "SELECT a.allocation_id,a.service_id,a.replica,a.desired_state,a.observed_state,"
        "a.allocation_revision,s.name,s.image,s.cpu_millis,s.memory_mb,s.container_port,"
        "s.health_path,s.environment,s.placement,s.generation FROM allocations a "
        "JOIN services s ON s.service_id=a.service_id WHERE a.node_id=$1 "
        "AND (a.desired_state='running' OR a.observed_state<>'stopped') "
        "ORDER BY s.created_at,a.replica FOR UPDATE OF a",
        node_id);
    for (const auto& allocation : assigned) {
      const std::string allocation_id = allocation["allocation_id"].as<std::string>();
      const std::int64_t revision =
          allocation["allocation_revision"].as<std::int64_t>() + 1;
      const std::int64_t generation = allocation["generation"].as<std::int64_t>();
      const bool ensure = allocation["desired_state"].as<std::string>() == "running";
      supersede_active_commands(transaction, allocation_id,
                                "worker process epoch was replaced");
      transaction.exec_params(
          "UPDATE allocations SET observed_state=$2,service_generation=$3,"
          "allocation_revision=$4,container_id=NULL,endpoint=NULL,"
          "last_error='worker process restarted; desired state will be replayed',updated_at=now() "
          "WHERE allocation_id=$1",
          allocation_id, ensure ? "pending" : "stopping", generation, revision);
      const std::string kind = ensure ? "ensure" : "stop";
      const std::string dedupe = allocation_id + ":" + std::to_string(revision) + ":" + kind;
      const nlohmann::json payload = ensure
          ? command_payload(allocation, allocation_id,
                            allocation["replica"].as<std::int32_t>(), revision)
          : nlohmann::json{{"allocationId", allocation_id},
                           {"allocationRevision", revision}};
      transaction.exec_params(
          "INSERT INTO commands(command_id,dedupe_key,allocation_id,node_id,kind,payload,"
          "service_generation,allocation_revision,status) "
          "VALUES($1,$2,$3,$4,$5,$6::jsonb,$7,$8,'pending') "
          "ON CONFLICT(dedupe_key) DO NOTHING",
          common::random_uuid(), dedupe, allocation_id, node_id, kind, payload.dump(),
          generation, revision);
    }
  }
  append_event(transaction, "node.registered", node_id, "worker registered with the controller",
               {{"name", input.name}, {"epoch", epoch},
                {"processReplaced", process_replaced}});
  transaction.commit();
  return node_from(rows.front());
}

bool Repository::heartbeat(const std::string& node_id, const std::string& instance_id,
                           const std::int64_t epoch) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto changed = transaction.exec_params(
      "UPDATE nodes SET status='ready',last_heartbeat=now(),updated_at=now() "
      "WHERE node_id=$1 AND instance_id=$2 AND agent_epoch=$3 RETURNING node_id",
      node_id, instance_id, epoch);
  transaction.commit();
  return !changed.empty();
}

std::vector<CommandRecord> Repository::claim_commands(
    const std::string& node_id, const std::string& instance_id, const std::int64_t epoch,
    const std::size_t limit, const std::int32_t lease_seconds) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto valid = transaction.exec_params(
      "SELECT 1 FROM nodes WHERE node_id=$1 AND instance_id=$2 AND agent_epoch=$3 AND status='ready'",
      node_id, instance_id, epoch);
  if (valid.empty()) throw std::runtime_error("worker registration is stale");
  transaction.exec_params(
      "UPDATE commands c SET status='dead',last_error='command was superseded',"
      "completed_at=now(),updated_at=now() FROM allocations a WHERE c.node_id=$1 "
      "AND c.allocation_id=a.allocation_id AND c.status IN ('pending','retry','leased') "
      "AND (c.service_generation<>a.service_generation "
      "OR c.allocation_revision<>a.allocation_revision "
      "OR (c.kind='ensure' AND a.desired_state<>'running') "
      "OR (c.kind='stop' AND a.desired_state<>'stopped'))",
      node_id);
  transaction.exec_params(
      "UPDATE commands SET status='dead',last_error='delivery attempts exhausted',"
      "completed_at=now(),updated_at=now() WHERE node_id=$1 AND attempt_count>=max_attempts "
      "AND (status IN ('pending','retry') OR "
      "(status='leased' AND lease_expires_at<now()))",
      node_id);
  const std::string lease_token = common::random_uuid();
  const auto rows = transaction.exec_params(
      "WITH candidates AS (SELECT command_id FROM commands WHERE node_id=$1 AND "
      "attempt_count<max_attempts AND ((status IN ('pending','retry') AND available_at<=now()) OR "
      "(status='leased' AND lease_expires_at<now())) ORDER BY created_at FOR UPDATE SKIP LOCKED LIMIT $2) "
      "UPDATE commands c SET status='leased',lease_owner=$3::uuid,lease_token=$4::uuid,"
      "lease_expires_at=now()+make_interval(secs=>$5),attempt_count=attempt_count+1,updated_at=now() "
      "FROM candidates WHERE c.command_id=candidates.command_id RETURNING c.command_id,c.allocation_id,"
      "c.kind,c.payload,c.service_generation,c.allocation_revision,c.lease_token,c.attempt_count",
      node_id, static_cast<int>(std::min<std::size_t>(limit, 32)), instance_id,
      lease_token, lease_seconds);
  std::vector<CommandRecord> commands;
  commands.reserve(static_cast<std::size_t>(rows.size()));
  for (const auto& row : rows) {
    commands.push_back(CommandRecord{
        row["command_id"].as<std::string>(), row["allocation_id"].as<std::string>(),
        row["kind"].as<std::string>(), json_field(row["payload"]),
        row["service_generation"].as<std::int64_t>(),
        row["allocation_revision"].as<std::int64_t>(),
        row["lease_token"].as<std::string>(),
        row["attempt_count"].as<std::uint32_t>()});
  }
  transaction.commit();
  return commands;
}

CommandCompletion Repository::complete_command(
    const std::string& node_id, const std::string& instance_id,
    const std::int64_t epoch, const std::string& command_id,
    const std::string& lease_token, const bool success, const bool retryable,
    const std::string& error) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  const auto valid = transaction.exec_params(
      "SELECT 1 FROM nodes WHERE node_id=$1 AND instance_id=$2 AND agent_epoch=$3 "
      "AND status='ready'",
      node_id, instance_id, epoch);
  if (valid.empty()) return CommandCompletion::kStale;
  const auto existing = transaction.exec_params(
      "SELECT status,lease_owner::text,lease_token::text FROM commands "
      "WHERE command_id=$1 AND node_id=$2 FOR UPDATE",
      command_id, node_id);
  if (existing.empty() || existing.front()["lease_owner"].is_null() ||
      existing.front()["lease_token"].is_null() ||
      existing.front()["lease_owner"].as<std::string>() != instance_id ||
      existing.front()["lease_token"].as<std::string>() != lease_token) {
    transaction.commit();
    return CommandCompletion::kStale;
  }
  if (existing.front()["status"].as<std::string>() != "leased") {
    transaction.commit();
    return CommandCompletion::kAlreadyCommitted;
  }
  const std::string status = success ? "succeeded" : (retryable ? "retry" : "dead");
  const auto rows = transaction.exec_params(
      "UPDATE commands SET status=$4,last_error=NULLIF($5,''),"
      "available_at=CASE WHEN $4='retry' THEN now()+LEAST(interval '30 seconds',"
      "make_interval(secs=>power(2,LEAST(attempt_count,5))::int)) ELSE available_at END,"
      "completed_at=CASE WHEN $4 IN ('succeeded','dead') THEN now() ELSE NULL END,updated_at=now() "
      "WHERE command_id=$1 AND node_id=$2 AND status='leased' AND lease_token=$3::uuid "
      "AND lease_owner=$6::uuid RETURNING allocation_id,kind,service_generation,allocation_revision",
      command_id, node_id, lease_token, status, error.substr(0, 2048), instance_id);
  if (!rows.empty()) {
    if (success && rows.front()["kind"].as<std::string>() == "stop") {
      transaction.exec_params(
          "UPDATE allocations SET observed_state='stopped',node_id=NULL,container_id=NULL,endpoint=NULL,"
          "updated_at=now() WHERE allocation_id=$1 AND service_generation=$2 "
          "AND allocation_revision=$3 AND desired_state='stopped'",
          rows.front()["allocation_id"].as<std::string>(),
          rows.front()["service_generation"].as<std::int64_t>(),
          rows.front()["allocation_revision"].as<std::int64_t>());
    }
    append_event(transaction, "command." + status, command_id,
                 success ? "worker applied a command" : "worker reported a command failure",
                 {{"error", error.substr(0, 512)}});
  }
  transaction.commit();
  return rows.empty() ? CommandCompletion::kStale : CommandCompletion::kCommitted;
}

ObservationResult Repository::observe(
    const std::string& node_id, const std::string& instance_id,
    const std::int64_t epoch, const AllocationObservation& observation) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  transaction.exec("DELETE FROM status_report_receipts WHERE expires_at<now()");
  const std::string fingerprint = nlohmann::json{
      {"nodeId", node_id}, {"instanceId", instance_id}, {"workerEpoch", epoch},
      {"allocationId", observation.allocation_id},
      {"allocationRevision", observation.allocation_revision},
      {"serviceGeneration", observation.service_generation},
      {"state", observation.state},
      {"containerId", observation.container_id.value_or("")},
      {"endpoint", observation.endpoint.value_or("")},
      {"restartCount", observation.restart_count},
      {"error", observation.error.value_or("")}}
                                      .dump();
  const auto inserted = transaction.exec_params(
      "INSERT INTO status_report_receipts(report_id,fingerprint,disposition) "
      "VALUES($1::uuid,$2,'processing') ON CONFLICT(report_id) DO NOTHING "
      "RETURNING report_id",
      observation.report_id, fingerprint);
  if (inserted.empty()) {
    const auto prior = transaction.exec_params(
        "SELECT fingerprint,disposition,service_name FROM status_report_receipts "
        "WHERE report_id=$1::uuid",
        observation.report_id);
    if (prior.empty() || prior.front()["fingerprint"].as<std::string>() != fingerprint) {
      transaction.commit();
      return {ObservationDisposition::kConflictingReportId, std::nullopt};
    }
    std::optional<std::string> service_name;
    if (prior.front()["disposition"].as<std::string>() == "accepted" &&
        !prior.front()["service_name"].is_null()) {
      const auto still_owned = transaction.exec_params(
          "SELECT 1 FROM allocations a JOIN nodes n ON n.node_id=a.node_id "
          "WHERE a.allocation_id=$1 AND a.node_id=$2 AND n.instance_id=$3 "
          "AND n.agent_epoch=$4 AND n.status='ready' "
          "AND a.allocation_revision=$5 AND a.service_generation=$6",
          observation.allocation_id, node_id, instance_id, epoch,
          observation.allocation_revision, observation.service_generation);
      if (!still_owned.empty()) {
        service_name = prior.front()["service_name"].as<std::string>();
      }
    }
    transaction.commit();
    return {ObservationDisposition::kDuplicate, std::move(service_name)};
  }
  const auto rows = transaction.exec_params(
      "UPDATE allocations a SET observed_state=$5,container_id=NULLIF($6,''),endpoint=NULLIF($7,''),"
      "restart_count=$8,last_error=NULLIF($9,''),updated_at=now() FROM nodes n,services s "
      "WHERE a.allocation_id=$1 AND a.node_id=$2 AND n.node_id=$2 AND n.instance_id=$3 "
      "AND n.agent_epoch=$4 AND n.status='ready' AND s.service_id=a.service_id "
      "AND a.allocation_revision=$10 AND a.service_generation=$11 "
      "RETURNING s.name AS service_name",
      observation.allocation_id, node_id, instance_id, epoch, observation.state,
      observation.container_id.value_or(""), observation.endpoint.value_or(""),
      observation.restart_count, observation.error.value_or(""),
      observation.allocation_revision, observation.service_generation);
  if (!rows.empty()) {
    append_event(transaction, "allocation.observed", observation.allocation_id,
                 "worker reported allocation state",
                 {{"state", observation.state}, {"restartCount", observation.restart_count}});
  }
  const std::optional<std::string> service_name = rows.empty()
      ? std::nullopt
      : std::optional<std::string>(rows.front()["service_name"].as<std::string>());
  transaction.exec_params(
      "UPDATE status_report_receipts SET disposition=$2,service_name=$3,expires_at=now()+interval '24 hours' "
      "WHERE report_id=$1::uuid",
      observation.report_id, rows.empty() ? "stale" : "accepted",
      service_name.value_or(""));
  transaction.commit();
  return {rows.empty() ? ObservationDisposition::kStale
                       : ObservationDisposition::kAccepted,
          service_name};
}

std::optional<std::string> Repository::owns_allocation(
    const std::string& node_id, const std::string& instance_id,
    const std::int64_t epoch, const std::string& allocation_id,
    const std::int64_t allocation_revision, const std::int64_t service_generation) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::read_transaction transaction(implementation_->connection_);
  const auto rows = transaction.exec_params(
      "SELECT s.name AS service_name FROM allocations a "
      "JOIN services s ON s.service_id=a.service_id "
      "JOIN nodes n ON n.node_id=a.node_id WHERE a.allocation_id=$1 AND a.node_id=$2 "
      "AND n.instance_id=$3 AND n.agent_epoch=$4 AND n.status='ready' "
      "AND a.allocation_revision=$5 AND a.service_generation=$6",
      allocation_id, node_id, instance_id, epoch, allocation_revision,
      service_generation);
  if (rows.empty()) return std::nullopt;
  return rows.front()["service_name"].as<std::string>();
}

Repository::ReconcileResult Repository::reconcile(const std::int32_t heartbeat_timeout_seconds) {
  std::lock_guard lock(implementation_->mutex_);
  pqxx::work transaction(implementation_->connection_);
  ReconcileResult result;

  const auto expired = transaction.exec_params(
      "UPDATE nodes SET status='not_ready',updated_at=now() WHERE status='ready' AND "
      "last_heartbeat < now()-make_interval(secs=>$1) RETURNING node_id,name",
      heartbeat_timeout_seconds);
  result.nodes_expired = static_cast<std::size_t>(expired.size());
  for (const auto& node : expired) {
    append_event(transaction, "node.expired", node["node_id"].as<std::string>(),
                 "worker heartbeat expired", {{"name", node["name"].as<std::string>()}});
  }
  if (!expired.empty()) {
    transaction.exec(
        "UPDATE commands c SET status='dead',last_error='assigned worker heartbeat expired',"
        "completed_at=now(),updated_at=now() FROM nodes n WHERE c.node_id=n.node_id "
        "AND n.status='not_ready' AND c.status IN ('pending','retry','leased')");
    transaction.exec(
        "UPDATE allocations a SET node_id=NULL,observed_state='lost',"
        "allocation_revision=allocation_revision+1,"
        "container_id=NULL,endpoint=NULL,"
        "last_error='assigned worker heartbeat expired',updated_at=now() "
        "FROM nodes n WHERE a.node_id=n.node_id AND n.status='not_ready' "
        "AND a.desired_state='running'");
  }

  const auto services = transaction.exec(
      "SELECT * FROM services ORDER BY created_at,service_id FOR UPDATE");
  for (const auto& service : services) {
    const std::string service_id = service["service_id"].as<std::string>();
    const std::int32_t desired = service["desired_replicas"].as<std::int32_t>();
    const auto reactivated = transaction.exec_params(
        "UPDATE allocations SET desired_state='running',observed_state='pending',node_id=NULL,"
        "service_generation=$3,allocation_revision=allocation_revision+1,container_id=NULL,"
        "endpoint=NULL,last_error=NULL,updated_at=now() WHERE service_id=$1 AND replica<$2 "
        "AND desired_state='stopped' RETURNING allocation_id,replica",
        service_id, desired, service["generation"].as<std::int64_t>());
    for (const auto& allocation : reactivated) {
      const std::string allocation_id = allocation["allocation_id"].as<std::string>();
      supersede_active_commands(transaction, allocation_id, "allocation was reactivated");
      ++result.updates;
      append_event(transaction, "allocation.reactivated", allocation_id,
                   "a scaled-down replica was reactivated",
                   {{"replica", allocation["replica"].as<std::int32_t>()}});
    }
    for (std::int32_t replica = 0; replica < desired; ++replica) {
      const std::string allocation_id = common::random_uuid();
      const auto inserted = transaction.exec_params(
          "INSERT INTO allocations(allocation_id,service_id,replica,desired_state,observed_state,"
          "service_generation) VALUES($1,$2,$3,'running','pending',$4) "
          "ON CONFLICT(service_id,replica) DO NOTHING RETURNING allocation_id",
          allocation_id, service_id, replica, service["generation"].as<std::int64_t>());
      if (!inserted.empty()) {
        ++result.allocations_created;
        append_event(transaction, "allocation.created", allocation_id,
                     "reconciler created a missing replica", {{"replica", replica}});
      }
    }
    const auto scaled_down = transaction.exec_params(
        "UPDATE allocations SET desired_state='stopped',allocation_revision=allocation_revision+1,"
        "observed_state=CASE WHEN node_id IS NULL THEN 'stopped' ELSE 'stopping' END,"
        "container_id=CASE WHEN node_id IS NULL THEN NULL ELSE container_id END,"
        "endpoint=CASE WHEN node_id IS NULL THEN NULL ELSE endpoint END,"
        "updated_at=now() WHERE service_id=$1 AND replica >= $2 AND desired_state='running' "
        "RETURNING allocation_id,node_id,allocation_revision,service_generation,replica",
        service_id, desired);
    for (const auto& allocation : scaled_down) {
      const std::string allocation_id = allocation["allocation_id"].as<std::string>();
      supersede_active_commands(transaction, allocation_id, "allocation was scaled down");
      if (!allocation["node_id"].is_null()) {
        const std::int64_t revision = allocation["allocation_revision"].as<std::int64_t>();
        const std::string dedupe = allocation_id + ":" + std::to_string(revision) + ":stop";
        const auto created = transaction.exec_params(
            "INSERT INTO commands(command_id,dedupe_key,allocation_id,node_id,kind,payload,"
            "service_generation,allocation_revision,status) VALUES($1,$2,$3,$4,'stop',$5::jsonb,$6,$7,'pending') "
            "ON CONFLICT(dedupe_key) DO NOTHING RETURNING command_id",
            common::random_uuid(), dedupe, allocation_id,
            allocation["node_id"].as<std::string>(),
            nlohmann::json({{"allocationId", allocation_id}, {"allocationRevision", revision}}).dump(),
            allocation["service_generation"].as<std::int64_t>(), revision);
        if (!created.empty()) ++result.stops;
      }
    }
  }

  // A generation change or a terminal runtime failure is repaired on the same
  // ready worker. Moving it without first stopping the old runtime can orphan a
  // container; only loss of worker ownership makes an allocation unassigned.
  const auto repairs = transaction.exec(
      "SELECT a.allocation_id,a.service_id,a.replica,a.node_id,a.observed_state,"
      "a.service_generation,a.allocation_revision,s.name,s.image,s.cpu_millis,s.memory_mb,"
      "s.container_port,s.health_path,s.environment,s.placement,s.generation,n.name AS node_name "
      "FROM allocations a JOIN services s ON s.service_id=a.service_id "
      "JOIN nodes n ON n.node_id=a.node_id WHERE a.desired_state='running' "
      "AND n.status='ready' AND (a.service_generation<>s.generation "
      "OR a.observed_state IN ('failed','unhealthy','stopped','lost')) "
      "ORDER BY s.created_at,a.replica FOR UPDATE OF a");
  for (const auto& allocation : repairs) {
    const std::string allocation_id = allocation["allocation_id"].as<std::string>();
    const std::int64_t revision = allocation["allocation_revision"].as<std::int64_t>() + 1;
    const std::int64_t generation = allocation["generation"].as<std::int64_t>();
    supersede_active_commands(transaction, allocation_id, "allocation revision was superseded");
    transaction.exec_params(
        "UPDATE allocations SET observed_state='pending',service_generation=$2,"
        "allocation_revision=$3,container_id=NULL,endpoint=NULL,last_error=NULL,updated_at=now() "
        "WHERE allocation_id=$1",
        allocation_id, generation, revision);
    const std::string dedupe = allocation_id + ":" + std::to_string(revision) + ":ensure";
    const auto created = transaction.exec_params(
        "INSERT INTO commands(command_id,dedupe_key,allocation_id,node_id,kind,payload,"
        "service_generation,allocation_revision,status) VALUES($1,$2,$3,$4,'ensure',$5::jsonb,$6,$7,'pending') "
        "ON CONFLICT(dedupe_key) DO NOTHING RETURNING command_id",
        common::random_uuid(), dedupe, allocation_id,
        allocation["node_id"].as<std::string>(),
        command_payload(allocation, allocation_id,
                        allocation["replica"].as<std::int32_t>(), revision).dump(),
        generation, revision);
    if (!created.empty()) {
      ++result.scheduled;
      ++result.updates;
      append_event(transaction, "allocation.updated", allocation_id,
                   "the assigned worker will reconcile a new allocation revision",
                   {{"node", allocation["node_name"].as<std::string>()},
                    {"revision", revision}});
    }
  }

  struct Capacity {
    std::string id;
    std::string name;
    std::int64_t cpu_capacity{};
    std::int64_t memory_capacity{};
    std::int64_t cpu_free{};
    std::int64_t memory_free{};
    nlohmann::json labels;
    std::map<std::string, std::uint64_t> anti_affinity_counts;
  };
  std::vector<Capacity> nodes;
  for (const auto& row : transaction.exec(
           "SELECT n.node_id,n.name,n.cpu_capacity_millis,n.memory_capacity_mb,"
           "n.cpu_capacity_millis-COALESCE(SUM(CASE WHEN a.desired_state='running' "
           "THEN s.cpu_millis ELSE 0 END),0) AS cpu_free,n.memory_capacity_mb-COALESCE(SUM(CASE "
           "WHEN a.desired_state='running' THEN s.memory_mb ELSE 0 END),0) AS memory_free,n.labels "
           "FROM nodes n LEFT JOIN allocations a ON a.node_id=n.node_id LEFT JOIN services s ON "
           "s.service_id=a.service_id WHERE n.status='ready' GROUP BY n.node_id ORDER BY n.name")) {
    nodes.push_back(Capacity{row["node_id"].as<std::string>(), row["name"].as<std::string>(),
                             row["cpu_capacity_millis"].as<std::int64_t>(),
                             row["memory_capacity_mb"].as<std::int64_t>(),
                             row["cpu_free"].as<std::int64_t>(),
                             row["memory_free"].as<std::int64_t>(),
                             json_field(row["labels"]), {}});
  }
  for (const auto& row : transaction.exec(
           "SELECT a.node_id,s.placement->>'antiAffinityGroup' AS affinity_group,COUNT(*) AS total "
           "FROM allocations a JOIN services s ON s.service_id=a.service_id "
           "WHERE a.node_id IS NOT NULL AND a.desired_state='running' "
           "AND jsonb_typeof(s.placement->'antiAffinityGroup')='string' "
           "GROUP BY a.node_id,s.placement->>'antiAffinityGroup'")) {
    const std::string node_id = row["node_id"].as<std::string>();
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const Capacity& node) {
      return node.id == node_id;
    });
    if (found != nodes.end()) {
      found->anti_affinity_counts[row["affinity_group"].as<std::string>()] =
          row["total"].as<std::uint64_t>();
    }
  }

  const auto pending = transaction.exec(
      "SELECT a.allocation_id,a.service_id,a.replica,a.observed_state,a.service_generation,"
      "a.allocation_revision,s.name,s.image,s.cpu_millis,s.memory_mb,s.container_port,"
      "s.health_path,s.environment,s.placement,s.generation FROM allocations a "
      "JOIN services s ON s.service_id=a.service_id WHERE a.desired_state='running' "
      "AND a.node_id IS NULL ORDER BY s.created_at,a.replica FOR UPDATE OF a");
  for (const auto& allocation : pending) {
    const std::int64_t cpu = allocation["cpu_millis"].as<std::int64_t>();
    const std::int64_t memory = allocation["memory_mb"].as<std::int64_t>();
    const nlohmann::json placement = json_field(allocation["placement"]);
    const nlohmann::json required = placement.value("requiredLabels", nlohmann::json::object());
    const std::string affinity = placement.value("antiAffinityGroup", "");
    auto selected = nodes.end();
    using Rank = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                            std::string, std::string>;
    std::optional<Rank> selected_rank;
    for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
      if (iterator->cpu_free < cpu || iterator->memory_free < memory ||
          !labels_match(required, iterator->labels)) {
        continue;
      }
      const std::uint64_t cpu_used_after = static_cast<std::uint64_t>(
          iterator->cpu_capacity - iterator->cpu_free + cpu);
      const std::uint64_t memory_used_after = static_cast<std::uint64_t>(
          iterator->memory_capacity - iterator->memory_free + memory);
      const std::uint64_t cpu_ppm =
          (cpu_used_after * 1'000'000ULL) /
          static_cast<std::uint64_t>(iterator->cpu_capacity);
      const std::uint64_t memory_ppm =
          (memory_used_after * 1'000'000ULL) /
          static_cast<std::uint64_t>(iterator->memory_capacity);
      const auto conflicts = affinity.empty() ? 0ULL : iterator->anti_affinity_counts[affinity];
      Rank rank{conflicts, std::max(cpu_ppm, memory_ppm), cpu_ppm + memory_ppm,
                iterator->name, iterator->id};
      if (!selected_rank || rank < *selected_rank) {
        selected_rank = std::move(rank);
        selected = iterator;
      }
    }
    if (selected == nodes.end()) {
      ++result.unschedulable;
      transaction.exec_params(
          "UPDATE allocations SET last_error='no ready worker satisfies resources and placement',"
          "updated_at=now() WHERE allocation_id=$1",
          allocation["allocation_id"].as<std::string>());
      continue;
    }
    const std::string allocation_id = allocation["allocation_id"].as<std::string>();
    const std::int64_t revision = allocation["allocation_revision"].as<std::int64_t>();
    const std::int64_t generation = allocation["generation"].as<std::int64_t>();
    supersede_active_commands(transaction, allocation_id, "allocation was reassigned");
    transaction.exec_params(
        "UPDATE allocations SET node_id=$2,observed_state='pending',service_generation=$3,"
        "container_id=NULL,endpoint=NULL,last_error=NULL,updated_at=now() "
        "WHERE allocation_id=$1",
        allocation_id, selected->id, generation);
    const std::string dedupe = allocation_id + ":" + std::to_string(revision) + ":ensure";
    const auto created = transaction.exec_params(
        "INSERT INTO commands(command_id,dedupe_key,allocation_id,node_id,kind,payload,"
        "service_generation,allocation_revision,status) VALUES($1,$2,$3,$4,'ensure',$5::jsonb,$6,$7,'pending') "
        "ON CONFLICT(dedupe_key) DO NOTHING RETURNING command_id",
        common::random_uuid(), dedupe, allocation_id, selected->id,
        command_payload(allocation, allocation_id, allocation["replica"].as<std::int32_t>(), revision).dump(),
        generation, revision);
    if (!created.empty()) {
      ++result.scheduled;
      append_event(transaction, "allocation.scheduled", allocation_id,
                   "scheduler selected a worker",
                   {{"node", selected->name}, {"revision", revision}});
    }
    selected->cpu_free -= cpu;
    selected->memory_free -= memory;
    if (!affinity.empty()) ++selected->anti_affinity_counts[affinity];
  }
  transaction.commit();
  return result;
}

void to_json(nlohmann::json& json, const ServiceRecord& value) {
  json = {{"id", value.id}, {"name", value.name}, {"image", value.image},
          {"desiredReplicas", value.replicas}, {"cpuMillis", value.cpu_millis},
          {"memoryMb", value.memory_mb}, {"containerPort", value.container_port},
          {"healthPath", value.health_path}, {"environment", value.environment},
          {"placement", value.placement}, {"generation", value.generation},
          {"createdAt", value.created_at}, {"updatedAt", value.updated_at}};
}

void to_json(nlohmann::json& json, const NodeRecord& value) {
  json = {{"id", value.id}, {"name", value.name}, {"status", value.status},
          {"cpuCapacityMillis", value.cpu_capacity_millis},
          {"cpuAllocatedMillis", value.cpu_allocated_millis},
          {"memoryCapacityMb", value.memory_capacity_mb},
          {"memoryAllocatedMb", value.memory_allocated_mb}, {"labels", value.labels},
          {"dockerVersion", value.docker_version}, {"agentVersion", value.agent_version},
          {"lastHeartbeat", value.last_heartbeat}};
}

void to_json(nlohmann::json& json, const AllocationRecord& value) {
  json = {{"id", value.id}, {"serviceName", value.service_name}, {"replica", value.replica},
          {"nodeName", value.node_name ? nlohmann::json(*value.node_name) : nlohmann::json(nullptr)},
          {"state", value.observed_state},
          {"containerId", value.container_id ? nlohmann::json(*value.container_id) : nlohmann::json(nullptr)},
          {"endpoint", value.endpoint ? nlohmann::json(*value.endpoint) : nlohmann::json(nullptr)},
          {"restartCount", value.restart_count},
          {"lastError", value.last_error ? nlohmann::json(*value.last_error) : nlohmann::json(nullptr)},
          {"updatedAt", value.updated_at}};
}

void to_json(nlohmann::json& json, const EventRecord& value) {
  json = {{"id", value.id}, {"type", value.type}, {"aggregateId", value.aggregate_id},
          {"message", value.message}, {"payload", value.payload}, {"createdAt", value.created_at}};
}

}  // namespace minicloud::controller

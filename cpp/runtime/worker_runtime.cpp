#include "minicloud/runtime/worker_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace minicloud::runtime {
namespace {

constexpr const char *kManagedLabel = "io.minicloud.managed";
constexpr const char *kWorkerLabel = "io.minicloud.worker-id";
constexpr const char *kWorkerEpochLabel = "io.minicloud.worker-epoch";
constexpr const char *kControllerEpochLabel = "io.minicloud.controller-epoch";
constexpr const char *kWorkloadLabel = "io.minicloud.workload-id";
constexpr const char *kRevisionLabel = "io.minicloud.revision";
constexpr const char *kFingerprintLabel = "io.minicloud.spec-fingerprint";

class OwnershipError final : public std::runtime_error {
public:
  explicit OwnershipError(const std::string &message)
      : std::runtime_error(message) {}
};

[[nodiscard]] bool valid_id(const std::string &value) {
  static const std::regex pattern(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$)");
  return std::regex_match(value, pattern);
}

[[nodiscard]] std::string operation_name(RuntimeCommandKind kind) {
  switch (kind) {
  case RuntimeCommandKind::Ensure:
    return "ensure";
  case RuntimeCommandKind::Stop:
    return "stop";
  case RuntimeCommandKind::Remove:
    return "remove";
  }
  return "unknown";
}

[[nodiscard]] std::string hex_hash(const std::string &value);
[[nodiscard]] std::string workload_fingerprint(const WorkloadSpec &spec);

[[nodiscard]] std::string command_fingerprint(const RuntimeCommand &command) {
  std::string value = operation_name(command.kind) + "\n" +
                      command.workload_id + "\n" +
                      std::to_string(command.revision);
  if (command.desired.has_value()) {
    value += "\n" + workload_fingerprint(*command.desired);
  }
  if (command.stop_timeout.has_value()) {
    value += "\nstop-timeout=" + std::to_string(command.stop_timeout->count());
  }
  value += "\ncontroller-epoch=" + std::to_string(command.controller_epoch);
  return hex_hash(value);
}

[[nodiscard]] std::string safe_message(const std::exception &error) {
  std::string message = error.what();
  std::replace(message.begin(), message.end(), '\r', ' ');
  std::replace(message.begin(), message.end(), '\n', ' ');
  if (message.empty()) {
    message = "runtime operation failed";
  }
  if (message.size() > 512) {
    message.resize(512);
  }
  return message;
}

[[nodiscard]] std::uint64_t fnv1a(const std::string &value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char raw_byte : value) {
    const auto byte = static_cast<unsigned char>(raw_byte);
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::string hex_hash(const std::string &value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::hex << std::setw(16) << std::setfill('0') << fnv1a(value);
  return output.str();
}

[[nodiscard]] std::string workload_fingerprint(const WorkloadSpec &spec) {
  std::ostringstream value;
  value.imbue(std::locale::classic());
  value << WorkerRuntime::spec_fingerprint(spec.container) << '\n'
        << spec.restart.initial_delay.count() << '\n'
        << std::setprecision(std::numeric_limits<double>::max_digits10)
        << spec.restart.multiplier << '\n'
        << spec.restart.max_delay.count() << '\n'
        << spec.restart.max_restarts << '\n'
        << spec.restart.reset_after.count() << '\n'
        << spec.restart.startup_timeout.count() << '\n'
        << spec.stop_timeout.count();
  return hex_hash(value.str());
}

void validate_restart_policy(const RuntimeRestartPolicy &policy) {
  if (policy.initial_delay < std::chrono::milliseconds(0) ||
      policy.initial_delay > std::chrono::minutes(5) ||
      !std::isfinite(policy.multiplier) || policy.multiplier < 1.0 ||
      policy.multiplier > 100.0 || policy.max_delay < policy.initial_delay ||
      policy.max_delay > std::chrono::hours(1) || policy.max_restarts > 10000 ||
      policy.reset_after <= std::chrono::milliseconds(0) ||
      policy.reset_after > std::chrono::hours(24) ||
      policy.startup_timeout <= std::chrono::milliseconds(0) ||
      policy.startup_timeout > std::chrono::hours(1)) {
    throw std::invalid_argument("restart policy is outside safe bounds");
  }
}

[[nodiscard]] std::uint64_t
label_positive_integer(const ContainerInspection &inspection, const char *label,
                       const char *description) {
  const auto iterator = inspection.labels.find(label);
  if (iterator == inspection.labels.end() || iterator->second.empty()) {
    throw OwnershipError(std::string("owned container is missing its ") +
                         description + " label");
  }
  std::size_t consumed = 0;
  std::uint64_t value = 0;
  try {
    value = std::stoull(iterator->second, &consumed);
  } catch (const std::exception &) {
    throw OwnershipError(std::string("owned container has an invalid ") +
                         description + " label");
  }
  if (consumed != iterator->second.size() || value == 0) {
    throw OwnershipError(std::string("owned container has an invalid ") +
                         description + " label");
  }
  return value;
}

[[nodiscard]] std::uint64_t
label_revision(const ContainerInspection &inspection) {
  return label_positive_integer(inspection, kRevisionLabel, "revision");
}

[[nodiscard]] std::uint64_t
label_worker_epoch(const ContainerInspection &inspection) {
  return label_positive_integer(inspection, kWorkerEpochLabel, "worker epoch");
}

[[nodiscard]] std::uint64_t
label_controller_epoch(const ContainerInspection &inspection) {
  return label_positive_integer(inspection, kControllerEpochLabel,
                                "controller epoch");
}

} // namespace

WorkerRuntime::WorkerRuntime(std::string worker_id,
                             std::shared_ptr<IDockerClient> docker,
                             std::size_t receipt_capacity)
    : WorkerRuntime(std::move(worker_id), 1, std::move(docker),
                    receipt_capacity) {}

WorkerRuntime::WorkerRuntime(std::string worker_id, std::uint64_t worker_epoch,
                             std::shared_ptr<IDockerClient> docker,
                             std::size_t receipt_capacity)
    : worker_id_(std::move(worker_id)), worker_epoch_(worker_epoch),
      docker_(std::move(docker)), receipt_capacity_(receipt_capacity) {
  if (!valid_id(worker_id_)) {
    throw std::invalid_argument("worker_id is invalid");
  }
  if (!docker_) {
    throw std::invalid_argument("Docker client is required");
  }
  if (worker_epoch_ == 0) {
    throw std::invalid_argument("worker_epoch must be positive");
  }
  if (receipt_capacity_ == 0 || receipt_capacity_ > 1'000'000) {
    throw std::invalid_argument(
        "receipt_capacity must be between 1 and 1000000");
  }
}

CommandResult WorkerRuntime::apply(const RuntimeCommand &command,
                                   Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!valid_id(command.command_id) || !valid_id(command.workload_id) ||
      command.revision == 0 || command.controller_epoch == 0) {
    throw std::invalid_argument(
        "runtime command identity or revision is invalid");
  }
  const std::string received_fingerprint = command_fingerprint(command);
  const auto receipt = receipts_.find(command.command_id);
  if (receipt != receipts_.end()) {
    if (receipt->second.command_fingerprint != received_fingerprint) {
      throw std::invalid_argument(
          "command_id was reused for different command content");
    }
    CommandResult replay = receipt->second.result;
    replay.replayed = true;
    metrics_.record_replay();
    metrics_.record_command(operation_name(command.kind), "replayed");
    return replay;
  }

  CommandResult result;
  try {
    switch (command.kind) {
    case RuntimeCommandKind::Ensure:
      result = apply_ensure(command, now);
      break;
    case RuntimeCommandKind::Stop:
      result = apply_stop(command, false);
      break;
    case RuntimeCommandKind::Remove:
      result = apply_stop(command, true);
      break;
    default:
      throw std::invalid_argument("runtime command kind is invalid");
    }
  } catch (const DockerError &error) {
    result = {
        command.command_id,
        command.workload_id,
        command.revision,
        WorkloadState::Failed,
        false,
        false,
        {},
        safe_message(error),
    };
    result.retryable = true;
    metrics_.record_docker_error(operation_name(command.kind));
    const auto entry = workloads_.find(command.workload_id);
    if (entry != workloads_.end()) {
      result.state = entry->second.state;
      result.container_id = entry->second.container_id;
      entry->second.last_error = result.message;
    }
  } catch (const std::exception &error) {
    result = {
        command.command_id,
        command.workload_id,
        command.revision,
        WorkloadState::Failed,
        false,
        false,
        {},
        safe_message(error),
    };
    const auto entry = workloads_.find(command.workload_id);
    if (entry != workloads_.end()) {
      result.state = entry->second.state;
      result.container_id = entry->second.container_id;
      entry->second.last_error = result.message;
    }
  }
  metrics_.record_command(operation_name(command.kind),
                          result.succeeded ? "success" : "failure");
  if (result.retryable) {
    return result;
  }
  return cache_result(std::move(result), received_fingerprint);
}

void WorkerRuntime::advance_worker_epoch(std::uint64_t worker_epoch,
                                         Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (worker_epoch <= worker_epoch_) {
    throw std::invalid_argument("worker epoch must advance monotonically");
  }
  worker_epoch_ = worker_epoch;
  for (auto &[ignored, entry] : workloads_) {
    (void)ignored;
    entry.spec.container.labels[kWorkerEpochLabel] =
        std::to_string(worker_epoch_);
    entry.transient_failure_count = 0;
    entry.healthy_since.reset();
    entry.starting_since.reset();
    if (entry.desired_running && entry.state != WorkloadState::Removed) {
      entry.state = WorkloadState::Backoff;
      entry.next_restart_at = now;
      entry.last_error = "worker epoch advanced; ownership will be reacquired";
    }
  }
}

CommandResult WorkerRuntime::apply_ensure(const RuntimeCommand &command,
                                          Clock::time_point now) {
  if (!command.desired.has_value()) {
    throw std::invalid_argument(
        "ensure command requires a desired workload spec");
  }
  if (command.stop_timeout.has_value()) {
    throw std::invalid_argument(
        "ensure command must not contain a stop timeout override");
  }
  if (command.desired->workload_id != command.workload_id ||
      command.desired->revision != command.revision ||
      command.desired->controller_epoch != command.controller_epoch) {
    throw std::invalid_argument(
        "ensure command and desired spec identities must match");
  }
  validate_restart_policy(command.desired->restart);
  if (command.desired->stop_timeout < std::chrono::seconds(0) ||
      command.desired->stop_timeout > std::chrono::seconds(300)) {
    throw std::invalid_argument(
        "stop_timeout must be between 0 and 300 seconds");
  }
  for (const auto &[key, ignored] : command.desired->container.labels) {
    (void)ignored;
    if (key.rfind("io.minicloud.", 0) == 0) {
      throw std::invalid_argument(
          "io.minicloud.* labels are reserved for ownership fencing");
    }
  }

  ContainerSpec container = command.desired->container;
  WorkloadSpec fingerprint_source = *command.desired;
  fingerprint_source.container = container;
  const std::string fingerprint = workload_fingerprint(fingerprint_source);
  container.labels[kManagedLabel] = "true";
  container.labels[kWorkerLabel] = worker_id_;
  container.labels[kWorkerEpochLabel] = std::to_string(worker_epoch_);
  container.labels[kControllerEpochLabel] =
      std::to_string(command.controller_epoch);
  container.labels[kWorkloadLabel] = command.workload_id;
  container.labels[kRevisionLabel] = std::to_string(command.revision);
  container.labels[kFingerprintLabel] = fingerprint;
  // Validate the final request, including platform-owned labels, before any
  // destructive action touches an existing container.
  (void)DockerClient::create_payload(container);

  const auto current = workloads_.find(command.workload_id);
  if (current != workloads_.end()) {
    if (command.controller_epoch < current->second.spec.controller_epoch) {
      throw std::invalid_argument("stale controller epoch was fenced");
    }
    if (command.revision < current->second.spec.revision) {
      throw std::invalid_argument("stale ensure revision was fenced");
    }
    if (command.revision == current->second.spec.revision &&
        fingerprint != current->second.fingerprint) {
      throw std::invalid_argument("a workload revision is immutable");
    }
  }

  Entry next;
  next.spec = *command.desired;
  next.spec.container = std::move(container);
  next.container_name = container_name_for(command.workload_id);
  next.fingerprint = fingerprint;
  next.desired_running = true;

  if (current != workloads_.end() &&
      command.revision == current->second.spec.revision) {
    next.restart_count = current->second.restart_count;
    next.transient_failure_count = current->second.transient_failure_count;
    next.next_restart_at = current->second.next_restart_at;
    next.healthy_since = current->second.healthy_since;
    next.starting_since = current->second.starting_since;
    next.last_exit_code = current->second.last_exit_code;
  }
  auto [iterator, inserted] =
      workloads_.insert_or_assign(command.workload_id, std::move(next));
  (void)inserted;
  try {
    ensure_container(iterator->second, now);
  } catch (const OwnershipError &error) {
    iterator->second.state = WorkloadState::Failed;
    iterator->second.next_restart_at.reset();
    iterator->second.last_error = safe_message(error);
    throw;
  } catch (const DockerError &error) {
    schedule_transient_retry(iterator->second, "docker_error", now);
    iterator->second.last_error = safe_message(error);
    throw;
  }

  return {
      command.command_id,
      command.workload_id,
      command.revision,
      iterator->second.state,
      true,
      false,
      iterator->second.container_id,
      iterator->second.state == WorkloadState::Backoff
          ? "desired state accepted; Docker retry is in backoff"
          : "desired state accepted",
  };
}

CommandResult WorkerRuntime::apply_stop(const RuntimeCommand &command,
                                        bool remove_container) {
  if (command.desired.has_value()) {
    throw std::invalid_argument(
        "stop and remove commands must not contain a desired spec");
  }
  if (command.stop_timeout.has_value() &&
      (*command.stop_timeout < std::chrono::seconds(0) ||
       *command.stop_timeout > std::chrono::seconds(300))) {
    throw std::invalid_argument(
        "stop timeout override must be between 0 and 300 seconds");
  }
  auto iterator = workloads_.find(command.workload_id);
  if (iterator != workloads_.end() &&
      command.revision < iterator->second.spec.revision) {
    throw std::invalid_argument("stale stop/remove revision was fenced");
  }

  Entry fallback;
  fallback.spec.workload_id = command.workload_id;
  fallback.spec.revision = command.revision;
  fallback.spec.controller_epoch = command.controller_epoch;
  fallback.container_name = container_name_for(command.workload_id);
  Entry &entry =
      iterator == workloads_.end()
          ? workloads_.emplace(command.workload_id, std::move(fallback))
                .first->second
          : iterator->second;
  if (command.controller_epoch < entry.spec.controller_epoch) {
    throw std::invalid_argument("stale controller epoch was fenced");
  }
  entry.desired_running = false;
  entry.next_restart_at.reset();
  entry.healthy_since.reset();
  entry.starting_since.reset();

  const auto stop_timeout =
      command.stop_timeout.value_or(entry.spec.stop_timeout);

  const auto inspection = docker_->inspect(entry.container_name);
  if (inspection.has_value()) {
    validate_managed_identity(*inspection, entry);
    const bool current_owner = owned_by_current_worker(*inspection);
    const auto observed_controller_epoch = label_controller_epoch(*inspection);
    const auto observed_revision = label_revision(*inspection);
    if (observed_controller_epoch > command.controller_epoch) {
      throw OwnershipError(
          "a newer controller epoch fences this stop/remove command");
    }
    if (!current_owner && observed_revision >= command.revision) {
      throw OwnershipError(
          "another worker owns this container at the same or a newer revision");
    }
    if (current_owner && label_worker_epoch(*inspection) > worker_epoch_) {
      throw OwnershipError(
          "a newer worker epoch fences this stop/remove command");
    }
    if (observed_revision > command.revision) {
      throw OwnershipError(
          "a newer owned container fences this stop/remove command");
    }
    entry.container_id = inspection->id;
    if (inspection->running) {
      entry.state = WorkloadState::Stopping;
      docker_->stop(inspection->id, stop_timeout);
    }
    if (remove_container) {
      docker_->remove(inspection->id, false, false);
    }
  }
  entry.spec.revision = command.revision;
  entry.spec.controller_epoch = command.controller_epoch;
  entry.state =
      remove_container ? WorkloadState::Removed : WorkloadState::Stopped;
  entry.last_error.clear();
  if (remove_container) {
    entry.container_id.clear();
  }
  return {
      command.command_id,
      command.workload_id,
      command.revision,
      entry.state,
      true,
      false,
      entry.container_id,
      remove_container ? "owned container is absent"
                       : "owned container is stopped",
  };
}

void WorkerRuntime::ensure_container(Entry &entry, Clock::time_point now) {
  auto inspection = docker_->inspect(entry.container_name);
  if (inspection.has_value()) {
    validate_managed_identity(*inspection, entry);
    const bool current_owner = owned_by_current_worker(*inspection);
    const auto observed_worker_epoch = label_worker_epoch(*inspection);
    const auto observed_controller_epoch = label_controller_epoch(*inspection);
    const auto observed_revision = label_revision(*inspection);
    const auto observed_fingerprint =
        inspection->labels.find(kFingerprintLabel);
    if (observed_controller_epoch > entry.spec.controller_epoch) {
      throw OwnershipError(
          "a newer controller epoch fences this desired workload");
    }
    if (!current_owner && observed_revision >= entry.spec.revision) {
      throw OwnershipError(
          "another worker owns this container at the same or a newer revision");
    }
    if (current_owner && observed_worker_epoch > worker_epoch_) {
      throw OwnershipError("a newer worker epoch fences this desired workload");
    }
    if (observed_revision > entry.spec.revision) {
      throw OwnershipError(
          "a newer owned container fences this desired workload");
    }
    if (current_owner && observed_revision == entry.spec.revision &&
        (observed_fingerprint == inspection->labels.end() ||
         observed_fingerprint->second != entry.fingerprint)) {
      throw OwnershipError(
          "an owned container conflicts with the immutable workload revision");
    }
    if (!current_owner || observed_worker_epoch < worker_epoch_ ||
        observed_controller_epoch < entry.spec.controller_epoch ||
        observed_revision < entry.spec.revision) {
      if (inspection->running) {
        docker_->stop(inspection->id, entry.spec.stop_timeout);
      }
      docker_->remove(inspection->id, false, false);
      inspection.reset();
      entry.container_id.clear();
    }
  }
  if (!inspection.has_value()) {
    entry.container_id =
        docker_->create(entry.container_name, entry.spec.container);
    docker_->start(entry.container_id);
    entry.state = WorkloadState::Starting;
    entry.next_restart_at.reset();
    entry.transient_failure_count = 0;
    entry.healthy_since.reset();
    entry.starting_since = now;
    entry.last_error.clear();
    entry.last_exit_code.reset();
    return;
  }
  validate_owned(*inspection, entry, true);
  entry.container_id = inspection->id;
  if (!inspection->running) {
    docker_->start(inspection->id);
    entry.state = WorkloadState::Starting;
    entry.next_restart_at.reset();
    entry.transient_failure_count = 0;
    entry.healthy_since.reset();
    entry.starting_since = now;
    entry.last_error.clear();
    entry.last_exit_code.reset();
    return;
  }
  const bool has_healthcheck = entry.spec.container.health.has_value();
  if (!has_healthcheck || inspection->health_status == "healthy") {
    entry.state = WorkloadState::Healthy;
    entry.transient_failure_count = 0;
    if (!entry.healthy_since.has_value()) {
      entry.healthy_since = now;
    }
    entry.starting_since.reset();
  } else if (inspection->health_status == "unhealthy") {
    docker_->stop(inspection->id, entry.spec.stop_timeout);
    schedule_restart(entry, "unhealthy", now);
  } else {
    entry.state = WorkloadState::Starting;
    if (!entry.starting_since.has_value()) {
      entry.starting_since = now;
    }
    entry.last_error.clear();
  }
}

void WorkerRuntime::validate_managed_identity(
    const ContainerInspection &inspection, const Entry &entry) const {
  const auto require_label = [&inspection](const char *key,
                                           const std::string &expected) {
    const auto iterator = inspection.labels.find(key);
    if (iterator == inspection.labels.end() || iterator->second != expected) {
      throw OwnershipError(
          "refusing to operate on a container outside MiniCloud ownership");
    }
  };
  require_label(kManagedLabel, "true");
  require_label(kWorkloadLabel, entry.spec.workload_id);

  const auto owner = inspection.labels.find(kWorkerLabel);
  if (owner == inspection.labels.end() || !valid_id(owner->second)) {
    throw OwnershipError("managed container has an invalid worker owner label");
  }
  (void)label_worker_epoch(inspection);
  (void)label_controller_epoch(inspection);
  (void)label_revision(inspection);
  const auto fingerprint = inspection.labels.find(kFingerprintLabel);
  if (fingerprint == inspection.labels.end() ||
      fingerprint->second.size() != 16 ||
      !std::all_of(fingerprint->second.begin(), fingerprint->second.end(),
                   [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
    throw OwnershipError("managed container has an invalid fingerprint label");
  }
}

bool WorkerRuntime::owned_by_current_worker(
    const ContainerInspection &inspection) const {
  const auto owner = inspection.labels.find(kWorkerLabel);
  return owner != inspection.labels.end() && owner->second == worker_id_;
}

void WorkerRuntime::validate_owned(const ContainerInspection &inspection,
                                   const Entry &entry,
                                   bool require_current_spec) const {
  validate_managed_identity(inspection, entry);
  const auto require_label = [&inspection](const char *key,
                                           const std::string &expected) {
    const auto iterator = inspection.labels.find(key);
    if (iterator == inspection.labels.end() || iterator->second != expected) {
      throw OwnershipError(
          "refusing to operate on a container outside MiniCloud ownership");
    }
  };
  require_label(kWorkerLabel, worker_id_);
  if (require_current_spec) {
    require_label(kWorkerEpochLabel, std::to_string(worker_epoch_));
    require_label(kControllerEpochLabel,
                  std::to_string(entry.spec.controller_epoch));
    require_label(kRevisionLabel, std::to_string(entry.spec.revision));
    require_label(kFingerprintLabel, entry.fingerprint);
  }
}

void WorkerRuntime::schedule_restart(Entry &entry, const std::string &reason,
                                     Clock::time_point now) {
  entry.transient_failure_count = 0;
  entry.healthy_since.reset();
  entry.starting_since.reset();
  if (!entry.desired_running) {
    entry.state = WorkloadState::Stopped;
    entry.next_restart_at.reset();
    return;
  }
  if (entry.restart_count >= entry.spec.restart.max_restarts) {
    entry.state = WorkloadState::Failed;
    entry.next_restart_at.reset();
    entry.last_error = "restart limit exhausted after " + reason;
    return;
  }
  ++entry.restart_count;
  entry.state = WorkloadState::Backoff;
  entry.next_restart_at = now + restart_delay(entry);
  entry.last_error = "restart scheduled after " + reason;
  metrics_.record_restart(reason);
}

void WorkerRuntime::schedule_transient_retry(Entry &entry,
                                             const std::string &reason,
                                             Clock::time_point now) {
  entry.healthy_since.reset();
  entry.starting_since.reset();
  if (!entry.desired_running) {
    entry.state = WorkloadState::Stopped;
    entry.next_restart_at.reset();
    return;
  }
  entry.transient_failure_count =
      std::min<std::size_t>(entry.transient_failure_count + 1, 64);
  entry.state = WorkloadState::Backoff;
  entry.next_restart_at = now + transient_retry_delay(entry);
  entry.last_error = "runtime retry scheduled after " + reason;
}

std::chrono::milliseconds
WorkerRuntime::restart_delay(const Entry &entry) const {
  const auto exponent = entry.restart_count > 0 ? entry.restart_count - 1 : 0;
  const long double scaled =
      static_cast<long double>(entry.spec.restart.initial_delay.count()) *
      std::pow(static_cast<long double>(entry.spec.restart.multiplier),
               static_cast<long double>(exponent));
  const long double bounded = std::min<long double>(
      scaled, static_cast<long double>(entry.spec.restart.max_delay.count()));
  return std::chrono::milliseconds(static_cast<std::int64_t>(bounded));
}

std::chrono::milliseconds
WorkerRuntime::transient_retry_delay(const Entry &entry) const {
  const auto exponent =
      entry.transient_failure_count > 0 ? entry.transient_failure_count - 1 : 0;
  const long double scaled =
      static_cast<long double>(entry.spec.restart.initial_delay.count()) *
      std::pow(static_cast<long double>(entry.spec.restart.multiplier),
               static_cast<long double>(exponent));
  const long double bounded = std::min<long double>(
      scaled, static_cast<long double>(entry.spec.restart.max_delay.count()));
  return std::chrono::milliseconds(static_cast<std::int64_t>(bounded));
}

void WorkerRuntime::tick(Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &[workload_id, entry] : workloads_) {
    (void)workload_id;
    if (!entry.desired_running || entry.state == WorkloadState::Failed ||
        entry.state == WorkloadState::Removed) {
      continue;
    }
    if (entry.state == WorkloadState::Backoff &&
        entry.next_restart_at.has_value() && now < *entry.next_restart_at) {
      continue;
    }
    try {
      if (entry.state == WorkloadState::Backoff) {
        ensure_container(entry, now);
        continue;
      }
      const auto inspection = docker_->inspect(entry.container_name);
      if (!inspection.has_value()) {
        schedule_restart(entry, "missing", now);
        continue;
      }
      validate_owned(*inspection, entry, true);
      entry.container_id = inspection->id;
      if (!inspection->running) {
        entry.last_exit_code = inspection->exit_code;
        schedule_restart(entry, "exit", now);
        continue;
      }
      if (entry.spec.container.health.has_value() &&
          inspection->health_status == "unhealthy") {
        docker_->stop(inspection->id, entry.spec.stop_timeout);
        schedule_restart(entry, "unhealthy", now);
        continue;
      }
      if (!entry.spec.container.health.has_value() ||
          inspection->health_status == "healthy") {
        entry.state = WorkloadState::Healthy;
        entry.transient_failure_count = 0;
        if (!entry.healthy_since.has_value()) {
          entry.healthy_since = now;
        }
        if (now - *entry.healthy_since >= entry.spec.restart.reset_after) {
          entry.restart_count = 0;
        }
        entry.next_restart_at.reset();
        entry.starting_since.reset();
        entry.last_error.clear();
      } else {
        entry.state = WorkloadState::Starting;
        if (!entry.starting_since.has_value()) {
          entry.starting_since = now;
        } else if (now - *entry.starting_since >=
                   entry.spec.restart.startup_timeout) {
          docker_->stop(inspection->id, entry.spec.stop_timeout);
          schedule_restart(entry, "startup_timeout", now);
        }
      }
    } catch (const OwnershipError &error) {
      entry.state = WorkloadState::Failed;
      entry.next_restart_at.reset();
      entry.last_error = safe_message(error);
    } catch (const DockerError &error) {
      metrics_.record_docker_error("inspect");
      schedule_transient_retry(entry, "docker_error", now);
      entry.last_error = safe_message(error);
    }
  }
}

WorkloadSnapshot WorkerRuntime::to_snapshot(const Entry &entry) const {
  return {
      entry.spec.workload_id, entry.spec.revision, entry.state,
      entry.desired_running,  entry.container_id,  entry.restart_count,
      entry.next_restart_at,  entry.last_error,    entry.last_exit_code,
  };
}

std::optional<WorkloadSnapshot>
WorkerRuntime::snapshot(const std::string &workload_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = workloads_.find(workload_id);
  if (iterator == workloads_.end()) {
    return std::nullopt;
  }
  return to_snapshot(iterator->second);
}

std::vector<WorkloadSnapshot> WorkerRuntime::snapshots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorkloadSnapshot> result;
  result.reserve(workloads_.size());
  for (const auto &[ignored, entry] : workloads_) {
    (void)ignored;
    result.push_back(to_snapshot(entry));
  }
  return result;
}

std::string WorkerRuntime::prometheus_metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, std::size_t> states = {
      {"stopped", 0}, {"starting", 0}, {"healthy", 0}, {"backoff", 0},
      {"failed", 0},  {"stopping", 0}, {"removed", 0},
  };
  for (const auto &[ignored, entry] : workloads_) {
    (void)ignored;
    ++states[state_name(entry.state)];
  }
  return metrics_.render_prometheus(states);
}

std::string WorkerRuntime::state_name(WorkloadState state) {
  switch (state) {
  case WorkloadState::Stopped:
    return "stopped";
  case WorkloadState::Starting:
    return "starting";
  case WorkloadState::Healthy:
    return "healthy";
  case WorkloadState::Backoff:
    return "backoff";
  case WorkloadState::Failed:
    return "failed";
  case WorkloadState::Stopping:
    return "stopping";
  case WorkloadState::Removed:
    return "removed";
  }
  return "failed";
}

std::string WorkerRuntime::container_name_for(const std::string &workload_id) {
  if (!valid_id(workload_id)) {
    throw std::invalid_argument("workload_id is invalid");
  }
  std::string readable;
  readable.reserve(std::min<std::size_t>(workload_id.size(), 40));
  bool previous_dash = false;
  for (const char raw_character : workload_id) {
    const auto ch = static_cast<unsigned char>(raw_character);
    char output = '-';
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      output = static_cast<char>(ch);
    } else if (ch >= 'A' && ch <= 'Z') {
      output = static_cast<char>(ch - 'A' + 'a');
    }
    if (output == '-') {
      if (previous_dash || readable.empty()) {
        continue;
      }
      previous_dash = true;
    } else {
      previous_dash = false;
    }
    readable.push_back(output);
    if (readable.size() == 40) {
      break;
    }
  }
  while (!readable.empty() && readable.back() == '-') {
    readable.pop_back();
  }
  if (readable.empty()) {
    readable = "workload";
  }
  return "mc-" + readable + "-" + hex_hash(workload_id).substr(0, 12);
}

std::string WorkerRuntime::spec_fingerprint(const ContainerSpec &spec) {
  // nlohmann::json's default object type is key ordered, so dump() is stable
  // for maps. FNV-1a is an identity checksum, not an authenticity mechanism;
  // Docker ownership is independently fenced by worker/workload/revision
  // labels.
  return hex_hash(DockerClient::create_payload(spec).dump());
}

CommandResult
WorkerRuntime::cache_result(CommandResult result,
                            const std::string &command_fingerprint_value) {
  result.replayed = false;
  while (receipt_order_.size() >= receipt_capacity_) {
    receipts_.erase(receipt_order_.front());
    receipt_order_.pop_front();
  }
  receipt_order_.push_back(result.command_id);
  receipts_[result.command_id] = Receipt{command_fingerprint_value, result};
  return result;
}

} // namespace minicloud::runtime

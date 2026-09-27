#include "minicloud/core/reconciler.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace minicloud::core {
namespace {

std::uint64_t NextRevision(const std::uint64_t revision) {
  if (revision == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("allocation revision is exhausted");
  }
  return revision + 1;
}

std::uint64_t Fnv1a64(const std::string_view value) noexcept {
  std::uint64_t result = 14695981039346656037ULL;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    result ^= character;
    result *= 1099511628211ULL;
  }
  return result;
}

std::string Hex64(const std::uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

PlannedCommand CommandFor(const WorkloadSpec& workload,
                          const Allocation& allocation,
                          const CommandKind kind,
                          const std::uint64_t controller_epoch) {
  if (!allocation.node_id().has_value()) {
    throw std::logic_error("cannot plan a worker command for a pending allocation");
  }
  return PlannedCommand(
      MakeCommandId(kind, allocation.id(), workload.generation(),
                    allocation.revision(), controller_epoch),
      kind, *allocation.node_id(), allocation.id(), workload.id(),
      allocation.replica_index(), workload.generation(), allocation.revision(),
      controller_epoch,
      kind == CommandKind::kEnsureWorkload ? workload.spec_digest() : std::string());
}

Allocation PresentIntent(const WorkloadSpec& workload,
                         const Allocation& allocation,
                         const std::uint64_t revision) {
  return Allocation(
      allocation.id(), workload.id(), allocation.replica_index(),
      allocation.node_id(), workload.resources(), workload.anti_affinity_group(),
      DesiredState::kPresent,
      allocation.node_id().has_value() ? RuntimeState::kStarting
                                       : RuntimeState::kPending,
      workload.generation(), allocation.observed_generation(), revision);
}

Allocation AbsentIntent(const Allocation& allocation,
                        const std::uint64_t target_generation,
                        const std::uint64_t revision) {
  return Allocation(
      allocation.id(), allocation.workload_id(), allocation.replica_index(),
      allocation.node_id(), allocation.resources(),
      allocation.anti_affinity_group(), DesiredState::kAbsent,
      allocation.node_id().has_value() ? RuntimeState::kStopping
                                       : RuntimeState::kStopped,
      target_generation, allocation.observed_generation(), revision);
}

bool RequiresFreshEnsureRevision(const Allocation& allocation,
                                 const std::uint64_t desired_generation) noexcept {
  if (allocation.runtime_state() == RuntimeState::kFailed ||
      allocation.runtime_state() == RuntimeState::kStopped) {
    return true;
  }
  return allocation.runtime_state() == RuntimeState::kRunning &&
         allocation.observed_generation() != desired_generation;
}

}  // namespace

AllocationChange::AllocationChange(
    const AllocationChangeKind kind,
    std::optional<std::uint64_t> expected_revision, Allocation desired)
    : kind_(kind),
      expected_revision_(expected_revision),
      desired_(std::move(desired)) {
  if (kind_ == AllocationChangeKind::kCreate && expected_revision_.has_value()) {
    throw std::invalid_argument("create must not have an expected revision");
  }
  if (kind_ == AllocationChangeKind::kUpdateIntent &&
      !expected_revision_.has_value()) {
    throw std::invalid_argument("update must have an expected revision");
  }
}

PlannedCommand::PlannedCommand(
    std::string command_id, const CommandKind kind, std::string worker_id,
    std::string allocation_id, std::string workload_id,
    const std::uint32_t replica_index,
    const std::uint64_t workload_generation,
    const std::uint64_t allocation_revision,
    const std::uint64_t controller_epoch, std::string spec_digest)
    : command_id_(std::move(command_id)),
      kind_(kind),
      worker_id_(std::move(worker_id)),
      allocation_id_(std::move(allocation_id)),
      workload_id_(std::move(workload_id)),
      replica_index_(replica_index),
      workload_generation_(workload_generation),
      allocation_revision_(allocation_revision),
      controller_epoch_(controller_epoch),
      spec_digest_(std::move(spec_digest)) {
  if (command_id_.empty() || command_id_.size() > 256) {
    throw std::invalid_argument("command id has an invalid length");
  }
  if (!IsValidResourceId(worker_id_) || !IsValidResourceId(allocation_id_) ||
      !IsValidResourceId(workload_id_)) {
    throw std::invalid_argument("command references an invalid resource id");
  }
  if (workload_generation_ == 0 || allocation_revision_ == 0 ||
      controller_epoch_ == 0) {
    throw std::invalid_argument("command generations, revisions, and epochs must be positive");
  }
  if (kind_ == CommandKind::kEnsureWorkload && spec_digest_.empty()) {
    throw std::invalid_argument("ensure command requires a spec digest");
  }
  if (kind_ == CommandKind::kStopWorkload && !spec_digest_.empty()) {
    throw std::invalid_argument("stop command must not carry a spec digest");
  }
}

ReconcilePlan::ReconcilePlan(
    std::vector<AllocationChange> allocation_changes,
    std::vector<PlannedCommand> commands)
    : allocation_changes_(std::move(allocation_changes)),
      commands_(std::move(commands)) {}

ReconcilePlan Reconciler::Plan(const WorkloadSpec& workload,
                               const std::vector<Allocation>& current,
                               const std::uint64_t controller_epoch) {
  if (controller_epoch == 0) {
    throw std::invalid_argument("controller_epoch must be positive");
  }

  std::map<std::uint32_t, const Allocation*> by_replica;
  std::set<std::string> ids;
  for (const Allocation& allocation : current) {
    if (allocation.workload_id() != workload.id()) {
      throw std::invalid_argument("reconcile snapshot contains another workload");
    }
    if (!ids.insert(allocation.id()).second) {
      throw std::invalid_argument("allocation ids must be unique");
    }
    if (!by_replica.emplace(allocation.replica_index(), &allocation).second) {
      throw std::invalid_argument("replica indexes must be unique per workload");
    }
  }

  std::vector<AllocationChange> changes;
  std::vector<PlannedCommand> commands;

  for (std::uint32_t index = 0; index < workload.replicas(); ++index) {
    const auto found = by_replica.find(index);
    if (found == by_replica.end()) {
      Allocation pending(MakeAllocationId(workload.id(), index), workload.id(),
                         index, std::nullopt, workload.resources(),
                         workload.anti_affinity_group(), DesiredState::kPresent,
                         RuntimeState::kPending, workload.generation(), 0, 1);
      changes.emplace_back(AllocationChangeKind::kCreate, std::nullopt,
                           std::move(pending));
      continue;
    }

    const Allocation& allocation = *found->second;
    const bool intent_changed =
        allocation.desired_state() != DesiredState::kPresent ||
        allocation.target_generation() != workload.generation() ||
        allocation.resources() != workload.resources() ||
        allocation.anti_affinity_group() != workload.anti_affinity_group();
    if (intent_changed ||
        RequiresFreshEnsureRevision(allocation, workload.generation())) {
      Allocation updated = PresentIntent(
          workload, allocation, NextRevision(allocation.revision()));
      changes.emplace_back(AllocationChangeKind::kUpdateIntent,
                           allocation.revision(), updated);
      if (updated.node_id().has_value()) {
        commands.push_back(CommandFor(workload, updated,
                                      CommandKind::kEnsureWorkload,
                                      controller_epoch));
      }
      continue;
    }

    if (allocation.node_id().has_value() &&
        !allocation.IsConverged(workload.generation())) {
      // Re-emitting the same id is intentional. A transactional outbox inserts
      // it once; a lost worker response therefore cannot duplicate side effects.
      commands.push_back(CommandFor(workload, allocation,
                                    CommandKind::kEnsureWorkload,
                                    controller_epoch));
    }
  }

  for (const auto& [index, allocation_ptr] : by_replica) {
    if (index < workload.replicas()) {
      continue;
    }
    const Allocation& allocation = *allocation_ptr;
    if (allocation.desired_state() == DesiredState::kPresent ||
        allocation.target_generation() != workload.generation()) {
      Allocation stopped = AbsentIntent(allocation, workload.generation(),
                                        NextRevision(allocation.revision()));
      changes.emplace_back(AllocationChangeKind::kUpdateIntent,
                           allocation.revision(), stopped);
      if (stopped.node_id().has_value()) {
        commands.push_back(CommandFor(workload, stopped,
                                      CommandKind::kStopWorkload,
                                      controller_epoch));
      }
      continue;
    }
    if (allocation.node_id().has_value() &&
        allocation.runtime_state() != RuntimeState::kStopped) {
      commands.push_back(CommandFor(workload, allocation,
                                    CommandKind::kStopWorkload,
                                    controller_epoch));
    }
  }

  return ReconcilePlan(std::move(changes), std::move(commands));
}

std::string MakeAllocationId(const std::string_view workload_id,
                             const std::uint32_t replica_index) {
  if (!IsValidResourceId(workload_id)) {
    throw std::invalid_argument("workload id is invalid");
  }
  const std::string suffix = "-" + std::to_string(replica_index);
  if (workload_id.size() + suffix.size() <= 63) {
    return std::string(workload_id) + suffix;
  }
  const std::string fingerprint = Hex64(Fnv1a64(workload_id));
  const std::size_t prefix_size = 63 - suffix.size() - fingerprint.size() - 1;
  return std::string(workload_id.substr(0, prefix_size)) + "-" + fingerprint +
         suffix;
}

std::string MakeCommandId(const CommandKind kind,
                          const std::string_view allocation_id,
                          const std::uint64_t workload_generation,
                          const std::uint64_t allocation_revision,
                          const std::uint64_t controller_epoch) {
  if (!IsValidResourceId(allocation_id) || workload_generation == 0 ||
      allocation_revision == 0 || controller_epoch == 0) {
    throw std::invalid_argument("cannot build a command id from invalid identity fields");
  }
  return std::string(ToString(kind)) + ":" + std::string(allocation_id) +
         ":g" + std::to_string(workload_generation) + ":r" +
         std::to_string(allocation_revision) + ":e" +
         std::to_string(controller_epoch);
}

const char* ToString(const CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::kEnsureWorkload:
      return "ensure";
    case CommandKind::kStopWorkload:
      return "stop";
  }
  return "unknown";
}

}  // namespace minicloud::core

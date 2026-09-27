#ifndef MINICLOUD_CORE_RECONCILER_HPP
#define MINICLOUD_CORE_RECONCILER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "minicloud/core/domain.hpp"

namespace minicloud::core {

enum class AllocationChangeKind {
  kCreate,
  kUpdateIntent,
};

class AllocationChange final {
 public:
  AllocationChange(AllocationChangeKind kind,
                   std::optional<std::uint64_t> expected_revision,
                   Allocation desired);

  [[nodiscard]] AllocationChangeKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::optional<std::uint64_t>& expected_revision() const noexcept {
    return expected_revision_;
  }
  [[nodiscard]] const Allocation& desired() const noexcept { return desired_; }

  bool operator==(const AllocationChange&) const = default;

 private:
  AllocationChangeKind kind_;
  std::optional<std::uint64_t> expected_revision_;
  Allocation desired_;
};

enum class CommandKind {
  kEnsureWorkload,
  kStopWorkload,
};

class PlannedCommand final {
 public:
  PlannedCommand(std::string command_id, CommandKind kind, std::string worker_id,
                 std::string allocation_id, std::string workload_id,
                 std::uint32_t replica_index, std::uint64_t workload_generation,
                 std::uint64_t allocation_revision,
                 std::uint64_t controller_epoch, std::string spec_digest);

  [[nodiscard]] const std::string& command_id() const noexcept { return command_id_; }
  [[nodiscard]] CommandKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string& worker_id() const noexcept { return worker_id_; }
  [[nodiscard]] const std::string& allocation_id() const noexcept {
    return allocation_id_;
  }
  [[nodiscard]] const std::string& workload_id() const noexcept {
    return workload_id_;
  }
  [[nodiscard]] std::uint32_t replica_index() const noexcept {
    return replica_index_;
  }
  [[nodiscard]] std::uint64_t workload_generation() const noexcept {
    return workload_generation_;
  }
  [[nodiscard]] std::uint64_t allocation_revision() const noexcept {
    return allocation_revision_;
  }
  [[nodiscard]] std::uint64_t controller_epoch() const noexcept {
    return controller_epoch_;
  }
  [[nodiscard]] const std::string& spec_digest() const noexcept {
    return spec_digest_;
  }

  bool operator==(const PlannedCommand&) const = default;

 private:
  std::string command_id_;
  CommandKind kind_;
  std::string worker_id_;
  std::string allocation_id_;
  std::string workload_id_;
  std::uint32_t replica_index_;
  std::uint64_t workload_generation_;
  std::uint64_t allocation_revision_;
  std::uint64_t controller_epoch_;
  std::string spec_digest_;
};

class ReconcilePlan final {
 public:
  ReconcilePlan(std::vector<AllocationChange> allocation_changes,
                std::vector<PlannedCommand> commands);

  [[nodiscard]] const std::vector<AllocationChange>& allocation_changes() const noexcept {
    return allocation_changes_;
  }
  [[nodiscard]] const std::vector<PlannedCommand>& commands() const noexcept {
    return commands_;
  }

  bool operator==(const ReconcilePlan&) const = default;

 private:
  std::vector<AllocationChange> allocation_changes_;
  std::vector<PlannedCommand> commands_;
};

class Reconciler final {
 public:
  // Plan is pure: equal snapshots produce byte-for-byte equal command ids and
  // ordering. Persistence applies allocation changes with expected_revision and
  // inserts commands under a unique command_id in one transaction.
  [[nodiscard]] static ReconcilePlan Plan(
      const WorkloadSpec& workload, const std::vector<Allocation>& current,
      std::uint64_t controller_epoch);
};

[[nodiscard]] std::string MakeAllocationId(std::string_view workload_id,
                                           std::uint32_t replica_index);
[[nodiscard]] std::string MakeCommandId(CommandKind kind,
                                        std::string_view allocation_id,
                                        std::uint64_t workload_generation,
                                        std::uint64_t allocation_revision,
                                        std::uint64_t controller_epoch);
[[nodiscard]] const char* ToString(CommandKind kind) noexcept;

}  // namespace minicloud::core

#endif  // MINICLOUD_CORE_RECONCILER_HPP

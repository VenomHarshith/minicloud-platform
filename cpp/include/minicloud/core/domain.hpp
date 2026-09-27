#ifndef MINICLOUD_CORE_DOMAIN_HPP
#define MINICLOUD_CORE_DOMAIN_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace minicloud::core {

using Labels = std::map<std::string, std::string>;

// A deliberately bounded resource pair. The upper bound keeps score arithmetic
// portable and overflow-free on both MSVC and Unix toolchains.
class Resources final {
 public:
  static constexpr std::uint64_t kMaximumQuantity = 1'000'000'000'000ULL;

  Resources(std::uint64_t cpu_millis, std::uint64_t memory_bytes);

  [[nodiscard]] std::uint64_t cpu_millis() const noexcept { return cpu_millis_; }
  [[nodiscard]] std::uint64_t memory_bytes() const noexcept { return memory_bytes_; }
  [[nodiscard]] bool FitsWithin(const Resources& capacity) const noexcept;

  bool operator==(const Resources&) const = default;

 private:
  std::uint64_t cpu_millis_;
  std::uint64_t memory_bytes_;
};

enum class DesiredState {
  kPresent,
  kAbsent,
};

enum class RuntimeState {
  kUnknown,
  kPending,
  kStarting,
  kRunning,
  kStopping,
  kStopped,
  kFailed,
};

class Node final {
 public:
  Node(std::string id, Resources capacity, Labels labels, bool schedulable,
       std::uint64_t worker_epoch);

  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] const Resources& capacity() const noexcept { return capacity_; }
  [[nodiscard]] const Labels& labels() const noexcept { return labels_; }
  [[nodiscard]] bool schedulable() const noexcept { return schedulable_; }
  [[nodiscard]] std::uint64_t worker_epoch() const noexcept { return worker_epoch_; }

  bool operator==(const Node&) const = default;

 private:
  std::string id_;
  Resources capacity_;
  Labels labels_;
  bool schedulable_;
  std::uint64_t worker_epoch_;
};

class WorkloadSpec final {
 public:
  WorkloadSpec(std::string id, std::uint64_t generation, std::uint32_t replicas,
               Resources resources, Labels required_labels,
               std::optional<std::string> anti_affinity_group,
               std::string spec_digest);

  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] std::uint32_t replicas() const noexcept { return replicas_; }
  [[nodiscard]] const Resources& resources() const noexcept { return resources_; }
  [[nodiscard]] const Labels& required_labels() const noexcept {
    return required_labels_;
  }
  [[nodiscard]] const std::optional<std::string>& anti_affinity_group() const noexcept {
    return anti_affinity_group_;
  }
  [[nodiscard]] const std::string& spec_digest() const noexcept {
    return spec_digest_;
  }

  bool operator==(const WorkloadSpec&) const = default;

 private:
  std::string id_;
  std::uint64_t generation_;
  std::uint32_t replicas_;
  Resources resources_;
  Labels required_labels_;
  std::optional<std::string> anti_affinity_group_;
  std::string spec_digest_;
};

class PlacementRequest final {
 public:
  PlacementRequest(std::string allocation_id, std::string workload_id,
                   std::uint32_t replica_index, Resources resources,
                   Labels required_labels,
                   std::optional<std::string> anti_affinity_group);

  [[nodiscard]] const std::string& allocation_id() const noexcept {
    return allocation_id_;
  }
  [[nodiscard]] const std::string& workload_id() const noexcept {
    return workload_id_;
  }
  [[nodiscard]] std::uint32_t replica_index() const noexcept {
    return replica_index_;
  }
  [[nodiscard]] const Resources& resources() const noexcept { return resources_; }
  [[nodiscard]] const Labels& required_labels() const noexcept {
    return required_labels_;
  }
  [[nodiscard]] const std::optional<std::string>& anti_affinity_group() const noexcept {
    return anti_affinity_group_;
  }

  bool operator==(const PlacementRequest&) const = default;

 private:
  std::string allocation_id_;
  std::string workload_id_;
  std::uint32_t replica_index_;
  Resources resources_;
  Labels required_labels_;
  std::optional<std::string> anti_affinity_group_;
};

// Allocation is a value snapshot. Private fields and the absence of setters make
// a snapshot immutable after construction; a state transition creates a new one.
class Allocation final {
 public:
  Allocation(std::string id, std::string workload_id, std::uint32_t replica_index,
             std::optional<std::string> node_id, Resources resources,
             std::optional<std::string> anti_affinity_group,
             DesiredState desired_state, RuntimeState runtime_state,
             std::uint64_t target_generation,
             std::uint64_t observed_generation, std::uint64_t revision);

  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] const std::string& workload_id() const noexcept {
    return workload_id_;
  }
  [[nodiscard]] std::uint32_t replica_index() const noexcept {
    return replica_index_;
  }
  [[nodiscard]] const std::optional<std::string>& node_id() const noexcept {
    return node_id_;
  }
  [[nodiscard]] const Resources& resources() const noexcept { return resources_; }
  [[nodiscard]] const std::optional<std::string>& anti_affinity_group() const noexcept {
    return anti_affinity_group_;
  }
  [[nodiscard]] DesiredState desired_state() const noexcept { return desired_state_; }
  [[nodiscard]] RuntimeState runtime_state() const noexcept { return runtime_state_; }
  [[nodiscard]] std::uint64_t target_generation() const noexcept {
    return target_generation_;
  }
  [[nodiscard]] std::uint64_t observed_generation() const noexcept {
    return observed_generation_;
  }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] bool ReservesResources() const noexcept;
  [[nodiscard]] bool IsConverged(std::uint64_t generation) const noexcept;

  bool operator==(const Allocation&) const = default;

 private:
  std::string id_;
  std::string workload_id_;
  std::uint32_t replica_index_;
  std::optional<std::string> node_id_;
  Resources resources_;
  std::optional<std::string> anti_affinity_group_;
  DesiredState desired_state_;
  RuntimeState runtime_state_;
  std::uint64_t target_generation_;
  std::uint64_t observed_generation_;
  std::uint64_t revision_;
};

// Resource identifiers use the portable subset also accepted by DNS labels.
[[nodiscard]] bool IsValidResourceId(std::string_view value) noexcept;

}  // namespace minicloud::core

#endif  // MINICLOUD_CORE_DOMAIN_HPP

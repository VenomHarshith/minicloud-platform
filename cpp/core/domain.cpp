#include "minicloud/core/domain.hpp"

#include <stdexcept>
#include <utility>

namespace minicloud::core {
namespace {

constexpr std::size_t kMaximumLabels = 64;
constexpr std::size_t kMaximumLabelPartBytes = 128;
constexpr std::uint32_t kMaximumReplicas = 10'000;

void RequireResourceId(std::string_view value, std::string_view field) {
  if (!IsValidResourceId(value)) {
    throw std::invalid_argument(std::string(field) +
                                " must be a lowercase DNS-style resource id");
  }
}

void RequireSafeText(std::string_view value, std::string_view field,
                     std::size_t maximum_size) {
  if (value.empty() || value.size() > maximum_size) {
    throw std::invalid_argument(std::string(field) + " has an invalid length");
  }
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character < 0x20U || character == 0x7FU) {
      throw std::invalid_argument(std::string(field) + " contains a control character");
    }
  }
}

void ValidateLabels(const Labels& labels, std::string_view field) {
  if (labels.size() > kMaximumLabels) {
    throw std::invalid_argument(std::string(field) + " contains too many labels");
  }
  for (const auto& [key, value] : labels) {
    RequireSafeText(key, std::string(field) + " key", kMaximumLabelPartBytes);
    RequireSafeText(value, std::string(field) + " value", kMaximumLabelPartBytes);
  }
}

void ValidateAffinity(const std::optional<std::string>& group) {
  if (group.has_value()) {
    RequireResourceId(*group, "anti_affinity_group");
  }
}

}  // namespace

Resources::Resources(const std::uint64_t cpu_millis,
                     const std::uint64_t memory_bytes)
    : cpu_millis_(cpu_millis), memory_bytes_(memory_bytes) {
  if (cpu_millis == 0 || memory_bytes == 0) {
    throw std::invalid_argument("resource quantities must be positive");
  }
  if (cpu_millis > kMaximumQuantity || memory_bytes > kMaximumQuantity) {
    throw std::invalid_argument("resource quantity exceeds the portable upper bound");
  }
}

bool Resources::FitsWithin(const Resources& capacity) const noexcept {
  return cpu_millis_ <= capacity.cpu_millis_ &&
         memory_bytes_ <= capacity.memory_bytes_;
}

Node::Node(std::string id, Resources capacity, Labels labels,
           const bool schedulable, const std::uint64_t worker_epoch)
    : id_(std::move(id)),
      capacity_(capacity),
      labels_(std::move(labels)),
      schedulable_(schedulable),
      worker_epoch_(worker_epoch) {
  RequireResourceId(id_, "node id");
  ValidateLabels(labels_, "node labels");
  if (worker_epoch_ == 0) {
    throw std::invalid_argument("worker_epoch must be positive");
  }
}

WorkloadSpec::WorkloadSpec(
    std::string id, const std::uint64_t generation,
    const std::uint32_t replicas, Resources resources, Labels required_labels,
    std::optional<std::string> anti_affinity_group, std::string spec_digest)
    : id_(std::move(id)),
      generation_(generation),
      replicas_(replicas),
      resources_(resources),
      required_labels_(std::move(required_labels)),
      anti_affinity_group_(std::move(anti_affinity_group)),
      spec_digest_(std::move(spec_digest)) {
  RequireResourceId(id_, "workload id");
  if (generation_ == 0) {
    throw std::invalid_argument("generation must be positive");
  }
  if (replicas_ > kMaximumReplicas) {
    throw std::invalid_argument("replicas exceeds the supported upper bound");
  }
  ValidateLabels(required_labels_, "required labels");
  ValidateAffinity(anti_affinity_group_);
  RequireSafeText(spec_digest_, "spec_digest", 256);
}

PlacementRequest::PlacementRequest(
    std::string allocation_id, std::string workload_id,
    const std::uint32_t replica_index, Resources resources,
    Labels required_labels, std::optional<std::string> anti_affinity_group)
    : allocation_id_(std::move(allocation_id)),
      workload_id_(std::move(workload_id)),
      replica_index_(replica_index),
      resources_(resources),
      required_labels_(std::move(required_labels)),
      anti_affinity_group_(std::move(anti_affinity_group)) {
  RequireResourceId(allocation_id_, "allocation id");
  RequireResourceId(workload_id_, "workload id");
  ValidateLabels(required_labels_, "required labels");
  ValidateAffinity(anti_affinity_group_);
}

Allocation::Allocation(
    std::string id, std::string workload_id, const std::uint32_t replica_index,
    std::optional<std::string> node_id, Resources resources,
    std::optional<std::string> anti_affinity_group,
    const DesiredState desired_state, const RuntimeState runtime_state,
    const std::uint64_t target_generation,
    const std::uint64_t observed_generation, const std::uint64_t revision)
    : id_(std::move(id)),
      workload_id_(std::move(workload_id)),
      replica_index_(replica_index),
      node_id_(std::move(node_id)),
      resources_(resources),
      anti_affinity_group_(std::move(anti_affinity_group)),
      desired_state_(desired_state),
      runtime_state_(runtime_state),
      target_generation_(target_generation),
      observed_generation_(observed_generation),
      revision_(revision) {
  RequireResourceId(id_, "allocation id");
  RequireResourceId(workload_id_, "workload id");
  if (node_id_.has_value()) {
    RequireResourceId(*node_id_, "node id");
  }
  ValidateAffinity(anti_affinity_group_);
  if (target_generation_ == 0) {
    throw std::invalid_argument("target_generation must be positive");
  }
  if (observed_generation_ > target_generation_) {
    throw std::invalid_argument("observed_generation cannot exceed target_generation");
  }
  if (revision_ == 0) {
    throw std::invalid_argument("allocation revision must be positive");
  }
  if (!node_id_.has_value() &&
      (runtime_state_ == RuntimeState::kStarting ||
       runtime_state_ == RuntimeState::kRunning ||
       runtime_state_ == RuntimeState::kStopping)) {
    throw std::invalid_argument("an active runtime state requires an assigned node");
  }
}

bool Allocation::ReservesResources() const noexcept {
  return desired_state_ == DesiredState::kPresent && node_id_.has_value();
}

bool Allocation::IsConverged(const std::uint64_t generation) const noexcept {
  return desired_state_ == DesiredState::kPresent &&
         runtime_state_ == RuntimeState::kRunning &&
         target_generation_ == generation && observed_generation_ == generation;
}

bool IsValidResourceId(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 63) {
    return false;
  }
  const auto is_lower_or_digit = [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  if (value.front() < 'a' || value.front() > 'z' ||
      !is_lower_or_digit(value.back())) {
    return false;
  }
  for (const char character : value) {
    if (!is_lower_or_digit(character) && character != '-') {
      return false;
    }
  }
  return true;
}

}  // namespace minicloud::core

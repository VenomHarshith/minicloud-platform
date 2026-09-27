#include "minicloud/core/scheduler.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace minicloud::core {
namespace {

constexpr std::uint64_t kPartsPerMillion = 1'000'000ULL;

struct Usage final {
  std::uint64_t cpu_millis = 0;
  std::uint64_t memory_bytes = 0;
};

std::uint64_t CheckedAdd(const std::uint64_t left, const std::uint64_t right) {
  if (right > Resources::kMaximumQuantity - left) {
    throw std::overflow_error("aggregate resource usage exceeds the portable bound");
  }
  return left + right;
}

std::uint64_t UtilizationPpm(const std::uint64_t used,
                             const std::uint64_t capacity) {
  // Resources bounds guarantee that multiplication cannot overflow uint64_t.
  return (used * kPartsPerMillion) / capacity;
}

bool LabelsMatch(const Labels& required, const Labels& available) {
  return std::all_of(required.begin(), required.end(), [&](const auto& item) {
    const auto found = available.find(item.first);
    return found != available.end() && found->second == item.second;
  });
}

IneligibleReason Eligibility(const Node& node, const Usage& used,
                             const PlacementRequest& request) {
  if (!node.schedulable()) {
    return IneligibleReason::kNodeUnschedulable;
  }
  if (!LabelsMatch(request.required_labels(), node.labels())) {
    return IneligibleReason::kRequiredLabelMismatch;
  }
  if (used.cpu_millis > node.capacity().cpu_millis() ||
      request.resources().cpu_millis() >
          node.capacity().cpu_millis() - used.cpu_millis) {
    return IneligibleReason::kInsufficientCpu;
  }
  if (used.memory_bytes > node.capacity().memory_bytes() ||
      request.resources().memory_bytes() >
          node.capacity().memory_bytes() - used.memory_bytes) {
    return IneligibleReason::kInsufficientMemory;
  }
  return IneligibleReason::kNone;
}

}  // namespace

CandidateEvaluation::CandidateEvaluation(
    std::string node_id, const IneligibleReason reason,
    const std::uint64_t anti_affinity_conflicts,
    const std::uint64_t dominant_utilization_ppm,
    const std::uint64_t total_utilization_ppm)
    : node_id_(std::move(node_id)),
      reason_(reason),
      anti_affinity_conflicts_(anti_affinity_conflicts),
      dominant_utilization_ppm_(dominant_utilization_ppm),
      total_utilization_ppm_(total_utilization_ppm) {}

SchedulingDecision::SchedulingDecision(
    std::optional<std::string> selected_node_id,
    std::vector<CandidateEvaluation> candidates)
    : selected_node_id_(std::move(selected_node_id)),
      candidates_(std::move(candidates)) {}

SchedulingDecision Scheduler::SelectNode(
    const PlacementRequest& request, const std::vector<Node>& nodes,
    const std::vector<Allocation>& allocations) {
  std::map<std::string, const Node*> node_by_id;
  for (const Node& node : nodes) {
    if (!node_by_id.emplace(node.id(), &node).second) {
      throw std::invalid_argument("node ids must be unique");
    }
  }

  std::map<std::string, Usage> usage;
  std::map<std::string, std::uint64_t> affinity_conflicts;
  std::set<std::string> allocation_ids;
  for (const Allocation& allocation : allocations) {
    if (!allocation_ids.insert(allocation.id()).second) {
      throw std::invalid_argument("allocation ids must be unique");
    }
    // A retry may ask to place the same allocation again. Excluding its old
    // reservation makes the operation idempotent rather than double-counting it.
    if (allocation.id() == request.allocation_id() ||
        !allocation.ReservesResources()) {
      continue;
    }
    const std::string& node_id = *allocation.node_id();
    if (node_by_id.find(node_id) == node_by_id.end()) {
      continue;  // The missing node cannot be selected and consumes no live capacity.
    }
    Usage& node_usage = usage[node_id];
    node_usage.cpu_millis =
        CheckedAdd(node_usage.cpu_millis, allocation.resources().cpu_millis());
    node_usage.memory_bytes =
        CheckedAdd(node_usage.memory_bytes, allocation.resources().memory_bytes());
    if (request.anti_affinity_group().has_value() &&
        allocation.anti_affinity_group() == request.anti_affinity_group()) {
      ++affinity_conflicts[node_id];
    }
  }

  std::vector<CandidateEvaluation> candidates;
  candidates.reserve(node_by_id.size());
  using Rank =
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::string>;
  std::optional<Rank> best_rank;
  std::optional<std::string> selected;

  for (const auto& [node_id, node] : node_by_id) {
    const Usage used = usage[node_id];
    const IneligibleReason reason = Eligibility(*node, used, request);
    if (reason != IneligibleReason::kNone) {
      candidates.emplace_back(node_id, reason, affinity_conflicts[node_id], 0, 0);
      continue;
    }

    const std::uint64_t after_cpu =
        CheckedAdd(used.cpu_millis, request.resources().cpu_millis());
    const std::uint64_t after_memory =
        CheckedAdd(used.memory_bytes, request.resources().memory_bytes());
    const std::uint64_t cpu_ppm =
        UtilizationPpm(after_cpu, node->capacity().cpu_millis());
    const std::uint64_t memory_ppm =
        UtilizationPpm(after_memory, node->capacity().memory_bytes());
    const std::uint64_t dominant_ppm = std::max(cpu_ppm, memory_ppm);
    const std::uint64_t total_ppm = cpu_ppm + memory_ppm;
    const std::uint64_t conflicts = affinity_conflicts[node_id];
    candidates.emplace_back(node_id, reason, conflicts, dominant_ppm, total_ppm);

    Rank rank(conflicts, dominant_ppm, total_ppm, node_id);
    if (!best_rank.has_value() || rank < *best_rank) {
      best_rank = std::move(rank);
      selected = node_id;
    }
  }

  return SchedulingDecision(std::move(selected), std::move(candidates));
}

const char* ToString(const IneligibleReason reason) noexcept {
  switch (reason) {
    case IneligibleReason::kNone:
      return "eligible";
    case IneligibleReason::kNodeUnschedulable:
      return "node_unschedulable";
    case IneligibleReason::kRequiredLabelMismatch:
      return "required_label_mismatch";
    case IneligibleReason::kInsufficientCpu:
      return "insufficient_cpu";
    case IneligibleReason::kInsufficientMemory:
      return "insufficient_memory";
  }
  return "unknown";
}

}  // namespace minicloud::core

#ifndef MINICLOUD_CORE_SCHEDULER_HPP
#define MINICLOUD_CORE_SCHEDULER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "minicloud/core/domain.hpp"

namespace minicloud::core {

enum class IneligibleReason {
  kNone,
  kNodeUnschedulable,
  kRequiredLabelMismatch,
  kInsufficientCpu,
  kInsufficientMemory,
};

class CandidateEvaluation final {
 public:
  CandidateEvaluation(std::string node_id, IneligibleReason reason,
                      std::uint64_t anti_affinity_conflicts,
                      std::uint64_t dominant_utilization_ppm,
                      std::uint64_t total_utilization_ppm);

  [[nodiscard]] const std::string& node_id() const noexcept { return node_id_; }
  [[nodiscard]] bool eligible() const noexcept {
    return reason_ == IneligibleReason::kNone;
  }
  [[nodiscard]] IneligibleReason reason() const noexcept { return reason_; }
  [[nodiscard]] std::uint64_t anti_affinity_conflicts() const noexcept {
    return anti_affinity_conflicts_;
  }
  [[nodiscard]] std::uint64_t dominant_utilization_ppm() const noexcept {
    return dominant_utilization_ppm_;
  }
  [[nodiscard]] std::uint64_t total_utilization_ppm() const noexcept {
    return total_utilization_ppm_;
  }

  bool operator==(const CandidateEvaluation&) const = default;

 private:
  std::string node_id_;
  IneligibleReason reason_;
  std::uint64_t anti_affinity_conflicts_;
  std::uint64_t dominant_utilization_ppm_;
  std::uint64_t total_utilization_ppm_;
};

class SchedulingDecision final {
 public:
  SchedulingDecision(std::optional<std::string> selected_node_id,
                     std::vector<CandidateEvaluation> candidates);

  [[nodiscard]] const std::optional<std::string>& selected_node_id() const noexcept {
    return selected_node_id_;
  }
  [[nodiscard]] const std::vector<CandidateEvaluation>& candidates() const noexcept {
    return candidates_;
  }

  bool operator==(const SchedulingDecision&) const = default;

 private:
  std::optional<std::string> selected_node_id_;
  std::vector<CandidateEvaluation> candidates_;
};

class Scheduler final {
 public:
  // Selection is independent of input ordering. Eligible nodes are ranked by:
  // (1) fewest soft anti-affinity conflicts, (2) lowest dominant post-placement
  // utilization, (3) lowest total utilization, and (4) lexical node id.
  [[nodiscard]] static SchedulingDecision SelectNode(
      const PlacementRequest& request, const std::vector<Node>& nodes,
      const std::vector<Allocation>& allocations);
};

[[nodiscard]] const char* ToString(IneligibleReason reason) noexcept;

}  // namespace minicloud::core

#endif  // MINICLOUD_CORE_SCHEDULER_HPP

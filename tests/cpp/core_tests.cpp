#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "minicloud/core/domain.hpp"
#include "minicloud/core/reconciler.hpp"
#include "minicloud/core/scheduler.hpp"

namespace {

using minicloud::core::Allocation;
using minicloud::core::AllocationChangeKind;
using minicloud::core::CandidateEvaluation;
using minicloud::core::CommandKind;
using minicloud::core::DesiredState;
using minicloud::core::IneligibleReason;
using minicloud::core::Labels;
using minicloud::core::MakeAllocationId;
using minicloud::core::Node;
using minicloud::core::PlacementRequest;
using minicloud::core::Reconciler;
using minicloud::core::Resources;
using minicloud::core::RuntimeState;
using minicloud::core::Scheduler;
using minicloud::core::SchedulingDecision;
using minicloud::core::WorkloadSpec;

class TestFailure final : public std::runtime_error {
 public:
  explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      throw TestFailure(std::string("check failed: ") + #condition + " at " +  \
                        __FILE__ + ":" + std::to_string(__LINE__));              \
    }                                                                           \
  } while (false)

template <typename Exception, typename Function>
void CheckThrows(Function&& function) {
  try {
    std::forward<Function>(function)();
  } catch (const Exception&) {
    return;
  }
  throw TestFailure("expected exception was not thrown");
}

constexpr std::uint64_t MiB(const std::uint64_t value) {
  return value * 1024ULL * 1024ULL;
}

Resources SmallResources() { return Resources(100, MiB(64)); }

Allocation Assigned(std::string id, const std::uint32_t replica,
                    std::string node_id, Resources resources,
                    std::optional<std::string> affinity = std::nullopt,
                    RuntimeState state = RuntimeState::kRunning,
                    std::uint64_t target_generation = 1,
                    std::uint64_t observed_generation = 1,
                    std::uint64_t revision = 1,
                    DesiredState desired = DesiredState::kPresent) {
  return Allocation(std::move(id), "api", replica, std::move(node_id), resources,
                    std::move(affinity), desired, state, target_generation,
                    observed_generation, revision);
}

const CandidateEvaluation& Candidate(const SchedulingDecision& decision,
                                     const std::string& node_id) {
  const auto found = std::find_if(
      decision.candidates().begin(), decision.candidates().end(),
      [&](const CandidateEvaluation& candidate) {
        return candidate.node_id() == node_id;
      });
  if (found == decision.candidates().end()) {
    throw TestFailure("candidate not found: " + node_id);
  }
  return *found;
}

void DomainModelsRejectInvalidIdentityAndResources() {
  CheckThrows<std::invalid_argument>([] { Resources invalid(0, MiB(1)); });
  CheckThrows<std::invalid_argument>([] {
    Node invalid("UPPERCASE", Resources(100, MiB(1)), {}, true, 1);
  });
  CheckThrows<std::invalid_argument>([] {
    WorkloadSpec invalid("api", 0, 1, SmallResources(), {}, std::nullopt,
                         "sha256:abc");
  });
  CheckThrows<std::invalid_argument>([] {
    Allocation invalid("api-0", "api", 0, std::nullopt, SmallResources(),
                       std::nullopt, DesiredState::kPresent,
                       RuntimeState::kRunning, 1, 1, 1);
  });
  CheckThrows<std::invalid_argument>([] {
    Allocation invalid("api-0", "api", 0, std::string("node-a"),
                       SmallResources(), std::nullopt,
                       DesiredState::kPresent, RuntimeState::kRunning, 1, 2, 1);
  });
  CHECK(minicloud::core::IsValidResourceId("worker-a1"));
  CHECK(!minicloud::core::IsValidResourceId("worker_a"));
}

void SchedulerIsIndependentOfInputOrderAndUsesLexicalTieBreak() {
  std::vector<Node> nodes;
  nodes.emplace_back("node-b", Resources(1000, MiB(1024)), Labels{}, true, 1);
  nodes.emplace_back("node-a", Resources(1000, MiB(1024)), Labels{}, true, 1);
  PlacementRequest request("api-0", "api", 0, SmallResources(), {}, std::nullopt);

  const SchedulingDecision first = Scheduler::SelectNode(request, nodes, {});
  std::reverse(nodes.begin(), nodes.end());
  const SchedulingDecision second = Scheduler::SelectNode(request, nodes, {});

  CHECK(first == second);
  CHECK(first.selected_node_id() == std::optional<std::string>("node-a"));
  CHECK(first.candidates().front().node_id() == "node-a");
}

void SchedulerEnforcesLabelsCpuAndMemory() {
  const Labels linux{{"os", "linux"}};
  std::vector<Node> nodes;
  nodes.emplace_back("cpu-full", Resources(1000, MiB(1024)), linux, true, 1);
  nodes.emplace_back("label-wrong", Resources(1000, MiB(1024)),
                     Labels{{"os", "windows"}}, true, 1);
  nodes.emplace_back("memory-full", Resources(1000, MiB(100)), linux, true, 1);
  nodes.emplace_back("unschedulable", Resources(1000, MiB(1024)), linux, false, 1);

  std::vector<Allocation> current;
  current.push_back(Assigned("api-old", 8, "cpu-full", Resources(950, MiB(1))));
  PlacementRequest request("api-0", "api", 0, Resources(100, MiB(128)), linux,
                           std::nullopt);

  const SchedulingDecision decision = Scheduler::SelectNode(request, nodes, current);
  CHECK(!decision.selected_node_id().has_value());
  CHECK(Candidate(decision, "cpu-full").reason() ==
        IneligibleReason::kInsufficientCpu);
  CHECK(Candidate(decision, "label-wrong").reason() ==
        IneligibleReason::kRequiredLabelMismatch);
  CHECK(Candidate(decision, "memory-full").reason() ==
        IneligibleReason::kInsufficientMemory);
  CHECK(Candidate(decision, "unschedulable").reason() ==
        IneligibleReason::kNodeUnschedulable);
}

void SchedulerPrefersSoftAntiAffinityBeforeUtilization() {
  std::vector<Node> nodes;
  nodes.emplace_back("node-a", Resources(1000, MiB(1000)), Labels{}, true, 1);
  nodes.emplace_back("node-b", Resources(1000, MiB(1000)), Labels{}, true, 1);

  std::vector<Allocation> current;
  current.push_back(Assigned("api-7", 7, "node-a", Resources(50, MiB(50)),
                             std::string("api-spread")));
  current.push_back(Assigned("api-8", 8, "node-b", Resources(700, MiB(700)),
                             std::string("other-group")));
  PlacementRequest request("api-0", "api", 0, Resources(100, MiB(100)), {},
                           std::string("api-spread"));

  const SchedulingDecision decision = Scheduler::SelectNode(request, nodes, current);
  CHECK(decision.selected_node_id() == std::optional<std::string>("node-b"));
  CHECK(Candidate(decision, "node-a").anti_affinity_conflicts() == 1);
  CHECK(Candidate(decision, "node-b").anti_affinity_conflicts() == 0);
}

void SchedulerDoesNotDoubleCountTheAllocationBeingReplaced() {
  std::vector<Node> nodes;
  nodes.emplace_back("node-a", Resources(100, MiB(100)), Labels{}, true, 1);
  std::vector<Allocation> current;
  current.push_back(Assigned("api-0", 0, "node-a", Resources(100, MiB(100))));
  PlacementRequest request("api-0", "api", 0, Resources(100, MiB(100)), {},
                           std::nullopt);

  const SchedulingDecision decision = Scheduler::SelectNode(request, nodes, current);
  CHECK(decision.selected_node_id() == std::optional<std::string>("node-a"));
}

void ReconcilerCreatesDeterministicPendingAllocations() {
  const WorkloadSpec workload("api", 1, 2, SmallResources(), {},
                              std::string("api-spread"), "sha256:v1");
  const auto first = Reconciler::Plan(workload, {}, 7);
  const auto second = Reconciler::Plan(workload, {}, 7);

  CHECK(first == second);
  CHECK(first.allocation_changes().size() == 2);
  CHECK(first.commands().empty());
  CHECK(first.allocation_changes()[0].kind() == AllocationChangeKind::kCreate);
  CHECK(first.allocation_changes()[0].desired().id() == "api-0");
  CHECK(first.allocation_changes()[1].desired().id() == "api-1");
  CHECK(first.allocation_changes()[0].desired().runtime_state() ==
        RuntimeState::kPending);
}

void ReconcilerReemitsAnIdempotentEnsureCommand() {
  const WorkloadSpec workload("api", 1, 1, SmallResources(), {},
                              std::nullopt, "sha256:v1");
  const Allocation starting = Assigned(
      "api-0", 0, "node-a", SmallResources(), std::nullopt,
      RuntimeState::kStarting, 1, 0, 3);

  const auto first = Reconciler::Plan(workload, {starting}, 11);
  const auto second = Reconciler::Plan(workload, {starting}, 11);
  CHECK(first == second);
  CHECK(first.allocation_changes().empty());
  CHECK(first.commands().size() == 1);
  CHECK(first.commands()[0].kind() == CommandKind::kEnsureWorkload);
  CHECK(first.commands()[0].command_id() == "ensure:api-0:g1:r3:e11");
  CHECK(first.commands()[0].spec_digest() == "sha256:v1");
}

void ReconcilerDoesNothingForConvergedAllocation() {
  const WorkloadSpec workload("api", 1, 1, SmallResources(), {},
                              std::nullopt, "sha256:v1");
  const Allocation running = Assigned("api-0", 0, "node-a", SmallResources());

  const auto plan = Reconciler::Plan(workload, {running}, 1);
  CHECK(plan.allocation_changes().empty());
  CHECK(plan.commands().empty());
}

void ReconcilerBumpsRevisionForNewGeneration() {
  const WorkloadSpec workload("api", 2, 1, Resources(200, MiB(128)), {},
                              std::nullopt, "sha256:v2");
  const Allocation running = Assigned("api-0", 0, "node-a", SmallResources(),
                                       std::nullopt, RuntimeState::kRunning,
                                       1, 1, 9);

  const auto plan = Reconciler::Plan(workload, {running}, 4);
  CHECK(plan.allocation_changes().size() == 1);
  CHECK(plan.allocation_changes()[0].expected_revision() ==
        std::optional<std::uint64_t>(9));
  CHECK(plan.allocation_changes()[0].desired().revision() == 10);
  CHECK(plan.allocation_changes()[0].desired().target_generation() == 2);
  CHECK(plan.allocation_changes()[0].desired().resources() == workload.resources());
  CHECK(plan.commands().size() == 1);
  CHECK(plan.commands()[0].command_id() == "ensure:api-0:g2:r10:e4");
}

void ReconcilerCreatesFencedStopWhenScalingDown() {
  const WorkloadSpec workload("api", 2, 1, SmallResources(), {},
                              std::nullopt, "sha256:v2");
  const Allocation replica_zero = Assigned(
      "api-0", 0, "node-a", SmallResources(), std::nullopt,
      RuntimeState::kRunning, 2, 2, 3);
  const Allocation replica_one = Assigned(
      "api-1", 1, "node-b", SmallResources(), std::nullopt,
      RuntimeState::kRunning, 1, 1, 6);

  const auto plan = Reconciler::Plan(workload, {replica_one, replica_zero}, 12);
  CHECK(plan.allocation_changes().size() == 1);
  const Allocation& absent = plan.allocation_changes()[0].desired();
  CHECK(absent.replica_index() == 1);
  CHECK(absent.desired_state() == DesiredState::kAbsent);
  CHECK(absent.runtime_state() == RuntimeState::kStopping);
  CHECK(absent.target_generation() == 2);
  CHECK(absent.revision() == 7);
  CHECK(plan.commands().size() == 1);
  CHECK(plan.commands()[0].kind() == CommandKind::kStopWorkload);
  CHECK(plan.commands()[0].command_id() == "stop:api-1:g2:r7:e12");
  CHECK(plan.commands()[0].spec_digest().empty());
}

void ReconcilerUsesNewRevisionToRecoverFailedWorkload() {
  const WorkloadSpec workload("api", 1, 1, SmallResources(), {},
                              std::nullopt, "sha256:v1");
  const Allocation failed = Assigned(
      "api-0", 0, "node-a", SmallResources(), std::nullopt,
      RuntimeState::kFailed, 1, 1, 5);

  const auto plan = Reconciler::Plan(workload, {failed}, 2);
  CHECK(plan.allocation_changes().size() == 1);
  CHECK(plan.allocation_changes()[0].desired().revision() == 6);
  CHECK(plan.commands().size() == 1);
  CHECK(plan.commands()[0].command_id() == "ensure:api-0:g1:r6:e2");
}

void LongWorkloadIdsProduceBoundedStableAllocationIds() {
  const std::string workload_id = "a" + std::string(62, 'x');
  CHECK(workload_id.size() == 63);
  const std::string first = MakeAllocationId(workload_id, 9999);
  const std::string second = MakeAllocationId(workload_id, 9999);
  CHECK(first == second);
  CHECK(first.size() <= 63);
  CHECK(minicloud::core::IsValidResourceId(first));
  CHECK(first != MakeAllocationId(workload_id, 9998));
}

void ReconcilerRejectsAmbiguousSnapshots() {
  const WorkloadSpec workload("api", 1, 1, SmallResources(), {},
                              std::nullopt, "sha256:v1");
  const Allocation first = Assigned("api-0", 0, "node-a", SmallResources());
  const Allocation duplicate = Assigned("api-copy", 0, "node-b", SmallResources());
  CheckThrows<std::invalid_argument>([&] {
    static_cast<void>(Reconciler::Plan(workload, {first, duplicate}, 1));
  });
}

}  // namespace

int main() {
  using Test = std::pair<const char*, std::function<void()>>;
  const std::vector<Test> tests{
      {"domain validation", DomainModelsRejectInvalidIdentityAndResources},
      {"deterministic scheduler", SchedulerIsIndependentOfInputOrderAndUsesLexicalTieBreak},
      {"scheduler constraints", SchedulerEnforcesLabelsCpuAndMemory},
      {"scheduler anti-affinity", SchedulerPrefersSoftAntiAffinityBeforeUtilization},
      {"scheduler replacement", SchedulerDoesNotDoubleCountTheAllocationBeingReplaced},
      {"reconcile creates", ReconcilerCreatesDeterministicPendingAllocations},
      {"reconcile idempotency", ReconcilerReemitsAnIdempotentEnsureCommand},
      {"reconcile converged", ReconcilerDoesNothingForConvergedAllocation},
      {"reconcile generation", ReconcilerBumpsRevisionForNewGeneration},
      {"reconcile scale down", ReconcilerCreatesFencedStopWhenScalingDown},
      {"reconcile failed", ReconcilerUsesNewRevisionToRecoverFailedWorkload},
      {"bounded allocation ids", LongWorkloadIdsProduceBoundedStableAllocationIds},
      {"ambiguous snapshot", ReconcilerRejectsAmbiguousSnapshots},
  };

  std::size_t passed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      return 1;
    } catch (...) {
      std::cerr << "FAIL " << name << ": unknown exception\n";
      return 1;
    }
  }
  std::cout << "PASS " << passed << " C++ core tests\n";
  return 0;
}

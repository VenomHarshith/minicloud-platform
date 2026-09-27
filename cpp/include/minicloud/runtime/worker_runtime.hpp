#pragma once

#include "minicloud/runtime/docker_client.hpp"
#include "minicloud/runtime/metrics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace minicloud::runtime {

enum class RuntimeCommandKind {
  Ensure,
  Stop,
  Remove,
};

enum class WorkloadState {
  Stopped,
  Starting,
  Healthy,
  Backoff,
  Failed,
  Stopping,
  Removed,
};

struct RuntimeRestartPolicy {
  std::chrono::milliseconds initial_delay{250};
  double multiplier{2.0};
  std::chrono::milliseconds max_delay{10000};
  std::size_t max_restarts{5};
  std::chrono::milliseconds reset_after{30000};
  std::chrono::milliseconds startup_timeout{60000};
};

struct WorkloadSpec {
  std::string workload_id;
  std::uint64_t revision{0};
  std::uint64_t controller_epoch{1};
  ContainerSpec container;
  RuntimeRestartPolicy restart;
  std::chrono::seconds stop_timeout{10};
};

struct RuntimeCommand {
  std::string command_id;
  RuntimeCommandKind kind{RuntimeCommandKind::Ensure};
  std::string workload_id;
  std::uint64_t revision{0};
  std::optional<WorkloadSpec> desired;
  // Stop/remove commands may override the last desired spec's grace period.
  std::optional<std::chrono::seconds> stop_timeout;
  std::uint64_t controller_epoch{1};
};

struct CommandResult {
  std::string command_id;
  std::string workload_id;
  std::uint64_t revision{0};
  WorkloadState state{WorkloadState::Failed};
  bool succeeded{false};
  bool replayed{false};
  std::string container_id;
  std::string message;
  // Retryable failures are deliberately not kept in the in-memory receipt
  // cache so an at-least-once delivery can make progress after recovery.
  bool retryable{false};
};

struct WorkloadSnapshot {
  std::string workload_id;
  std::uint64_t revision{0};
  WorkloadState state{WorkloadState::Stopped};
  bool desired_running{false};
  std::string container_id;
  std::size_t restart_count{0};
  std::optional<std::chrono::steady_clock::time_point> next_restart_at;
  std::string last_error;
  std::optional<int> last_exit_code;
};

class WorkerRuntime {
public:
  using Clock = std::chrono::steady_clock;

  WorkerRuntime(std::string worker_id, std::shared_ptr<IDockerClient> docker,
                std::size_t receipt_capacity = 2048);
  WorkerRuntime(std::string worker_id, std::uint64_t worker_epoch,
                std::shared_ptr<IDockerClient> docker,
                std::size_t receipt_capacity = 2048);

  CommandResult apply(const RuntimeCommand &command,
                      Clock::time_point now = Clock::now());
  void tick(Clock::time_point now = Clock::now());
  void advance_worker_epoch(std::uint64_t worker_epoch,
                            Clock::time_point now = Clock::now());

  [[nodiscard]] std::optional<WorkloadSnapshot>
  snapshot(const std::string &workload_id) const;
  [[nodiscard]] std::vector<WorkloadSnapshot> snapshots() const;
  [[nodiscard]] std::string prometheus_metrics() const;

  static std::string state_name(WorkloadState state);
  static std::string container_name_for(const std::string &workload_id);
  static std::string spec_fingerprint(const ContainerSpec &spec);

private:
  struct Entry {
    WorkloadSpec spec;
    std::string container_name;
    std::string container_id;
    std::string fingerprint;
    WorkloadState state{WorkloadState::Stopped};
    bool desired_running{false};
    std::size_t restart_count{0};
    std::size_t transient_failure_count{0};
    std::optional<Clock::time_point> next_restart_at;
    std::optional<Clock::time_point> healthy_since;
    std::optional<Clock::time_point> starting_since;
    std::string last_error;
    std::optional<int> last_exit_code;
  };

  struct Receipt {
    std::string command_fingerprint;
    CommandResult result;
  };

  CommandResult apply_ensure(const RuntimeCommand &command,
                             Clock::time_point now);
  CommandResult apply_stop(const RuntimeCommand &command, bool remove);
  void ensure_container(Entry &entry, Clock::time_point now);
  void validate_managed_identity(const ContainerInspection &inspection,
                                 const Entry &entry) const;
  [[nodiscard]] bool
  owned_by_current_worker(const ContainerInspection &inspection) const;
  void validate_owned(const ContainerInspection &inspection, const Entry &entry,
                      bool require_current_spec) const;
  void schedule_restart(Entry &entry, const std::string &reason,
                        Clock::time_point now);
  void schedule_transient_retry(Entry &entry, const std::string &reason,
                                Clock::time_point now);
  std::chrono::milliseconds restart_delay(const Entry &entry) const;
  std::chrono::milliseconds transient_retry_delay(const Entry &entry) const;
  WorkloadSnapshot to_snapshot(const Entry &entry) const;
  CommandResult cache_result(CommandResult result,
                             const std::string &command_fingerprint);

  std::string worker_id_;
  std::uint64_t worker_epoch_;
  std::shared_ptr<IDockerClient> docker_;
  std::size_t receipt_capacity_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> workloads_;
  std::map<std::string, Receipt> receipts_;
  std::deque<std::string> receipt_order_;
  RuntimeMetrics metrics_;
};

} // namespace minicloud::runtime

#include "minicloud/runtime/metrics.hpp"

#include <algorithm>
#include <sstream>

namespace minicloud::runtime {
namespace {

[[nodiscard]] std::string bounded_label(const std::string &value) {
  if (value.empty() || value.size() > 32 ||
      !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
               ch == '_' || ch == '-';
      })) {
    return "other";
  }
  return value;
}

[[nodiscard]] std::string pair_key(const std::string &first,
                                   const std::string &second) {
  return bounded_label(first) + '\n' + bounded_label(second);
}

void render_counter_family(std::ostringstream &output, const char *name,
                           const char *help, const char *first_label,
                           const char *second_label,
                           const std::map<std::string, std::uint64_t> &values) {
  output << "# HELP " << name << ' ' << help << '\n';
  output << "# TYPE " << name << " counter\n";
  for (const auto &[key, value] : values) {
    const auto separator = key.find('\n');
    const std::string first = key.substr(0, separator);
    const std::string second = separator == std::string::npos
                                   ? std::string("other")
                                   : key.substr(separator + 1);
    output << name << '{' << first_label << "=\"" << first << "\","
           << second_label << "=\"" << second << "\"} " << value << '\n';
  }
}

} // namespace

void RuntimeMetrics::record_command(const std::string &operation,
                                    const std::string &outcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++commands_[pair_key(operation, outcome)];
}

void RuntimeMetrics::record_replay() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++replays_;
}

void RuntimeMetrics::record_restart(const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++restarts_[bounded_label(reason)];
}

void RuntimeMetrics::record_docker_error(const std::string &operation) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++docker_errors_[bounded_label(operation)];
}

std::string RuntimeMetrics::render_prometheus(
    const std::map<std::string, std::size_t> &workload_states) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream output;
  render_counter_family(output, "minicloud_runtime_commands_total",
                        "Runtime commands by bounded operation and outcome.",
                        "operation", "outcome", commands_);

  output << "# HELP minicloud_runtime_command_replays_total Idempotent "
            "command receipt replays.\n"
         << "# TYPE minicloud_runtime_command_replays_total counter\n"
         << "minicloud_runtime_command_replays_total " << replays_ << '\n';

  output << "# HELP minicloud_runtime_restarts_total Worker-managed restart "
            "attempts by reason.\n"
         << "# TYPE minicloud_runtime_restarts_total counter\n";
  for (const auto &[reason, value] : restarts_) {
    output << "minicloud_runtime_restarts_total{reason=\"" << reason << "\"} "
           << value << '\n';
  }

  output << "# HELP minicloud_runtime_docker_errors_total Docker Engine "
            "operation failures.\n"
         << "# TYPE minicloud_runtime_docker_errors_total counter\n";
  for (const auto &[operation, value] : docker_errors_) {
    output << "minicloud_runtime_docker_errors_total{operation=\"" << operation
           << "\"} " << value << '\n';
  }

  output << "# HELP minicloud_runtime_workloads Current workloads by "
            "lifecycle state.\n"
         << "# TYPE minicloud_runtime_workloads gauge\n";
  for (const auto &[state, value] : workload_states) {
    output << "minicloud_runtime_workloads{state=\"" << bounded_label(state)
           << "\"} " << value << '\n';
  }
  return output.str();
}

} // namespace minicloud::runtime

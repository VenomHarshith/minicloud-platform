#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace minicloud::runtime {

class RuntimeMetrics {
public:
  void record_command(const std::string &operation, const std::string &outcome);
  void record_replay();
  void record_restart(const std::string &reason);
  void record_docker_error(const std::string &operation);

  [[nodiscard]] std::string render_prometheus(
      const std::map<std::string, std::size_t> &workload_states) const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> commands_;
  std::map<std::string, std::uint64_t> restarts_;
  std::map<std::string, std::uint64_t> docker_errors_;
  std::uint64_t replays_{0};
};

} // namespace minicloud::runtime

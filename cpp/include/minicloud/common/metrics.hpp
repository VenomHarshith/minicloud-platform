#pragma once

#include <map>
#include <mutex>
#include <string>

namespace minicloud::common {

class MetricsRegistry final {
 public:
  void increment(const std::string& name, double amount = 1.0);
  void gauge(const std::string& name, double value);
  [[nodiscard]] std::string render() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, double> counters_;
  std::map<std::string, double> gauges_;
};

}  // namespace minicloud::common

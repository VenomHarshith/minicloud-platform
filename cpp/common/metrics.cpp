#include "minicloud/common/metrics.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace minicloud::common {
namespace {
void validate_name(const std::string& name) {
  if (name.empty() || name.size() > 160) throw std::invalid_argument("invalid metric name");
  const char first = name.front();
  if (!(first == '_' || first == ':' ||
        (first >= 'a' && first <= 'z') ||
        (first >= 'A' && first <= 'Z'))) {
    throw std::invalid_argument("metric name has an invalid first character");
  }
  for (const char character : name) {
    if (!(character == '_' || character == ':' ||
          (character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9'))) {
      throw std::invalid_argument("metric name contains unsupported characters");
    }
  }
}
}  // namespace

void MetricsRegistry::increment(const std::string& name, const double amount) {
  validate_name(name);
  if (!std::isfinite(amount) || amount < 0) {
    throw std::invalid_argument("counter increments must be finite and non-negative");
  }
  std::lock_guard lock(mutex_);
  if (gauges_.contains(name)) {
    throw std::invalid_argument("metric name is already registered as a gauge");
  }
  counters_[name] += amount;
}

void MetricsRegistry::gauge(const std::string& name, const double value) {
  validate_name(name);
  if (!std::isfinite(value)) throw std::invalid_argument("gauge values must be finite");
  std::lock_guard lock(mutex_);
  if (counters_.contains(name)) {
    throw std::invalid_argument("metric name is already registered as a counter");
  }
  gauges_[name] = value;
}

std::string MetricsRegistry::render() const {
  std::lock_guard lock(mutex_);
  std::ostringstream output;
  output << std::setprecision(17);
  for (const auto& [name, value] : counters_) {
    output << "# TYPE " << name << " counter\n" << name << ' ' << value << '\n';
  }
  for (const auto& [name, value] : gauges_) {
    output << "# TYPE " << name << " gauge\n" << name << ' ' << value << '\n';
  }
  return output.str();
}

}  // namespace minicloud::common

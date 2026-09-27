#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace minicloud::common {

class ConfigurationError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct BindAddress {
  std::string host;
  std::uint16_t port{};
};

class Environment final {
 public:
  [[nodiscard]] static std::optional<std::string> optional(const std::string& name);
  [[nodiscard]] static std::string required(const std::string& name);
  [[nodiscard]] static std::string value(const std::string& name, std::string fallback);
  [[nodiscard]] static std::int64_t integer(const std::string& name,
                                            std::int64_t fallback,
                                            std::int64_t minimum,
                                            std::int64_t maximum);
  [[nodiscard]] static BindAddress bind(const std::string& name,
                                        const std::string& fallback);
  [[nodiscard]] static std::map<std::string, std::string> labels(
      const std::string& name);
};

[[nodiscard]] std::string utc_timestamp();
[[nodiscard]] std::string random_uuid();
[[nodiscard]] bool constant_time_equal(const std::string& left,
                                       const std::string& right) noexcept;

}  // namespace minicloud::common

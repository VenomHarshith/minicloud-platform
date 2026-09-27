#include "minicloud/common/config.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace minicloud::common {

std::optional<std::string> Environment::optional(const std::string& name) {
#if defined(_WIN32)
  char* raw = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&raw, &length, name.c_str()) != 0 || raw == nullptr) {
    return std::nullopt;
  }
  std::string value(raw);
  std::free(raw);
#else
  const char* raw = std::getenv(name.c_str());
  if (raw == nullptr) return std::nullopt;
  std::string value(raw);
#endif
  if (value.empty()) return std::nullopt;
  if (value.size() > 16 * 1024) {
    throw ConfigurationError(name + " exceeds the 16 KiB configuration limit");
  }
  return value;
}

std::string Environment::required(const std::string& name) {
  const auto found = optional(name);
  if (!found) throw ConfigurationError(name + " is required");
  return *found;
}

std::string Environment::value(const std::string& name, std::string fallback) {
  const auto found = optional(name);
  return found ? *found : std::move(fallback);
}

std::int64_t Environment::integer(const std::string& name,
                                  const std::int64_t fallback,
                                  const std::int64_t minimum,
                                  const std::int64_t maximum) {
  const auto found = optional(name);
  if (!found) return fallback;
  std::int64_t parsed{};
  const auto result = std::from_chars(found->data(), found->data() + found->size(), parsed);
  if (result.ec != std::errc{} || result.ptr != found->data() + found->size() ||
      parsed < minimum || parsed > maximum) {
    throw ConfigurationError(name + " must be an integer between " +
                             std::to_string(minimum) + " and " +
                             std::to_string(maximum));
  }
  return parsed;
}

BindAddress Environment::bind(const std::string& name, const std::string& fallback) {
  const std::string text = value(name, fallback);
  std::string host;
  std::string port_text;
  if (!text.empty() && text.front() == '[') {
    const std::size_t closing = text.find(']');
    if (closing == std::string::npos || closing == 1 || closing + 2 >= text.size() ||
        text[closing + 1] != ':') {
      throw ConfigurationError(name + " must use [IPv6]:port format");
    }
    host = text.substr(1, closing - 1);
    port_text = text.substr(closing + 2);
  } else {
    const std::size_t separator = text.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size() ||
        text.find(':') != separator) {
      throw ConfigurationError(name + " must use host:port format (bracket IPv6 literals)");
    }
    host = text.substr(0, separator);
    port_text = text.substr(separator + 1);
  }
  if (host.size() > 253) throw ConfigurationError(name + " host is too long");
  for (const char character : host) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte <= 0x20U || byte == 0x7FU) {
      throw ConfigurationError(name + " host contains whitespace or control characters");
    }
  }
  std::uint32_t port{};
  const auto result = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (result.ec != std::errc{} || result.ptr != port_text.data() + port_text.size() ||
      port == 0 || port > 65535) {
    throw ConfigurationError(name + " contains an invalid port");
  }
  return BindAddress{host, static_cast<std::uint16_t>(port)};
}

std::map<std::string, std::string> Environment::labels(const std::string& name) {
  std::map<std::string, std::string> result;
  const auto configured = optional(name);
  if (!configured) return result;
  if (configured->back() == ',') {
    throw ConfigurationError(name + " labels must not end with a comma");
  }
  std::size_t offset = 0;
  while (offset < configured->size()) {
    const std::size_t comma = configured->find(',', offset);
    const std::string pair = configured->substr(
        offset, comma == std::string::npos ? std::string::npos : comma - offset);
    const std::size_t equals = pair.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= pair.size() ||
        pair.size() > 256) {
      throw ConfigurationError(name + " labels must be comma-separated key=value pairs");
    }
    if (!result.emplace(pair.substr(0, equals), pair.substr(equals + 1)).second) {
      throw ConfigurationError(name + " contains a duplicate label key");
    }
    if (result.size() > 64) throw ConfigurationError(name + " has more than 64 labels");
    if (comma == std::string::npos) break;
    offset = comma + 1;
  }
  return result;
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &value) != 0) {
    throw std::runtime_error("failed to convert the current time to UTC");
  }
#else
  if (gmtime_r(&value, &utc) == nullptr) {
    throw std::runtime_error("failed to convert the current time to UTC");
  }
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string random_uuid() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    if (index == 3 || index == 5 || index == 7 || index == 9) output << '-';
  }
  return output.str();
}

bool constant_time_equal(const std::string& left, const std::string& right) noexcept {
  const std::size_t width = left.size() > right.size() ? left.size() : right.size();
  std::size_t mismatch = left.size() ^ right.size();
  for (std::size_t index = 0; index < width; ++index) {
    const unsigned char a = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
    const unsigned char b = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
    mismatch |= static_cast<std::size_t>(a ^ b);
  }
  return mismatch == 0;
}

}  // namespace minicloud::common

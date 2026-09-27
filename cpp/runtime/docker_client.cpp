#include "minicloud/runtime/docker_client.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <regex>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace minicloud::runtime {
namespace {

constexpr std::size_t kMaxIdentifier = 255;
constexpr std::size_t kMaxString = 4096;
constexpr std::size_t kMaxItems = 256;

[[nodiscard]] bool starts_with(const std::string &value,
                               const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] std::string lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

void require_bounded(const std::string &value, const char *field,
                     std::size_t maximum) {
  if (value.empty() || value.size() > maximum ||
      value.find('\0') != std::string::npos ||
      value.find('\r') != std::string::npos ||
      value.find('\n') != std::string::npos) {
    throw std::invalid_argument(std::string(field) +
                                " must be a bounded single-line string");
  }
}

[[nodiscard]] bool is_loopback_host(std::string host) {
  host = lowercase(std::move(host));
  if (host == "localhost" || host == "::1" || host == "[::1]") {
    return true;
  }
  if (!starts_with(host, "127.")) {
    return false;
  }
  std::size_t begin = 0;
  int components = 0;
  while (begin < host.size()) {
    const auto end = host.find('.', begin);
    const std::string part = host.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (part.empty() || part.size() > 3 ||
        !std::all_of(part.begin(), part.end(),
                     [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
      return false;
    }
    unsigned long value = 0;
    try {
      value = std::stoul(part);
    } catch (const std::exception &) {
      return false;
    }
    if (value > 255) {
      return false;
    }
    ++components;
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return components == 4;
}

struct ParsedTcpEndpoint {
  std::string url;
  std::string host;
};

[[nodiscard]] ParsedTcpEndpoint parse_tcp_endpoint(const std::string &input) {
  std::string url = input;
  if (starts_with(url, "tcp://")) {
    url = "http://" + url.substr(6);
  }
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos ||
      (url.substr(0, scheme_end) != "http" &&
       url.substr(0, scheme_end) != "https")) {
    throw std::invalid_argument(
        "Docker TCP endpoint must use tcp, http, or https");
  }
  const auto authority_begin = scheme_end + 3;
  if (authority_begin >= url.size()) {
    throw std::invalid_argument("Docker TCP endpoint is missing an authority");
  }
  const auto suffix = url.find_first_of("/?#", authority_begin);
  if (suffix != std::string::npos) {
    if (suffix != url.size() - 1 || url[suffix] != '/') {
      throw std::invalid_argument(
          "Docker endpoint must not contain a path, query, or fragment");
    }
    url.resize(suffix);
  }
  const std::string authority = url.substr(authority_begin);
  if (authority.find('@') != std::string::npos) {
    throw std::invalid_argument("Docker endpoint must not embed credentials");
  }

  std::string host;
  std::string port;
  if (!authority.empty() && authority.front() == '[') {
    const auto closing = authority.find(']');
    if (closing == std::string::npos || closing + 1 >= authority.size() ||
        authority[closing + 1] != ':') {
      throw std::invalid_argument("IPv6 Docker endpoint must include a port");
    }
    host = authority.substr(0, closing + 1);
    port = authority.substr(closing + 2);
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
      throw std::invalid_argument("Docker TCP endpoint must include a port");
    }
    host = authority.substr(0, colon);
    port = authority.substr(colon + 1);
    if (host.find(':') != std::string::npos) {
      throw std::invalid_argument(
          "IPv6 Docker endpoint hosts must use square brackets");
    }
  }
  if (host.empty() || port.empty() ||
      !std::all_of(port.begin(), port.end(),
                   [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
    throw std::invalid_argument(
        "Docker TCP endpoint has an invalid host or port");
  }
  unsigned long parsed_port = 0;
  try {
    parsed_port = std::stoul(port);
  } catch (const std::exception &) {
    throw std::invalid_argument("Docker TCP endpoint has an invalid port");
  }
  if (parsed_port == 0 || parsed_port > 65535) {
    throw std::invalid_argument(
        "Docker TCP endpoint port must be between 1 and 65535");
  }
  return {url, host};
}

[[nodiscard]] std::string percent_encode(const std::string &value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const char raw_character : value) {
    const auto ch = static_cast<unsigned char>(raw_character);
    if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      encoded.push_back(static_cast<char>(ch));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[(ch >> 4U) & 0x0FU]);
      encoded.push_back(hex[ch & 0x0FU]);
    }
  }
  return encoded;
}

[[nodiscard]] bool valid_container_reference(const std::string &value) {
  if (value.empty() || value.size() > kMaxIdentifier) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
  });
}

[[nodiscard]] bool valid_label_key(const std::string &value) {
  if (value.empty() || value.size() > kMaxIdentifier || value.front() == '.' ||
      value.back() == '.') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' ||
           ch == '/';
  });
}

[[nodiscard]] bool valid_env_key(const std::string &value) {
  if (value.empty() || value.size() > 128 ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) != 0 ||
        value.front() == '_')) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_';
  });
}

[[nodiscard]] bool valid_network(const std::string &value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
  });
}

void validate_spec(const ContainerSpec &spec) {
  require_bounded(spec.image, "image", 512);
  if (spec.entrypoint.size() > kMaxItems || spec.command.size() > kMaxItems ||
      spec.environment.size() > kMaxItems || spec.labels.size() > kMaxItems ||
      spec.ports.size() > kMaxItems) {
    throw std::invalid_argument("container spec contains too many entries");
  }
  const auto validate_arguments = [](const std::vector<std::string> &arguments,
                                     const char *field) {
    for (const auto &argument : arguments) {
      if (argument.empty() || argument.size() > kMaxString ||
          argument.find('\0') != std::string::npos) {
        throw std::invalid_argument(std::string(field) +
                                    " arguments must be bounded strings");
      }
    }
  };
  validate_arguments(spec.entrypoint, "container entrypoint");
  validate_arguments(spec.command, "container command");
  for (const auto &[key, value] : spec.environment) {
    if (!valid_env_key(key) || value.size() > kMaxString ||
        value.find('\0') != std::string::npos) {
      throw std::invalid_argument("container environment is invalid");
    }
  }
  for (const auto &[key, value] : spec.labels) {
    if (!valid_label_key(key) || value.size() > kMaxString ||
        value.find('\0') != std::string::npos) {
      throw std::invalid_argument("container labels are invalid");
    }
  }
  if (!spec.working_directory.empty()) {
    require_bounded(spec.working_directory, "working_directory", kMaxString);
    if (spec.working_directory.front() != '/') {
      throw std::invalid_argument(
          "container working_directory must be absolute inside the image");
    }
  }
  if (!spec.user.empty()) {
    require_bounded(spec.user, "user", 128);
    if (!std::all_of(spec.user.begin(), spec.user.end(), [](unsigned char ch) {
          return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' ||
                 ch == ':';
        })) {
      throw std::invalid_argument("container user is invalid");
    }
  }
  if (spec.cpu_millis < 1 || spec.cpu_millis > 1'000'000) {
    throw std::invalid_argument("cpu_millis must be between 1 and 1000000");
  }
  if (spec.memory_mb < 16 || spec.memory_mb > 1'048'576) {
    throw std::invalid_argument("memory_mb must be between 16 and 1048576");
  }
  if (spec.pids_limit < 1 || spec.pids_limit > 1'000'000) {
    throw std::invalid_argument("pids_limit must be between 1 and 1000000");
  }
  if (!valid_network(spec.network_mode)) {
    throw std::invalid_argument("network_mode is invalid");
  }

  std::map<std::string, bool> port_keys;
  std::map<std::string, bool> published_port_keys;
  for (const auto &port : spec.ports) {
    if (port.container_port == 0 ||
        (port.protocol != "tcp" && port.protocol != "udp")) {
      throw std::invalid_argument("container port and protocol are invalid");
    }
    if (!is_loopback_host(port.host_ip)) {
      throw std::invalid_argument(
          "published ports must bind to a loopback host");
    }
    const std::string key =
        std::to_string(port.container_port) + "/" + port.protocol;
    if (!port_keys.emplace(key, true).second) {
      throw std::invalid_argument("container ports must be unique");
    }
    if (port.host_port != 0) {
      const std::string published_key = lowercase(port.host_ip) + ":" +
                                        std::to_string(port.host_port) + "/" +
                                        port.protocol;
      if (!published_port_keys.emplace(published_key, true).second) {
        throw std::invalid_argument("published host ports must be unique");
      }
    }
  }
  if (!spec.ports.empty() &&
      (spec.network_mode == "none" || spec.network_mode == "host")) {
    throw std::invalid_argument(
        "published ports require a bridge-style network");
  }

  if (spec.health.has_value()) {
    const auto &health = *spec.health;
    if (health.test_argv.empty() || health.test_argv.size() > 64) {
      throw std::invalid_argument(
          "health test_argv must contain between 1 and 64 items");
    }
    for (const auto &argument : health.test_argv) {
      if (argument.empty() || argument.size() > kMaxString ||
          argument.find('\0') != std::string::npos) {
        throw std::invalid_argument(
            "health command arguments must be bounded strings");
      }
    }
    if (health.interval < std::chrono::milliseconds(1) ||
        health.interval > std::chrono::hours(24) ||
        health.timeout < std::chrono::milliseconds(1) ||
        health.timeout > health.interval ||
        health.start_period < std::chrono::milliseconds(0) ||
        health.start_period > std::chrono::hours(24) || health.retries < 1 ||
        health.retries > 100) {
      throw std::invalid_argument("health timing or retry bounds are invalid");
    }
  }
}

[[nodiscard]] std::string api_message(const HttpResponse &response) {
  std::string message = "Docker Engine request failed";
  try {
    const auto value = nlohmann::json::parse(response.body);
    if (value.is_object() && value.contains("message") &&
        value["message"].is_string()) {
      message = value["message"].get<std::string>();
    }
  } catch (const nlohmann::json::exception &) {
    // Docker occasionally returns an empty or plain-text error. Keep the
    // stable redacted message rather than reflecting an unbounded body.
  }
  if (message.size() > 512) {
    message.resize(512);
  }
  return message;
}

[[nodiscard]] nlohmann::json parse_json_body(const HttpResponse &response) {
  try {
    return nlohmann::json::parse(response.body);
  } catch (const nlohmann::json::exception &error) {
    throw DockerError(std::string("Docker Engine returned invalid JSON: ") +
                      error.what());
  }
}

struct CurlWriteState {
  std::string *destination;
  std::size_t maximum;
  bool overflow{false};
};

std::size_t write_body(char *data, std::size_t size, std::size_t count,
                       void *opaque) {
  if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
    static_cast<CurlWriteState *>(opaque)->overflow = true;
    return 0;
  }
  const std::size_t bytes = size * count;
  auto &state = *static_cast<CurlWriteState *>(opaque);
  if (bytes >
      state.maximum - std::min(state.maximum, state.destination->size())) {
    state.overflow = true;
    return 0;
  }
  state.destination->append(data, bytes);
  return bytes;
}

std::size_t write_header(char *data, std::size_t size, std::size_t count,
                         void *opaque) {
  if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
    return 0;
  }
  const std::size_t bytes = size * count;
  auto &headers = *static_cast<std::vector<std::string> *>(opaque);
  if (headers.size() < 256 && bytes <= 8192) {
    std::string line(data, bytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    if (!line.empty()) {
      headers.push_back(std::move(line));
    }
  }
  return bytes;
}

class CurlGlobal final {
public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw DockerError("libcurl global initialization failed");
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
  CurlGlobal(const CurlGlobal &) = delete;
  CurlGlobal &operator=(const CurlGlobal &) = delete;
};

void ensure_curl_global() {
  static CurlGlobal global;
  (void)global;
}

#ifdef _WIN32
[[nodiscard]] std::wstring utf8_to_wide(const std::string &value) {
  if (value.empty()) {
    return {};
  }
  const int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) {
    throw DockerError("named-pipe endpoint is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), wide.data(),
                          count) != count) {
    throw DockerError("named-pipe endpoint conversion failed");
  }
  return wide;
}

[[nodiscard]] DWORD
remaining_timeout(const std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  return static_cast<DWORD>(std::clamp<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
          .count(),
      1, std::numeric_limits<DWORD>::max()));
}

[[nodiscard]] DWORD
complete_pipe_io(HANDLE pipe, OVERLAPPED &operation,
                 const std::chrono::steady_clock::time_point deadline,
                 const char *failure_message) {
  const DWORD wait =
      WaitForSingleObject(operation.hEvent, remaining_timeout(deadline));
  if (wait == WAIT_TIMEOUT) {
    CancelIoEx(pipe, &operation);
    WaitForSingleObject(operation.hEvent, INFINITE);
    throw DockerError(
        "Docker named-pipe request exceeded its configured deadline");
  }
  if (wait != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &operation);
    // OVERLAPPED storage and its event must remain alive until cancellation
    // completes. This also prevents a late completion from touching a closed
    // event handle on the exceptional path.
    WaitForSingleObject(operation.hEvent, INFINITE);
    throw DockerError(failure_message);
  }
  DWORD transferred = 0;
  if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      return 0;
    }
    throw DockerError(failure_message);
  }
  return transferred;
}

void pipe_write_all(HANDLE pipe, const std::string &value,
                    const std::chrono::steady_clock::time_point deadline) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto remaining = std::min<std::size_t>(
        value.size() - offset,
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
      throw DockerError("creating a Docker named-pipe event failed");
    }
    struct EventGuard {
      HANDLE value;
      ~EventGuard() { CloseHandle(value); }
    } event_guard{event};
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD written = 0;
    if (!WriteFile(pipe, value.data() + offset, static_cast<DWORD>(remaining),
                   &written, &operation)) {
      if (GetLastError() != ERROR_IO_PENDING) {
        throw DockerError("Docker named-pipe write failed");
      }
      written = complete_pipe_io(pipe, operation, deadline,
                                 "Docker named-pipe write failed");
    }
    if (written == 0) {
      throw DockerError("Docker named-pipe write made no progress");
    }
    offset += written;
  }
}

[[nodiscard]] std::string decode_chunked(const std::string &input,
                                         std::size_t maximum) {
  std::string output;
  std::size_t cursor = 0;
  while (true) {
    const auto end = input.find("\r\n", cursor);
    if (end == std::string::npos) {
      throw DockerError("Docker named-pipe response has invalid chunk framing");
    }
    std::string size_text = input.substr(cursor, end - cursor);
    const auto extension = size_text.find(';');
    if (extension != std::string::npos) {
      size_text.resize(extension);
    }
    if (size_text.empty()) {
      throw DockerError("Docker named-pipe response has an invalid chunk size");
    }
    std::size_t chunk_size = 0;
    const auto parsed = std::from_chars(
        size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != size_text.data() + size_text.size()) {
      throw DockerError("Docker named-pipe response has an invalid chunk size");
    }
    cursor = end + 2;
    if (chunk_size == 0) {
      return output;
    }
    if (chunk_size > maximum - std::min(maximum, output.size()) ||
        cursor + chunk_size + 2 > input.size()) {
      throw DockerError(
          "Docker named-pipe response exceeds its configured bound");
    }
    output.append(input, cursor, chunk_size);
    cursor += chunk_size;
    if (input.compare(cursor, 2, "\r\n") != 0) {
      throw DockerError("Docker named-pipe response has invalid chunk framing");
    }
    cursor += 2;
  }
}

[[nodiscard]] HttpResponse perform_named_pipe(const DockerEndpoint &endpoint,
                                              const HttpRequest &request,
                                              const TransportLimits &limits) {
  const auto timeout_ms = static_cast<DWORD>(std::clamp<std::int64_t>(
      limits.connect_timeout.count(), 1, std::numeric_limits<DWORD>::max()));
  const std::wstring path = utf8_to_wide(endpoint.address);
  if (!WaitNamedPipeW(path.c_str(), timeout_ms)) {
    throw DockerError(
        "Docker named pipe was unavailable before the connect deadline");
  }
  HANDLE pipe =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    throw DockerError("opening the Docker named pipe failed");
  }
  struct HandleGuard {
    HANDLE value;
    ~HandleGuard() { CloseHandle(value); }
  } guard{pipe};

  std::ostringstream wire;
  wire << request.method << ' ' << request.path << " HTTP/1.1\r\n"
       << "Host: docker\r\nConnection: close\r\nUser-Agent: "
          "MiniCloud-Cpp/1\r\n";
  for (const auto &header : request.headers) {
    wire << header << "\r\n";
  }
  wire << "Content-Length: " << request.body.size() << "\r\n\r\n"
       << request.body;
  const auto deadline =
      std::chrono::steady_clock::now() + limits.request_timeout;
  pipe_write_all(pipe, wire.str(), deadline);

  std::string raw;
  const std::size_t wire_limit = limits.max_response_bytes + 64U * 1024U;
  std::array<char, 16U * 1024U> buffer{};
  while (true) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
      throw DockerError("creating a Docker named-pipe event failed");
    }
    struct EventGuard {
      HANDLE value;
      ~EventGuard() { CloseHandle(value); }
    } event_guard{event};
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD read = 0;
    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                  &operation)) {
      const DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        break;
      }
      if (error != ERROR_IO_PENDING) {
        throw DockerError("reading the Docker named-pipe response failed");
      }
      read = complete_pipe_io(pipe, operation, deadline,
                              "reading the Docker named-pipe response failed");
    }
    if (read == 0) {
      break;
    }
    if (read > wire_limit - std::min(wire_limit, raw.size())) {
      throw DockerError(
          "Docker named-pipe response exceeds its configured bound");
    }
    raw.append(buffer.data(), read);
  }

  const auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    throw DockerError("Docker named-pipe response is not valid HTTP");
  }
  const auto first_line_end = raw.find("\r\n");
  if (first_line_end == std::string::npos) {
    throw DockerError("Docker named-pipe response has no status line");
  }
  std::istringstream status_line(raw.substr(0, first_line_end));
  std::string version;
  long status = 0;
  status_line >> version >> status;
  if (!starts_with(version, "HTTP/") || status < 100 || status > 599) {
    throw DockerError("Docker named-pipe response has an invalid status line");
  }
  HttpResponse response;
  response.status_code = status;
  std::size_t line_start = first_line_end + 2;
  bool chunked = false;
  while (line_start < header_end) {
    const auto line_end = raw.find("\r\n", line_start);
    if (line_end == std::string::npos || line_end > header_end) {
      break;
    }
    std::string line = raw.substr(line_start, line_end - line_start);
    if (starts_with(lowercase(line), "transfer-encoding:") &&
        lowercase(line).find("chunked") != std::string::npos) {
      chunked = true;
    }
    response.headers.push_back(std::move(line));
    line_start = line_end + 2;
  }
  const std::string body = raw.substr(header_end + 4);
  response.body =
      chunked ? decode_chunked(body, limits.max_response_bytes) : body;
  if (response.body.size() > limits.max_response_bytes) {
    throw DockerError(
        "Docker named-pipe response exceeds its configured bound");
  }
  return response;
}
#endif

[[nodiscard]] std::uint16_t parse_port_number(const std::string &value) {
  unsigned long parsed = 0;
  try {
    parsed = std::stoul(value);
  } catch (const std::exception &) {
    return 0;
  }
  return parsed <= 65535 ? static_cast<std::uint16_t>(parsed) : 0;
}

[[nodiscard]] DockerLogs demultiplex_logs(const std::string &raw) {
  DockerLogs logs;
  std::size_t cursor = 0;
  bool framed = !raw.empty();
  while (cursor < raw.size()) {
    if (raw.size() - cursor < 8) {
      framed = false;
      break;
    }
    const unsigned char stream = static_cast<unsigned char>(raw[cursor]);
    if (stream != 1 && stream != 2) {
      framed = false;
      break;
    }
    const auto byte = [&raw, cursor](std::size_t offset) {
      return static_cast<std::uint32_t>(
          static_cast<unsigned char>(raw[cursor + offset]));
    };
    const std::uint32_t length =
        (byte(4) << 24U) | (byte(5) << 16U) | (byte(6) << 8U) | byte(7);
    cursor += 8;
    if (length > raw.size() - cursor) {
      framed = false;
      break;
    }
    auto &target = stream == 1 ? logs.stdout_text : logs.stderr_text;
    target.append(raw, cursor, length);
    logs.records.push_back(
        {stream == 1 ? DockerLogs::Stream::Stdout : DockerLogs::Stream::Stderr,
         raw.substr(cursor, length)});
    cursor += length;
  }
  if (!framed) {
    logs.stdout_text = raw;
    logs.stderr_text.clear();
    logs.records.clear();
    if (!raw.empty()) {
      logs.records.push_back({DockerLogs::Stream::Stdout, raw});
    }
  }
  return logs;
}

} // namespace

DockerError::DockerError(const std::string &message)
    : std::runtime_error(message) {}

DockerApiError::DockerApiError(long status_code, const std::string &message)
    : DockerError(message), status_code_(status_code) {}

long DockerApiError::status_code() const noexcept { return status_code_; }

DockerEndpoint DockerEndpoint::parse(const std::string &value) {
  require_bounded(value, "Docker endpoint", kMaxString);
  if (starts_with(value, "unix://")) {
    const std::string path = value.substr(7);
    if (path.empty() || path.front() != '/') {
      throw std::invalid_argument(
          "Docker Unix socket must be an absolute path");
    }
    return {DockerEndpointKind::UnixSocket, path, "http://localhost", true};
  }
  if (starts_with(value, "npipe://")) {
    std::string path = value.substr(8);
    std::replace(path.begin(), path.end(), '/', '\\');
    while (!path.empty() && path.front() == '\\') {
      path.erase(path.begin());
    }
    const std::string lowered = lowercase(path);
    const std::string prefix = ".\\pipe\\";
    const std::string pipe_name = path.size() >= prefix.size()
                                      ? path.substr(prefix.size())
                                      : std::string{};
    if (!starts_with(lowered, prefix) || pipe_name.empty() ||
        pipe_name.find('\\') != std::string::npos ||
        pipe_name.find("..") != std::string::npos ||
        !std::all_of(pipe_name.begin(), pipe_name.end(), [](unsigned char ch) {
          return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
        })) {
      throw std::invalid_argument("Docker named pipe must be under //./pipe/");
    }
    return {
        DockerEndpointKind::WindowsNamedPipe,
        "\\\\" + path,
        "http://localhost",
        true,
    };
  }
  const auto parsed = parse_tcp_endpoint(value);
  return {
      DockerEndpointKind::Tcp,
      parsed.url,
      parsed.url,
      is_loopback_host(parsed.host),
  };
}

HttpResponse CurlHttpTransport::perform(const DockerEndpoint &endpoint,
                                        const HttpRequest &request,
                                        const TransportLimits &limits) {
  if (limits.connect_timeout <= std::chrono::milliseconds(0) ||
      limits.request_timeout <= std::chrono::milliseconds(0) ||
      limits.connect_timeout > std::chrono::minutes(5) ||
      limits.request_timeout > std::chrono::minutes(5) ||
      limits.max_response_bytes == 0 ||
      limits.max_response_bytes > 64U * 1024U * 1024U) {
    throw std::invalid_argument(
        "Docker transport limits are outside safe bounds");
  }
  if (endpoint.kind == DockerEndpointKind::WindowsNamedPipe) {
#ifdef _WIN32
    return perform_named_pipe(endpoint, request, limits);
#else
    throw DockerError(
        "Docker named-pipe endpoints are available only on Windows");
#endif
  }

  ensure_curl_global();
  CURL *raw = curl_easy_init();
  if (raw == nullptr) {
    throw DockerError("libcurl easy handle initialization failed");
  }
  struct CurlGuard {
    CURL *handle;
    ~CurlGuard() { curl_easy_cleanup(handle); }
  } guard{raw};

  std::string body;
  std::vector<std::string> response_headers;
  CurlWriteState write_state{&body, limits.max_response_bytes, false};
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  const std::string url = endpoint.base_url + request.path;

  curl_easy_setopt(raw, CURLOPT_URL, url.c_str());
  curl_easy_setopt(raw, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(limits.connect_timeout.count()));
  curl_easy_setopt(raw, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(limits.request_timeout.count()));
  curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(raw, CURLOPT_ERRORBUFFER, error_buffer.data());
  curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, &write_body);
  curl_easy_setopt(raw, CURLOPT_WRITEDATA, &write_state);
  curl_easy_setopt(raw, CURLOPT_HEADERFUNCTION, &write_header);
  curl_easy_setopt(raw, CURLOPT_HEADERDATA, &response_headers);
  curl_easy_setopt(raw, CURLOPT_USERAGENT, "MiniCloud-Cpp/1");
  curl_easy_setopt(raw, CURLOPT_SSL_VERIFYPEER, request.verify_tls ? 1L : 0L);
  curl_easy_setopt(raw, CURLOPT_SSL_VERIFYHOST, request.verify_tls ? 2L : 0L);
  if (endpoint.kind == DockerEndpointKind::UnixSocket) {
    curl_easy_setopt(raw, CURLOPT_UNIX_SOCKET_PATH, endpoint.address.c_str());
  }
  if (!request.body.empty() || request.method == "POST" ||
      request.method == "PUT") {
    curl_easy_setopt(raw, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(raw, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size()));
  }

  curl_slist *raw_headers = nullptr;
  for (const auto &header : request.headers) {
    curl_slist *extended = curl_slist_append(raw_headers, header.c_str());
    if (extended == nullptr) {
      curl_slist_free_all(raw_headers);
      throw DockerError("allocating libcurl request headers failed");
    }
    raw_headers = extended;
  }
  struct HeaderGuard {
    curl_slist *value;
    ~HeaderGuard() { curl_slist_free_all(value); }
  } header_guard{raw_headers};
  if (raw_headers != nullptr) {
    curl_easy_setopt(raw, CURLOPT_HTTPHEADER, raw_headers);
  }

  const CURLcode code = curl_easy_perform(raw);
  if (write_state.overflow) {
    throw DockerError(
        "Docker Engine response exceeded its configured byte limit");
  }
  if (code != CURLE_OK) {
    std::string detail = error_buffer[0] != '\0' ? error_buffer.data()
                                                 : curl_easy_strerror(code);
    if (detail.size() > 512) {
      detail.resize(512);
    }
    throw DockerError("Docker Engine transport failed: " + detail);
  }
  long status = 0;
  curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
  return {status, std::move(response_headers), std::move(body)};
}

DockerClient::DockerClient(DockerClientOptions options,
                           std::shared_ptr<IHttpTransport> transport)
    : options_(std::move(options)), transport_(std::move(transport)) {
  if (!transport_) {
    throw std::invalid_argument("Docker HTTP transport is required");
  }
  switch (options_.endpoint.kind) {
  case DockerEndpointKind::UnixSocket:
    options_.endpoint =
        DockerEndpoint::parse("unix://" + options_.endpoint.address);
    break;
  case DockerEndpointKind::WindowsNamedPipe: {
    const std::string prefix = "\\\\.\\pipe\\";
    if (!starts_with(lowercase(options_.endpoint.address), prefix) ||
        options_.endpoint.address.size() <= prefix.size()) {
      throw std::invalid_argument("Docker named-pipe endpoint is invalid");
    }
    std::string portable = options_.endpoint.address;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    options_.endpoint = DockerEndpoint::parse("npipe:" + portable);
    break;
  }
  case DockerEndpointKind::Tcp:
    options_.endpoint = DockerEndpoint::parse(options_.endpoint.address);
    break;
  default:
    throw std::invalid_argument("Docker endpoint kind is invalid");
  }
  if (options_.api_version.size() > 16 ||
      !std::regex_match(options_.api_version,
                        std::regex(R"(^v[0-9]+\.[0-9]+$)"))) {
    throw std::invalid_argument("Docker API version must look like v1.43");
  }
  if (options_.endpoint.kind == DockerEndpointKind::Tcp &&
      !options_.endpoint.loopback && !options_.allow_remote_tcp) {
    throw std::invalid_argument("remote Docker TCP endpoints require the "
                                "explicit allow_remote_tcp option");
  }
  if (options_.limits.connect_timeout <= std::chrono::milliseconds(0) ||
      options_.limits.request_timeout <= std::chrono::milliseconds(0) ||
      options_.limits.connect_timeout > std::chrono::minutes(5) ||
      options_.limits.request_timeout > std::chrono::minutes(5) ||
      options_.limits.max_response_bytes == 0 ||
      options_.limits.max_response_bytes > 64U * 1024U * 1024U) {
    throw std::invalid_argument(
        "Docker transport limits are outside safe bounds");
  }
}

const DockerClientOptions &DockerClient::options() const noexcept {
  return options_;
}

nlohmann::json DockerClient::create_payload(const ContainerSpec &spec) {
  validate_spec(spec);
  nlohmann::json payload = {
      {"Image", spec.image},
      {"Labels", spec.labels},
      {"Tty", false},
      {"OpenStdin", false},
  };
  if (!spec.entrypoint.empty()) {
    payload["Entrypoint"] = spec.entrypoint;
  }
  if (!spec.command.empty()) {
    payload["Cmd"] = spec.command;
  }
  if (!spec.working_directory.empty()) {
    payload["WorkingDir"] = spec.working_directory;
  }
  if (!spec.user.empty()) {
    payload["User"] = spec.user;
  }
  std::vector<std::string> environment;
  environment.reserve(spec.environment.size());
  for (const auto &[key, value] : spec.environment) {
    environment.push_back(key + "=" + value);
  }
  payload["Env"] = std::move(environment);

  nlohmann::json host_config = {
      {"NanoCpus", spec.cpu_millis * 1'000'000LL},
      {"Memory", spec.memory_mb * 1024LL * 1024LL},
      {"PidsLimit", spec.pids_limit},
      {"NetworkMode", spec.network_mode},
      {"ReadonlyRootfs", spec.read_only_root_filesystem},
      {"RestartPolicy", {{"Name", "no"}, {"MaximumRetryCount", 0}}},
  };
  if (spec.drop_all_capabilities) {
    host_config["CapDrop"] = nlohmann::json::array({"ALL"});
  }
  if (spec.no_new_privileges) {
    host_config["SecurityOpt"] =
        nlohmann::json::array({"no-new-privileges:true"});
  }
  nlohmann::json exposed = nlohmann::json::object();
  nlohmann::json bindings = nlohmann::json::object();
  for (const auto &port : spec.ports) {
    const std::string key =
        std::to_string(port.container_port) + "/" + port.protocol;
    exposed[key] = nlohmann::json::object();
    bindings[key] = nlohmann::json::array({{
        {"HostIp", port.host_ip},
        {"HostPort",
         port.host_port == 0 ? std::string{} : std::to_string(port.host_port)},
    }});
  }
  if (!spec.ports.empty()) {
    payload["ExposedPorts"] = std::move(exposed);
    host_config["PortBindings"] = std::move(bindings);
  }
  payload["HostConfig"] = std::move(host_config);

  if (spec.health.has_value()) {
    const auto &source = *spec.health;
    nlohmann::json test = nlohmann::json::array({"CMD"});
    for (const auto &argument : source.test_argv) {
      test.push_back(argument);
    }
    payload["Healthcheck"] = {
        {"Test", std::move(test)},
        {"Interval", source.interval.count() * 1'000'000LL},
        {"Timeout", source.timeout.count() * 1'000'000LL},
        {"StartPeriod", source.start_period.count() * 1'000'000LL},
        {"Retries", source.retries},
    };
  }
  return payload;
}

HttpResponse
DockerClient::request(const std::string &method, const std::string &path,
                      const std::optional<nlohmann::json> &payload) {
  if (path.empty() || path.front() != '/' ||
      path.find('\r') != std::string::npos ||
      path.find('\n') != std::string::npos) {
    throw std::invalid_argument("Docker request path is invalid");
  }
  HttpRequest request_value;
  request_value.method = method;
  request_value.path = "/" + options_.api_version + path;
  request_value.headers = {"Accept: application/json"};
  request_value.verify_tls = options_.verify_tls;
  if (payload.has_value()) {
    request_value.headers.push_back("Content-Type: application/json");
    request_value.body = payload->dump();
  }
  return transport_->perform(options_.endpoint, request_value, options_.limits);
}

std::string DockerClient::create(const std::string &name,
                                 const ContainerSpec &spec) {
  if (!valid_container_reference(name)) {
    throw std::invalid_argument("container name is invalid");
  }
  const auto response =
      request("POST", "/containers/create?name=" + percent_encode(name),
              create_payload(spec));
  if (response.status_code != 201) {
    throw DockerApiError(response.status_code, api_message(response));
  }
  const auto value = parse_json_body(response);
  if (!value.is_object() || !value.contains("Id") || !value["Id"].is_string()) {
    throw DockerError("Docker create response did not contain a container ID");
  }
  const std::string id = value["Id"].get<std::string>();
  if (!valid_container_reference(id)) {
    throw DockerError(
        "Docker create response contained an invalid container ID");
  }
  return id;
}

void DockerClient::start(const std::string &id_or_name) {
  if (!valid_container_reference(id_or_name)) {
    throw std::invalid_argument("container reference is invalid");
  }
  const auto response =
      request("POST", "/containers/" + percent_encode(id_or_name) + "/start");
  if (response.status_code != 204 && response.status_code != 304) {
    throw DockerApiError(response.status_code, api_message(response));
  }
}

std::optional<ContainerInspection>
DockerClient::inspect(const std::string &id_or_name) {
  if (!valid_container_reference(id_or_name)) {
    throw std::invalid_argument("container reference is invalid");
  }
  const auto response =
      request("GET", "/containers/" + percent_encode(id_or_name) + "/json");
  if (response.status_code == 404) {
    return std::nullopt;
  }
  if (response.status_code != 200) {
    throw DockerApiError(response.status_code, api_message(response));
  }
  const auto value = parse_json_body(response);
  if (!value.is_object()) {
    throw DockerError("Docker inspect response must be an object");
  }
  try {
    ContainerInspection result;
    result.id = value.value("Id", std::string{});
    result.name = value.value("Name", std::string{});
    if (!result.name.empty() && result.name.front() == '/') {
      result.name.erase(result.name.begin());
    }
    if (value.contains("State") && value["State"].is_object()) {
      const auto &state = value["State"];
      result.status = state.value("Status", std::string{});
      result.running = state.value("Running", false);
      result.exit_code = state.value("ExitCode", 0);
      if (state.contains("Health") && state["Health"].is_object()) {
        result.health_status = state["Health"].value("Status", std::string{});
      }
    }
    if (value.contains("Config") && value["Config"].is_object() &&
        value["Config"].contains("Labels") &&
        value["Config"]["Labels"].is_object()) {
      for (auto iterator = value["Config"]["Labels"].begin();
           iterator != value["Config"]["Labels"].end(); ++iterator) {
        if (iterator.value().is_string()) {
          result.labels.emplace(iterator.key(),
                                iterator.value().get<std::string>());
        }
      }
    }
    if (value.contains("NetworkSettings") &&
        value["NetworkSettings"].is_object() &&
        value["NetworkSettings"].contains("Ports") &&
        value["NetworkSettings"]["Ports"].is_object()) {
      for (auto iterator = value["NetworkSettings"]["Ports"].begin();
           iterator != value["NetworkSettings"]["Ports"].end(); ++iterator) {
        const auto slash = iterator.key().find('/');
        if (slash == std::string::npos || !iterator.value().is_array()) {
          continue;
        }
        const auto container_port =
            parse_port_number(iterator.key().substr(0, slash));
        for (const auto &binding : iterator.value()) {
          if (!binding.is_object()) {
            continue;
          }
          const auto host_port =
              parse_port_number(binding.value("HostPort", std::string{}));
          if (container_port != 0 && host_port != 0) {
            result.published_ports.push_back({
                container_port,
                iterator.key().substr(slash + 1),
                binding.value("HostIp", std::string{}),
                host_port,
            });
          }
        }
      }
    }
    if (!valid_container_reference(result.id)) {
      throw DockerError(
          "Docker inspect response contained an invalid container ID");
    }
    return result;
  } catch (const nlohmann::json::exception &error) {
    throw DockerError(
        std::string("Docker inspect response has an invalid shape: ") +
        error.what());
  }
}

void DockerClient::stop(const std::string &id_or_name,
                        std::chrono::seconds timeout) {
  if (!valid_container_reference(id_or_name)) {
    throw std::invalid_argument("container reference is invalid");
  }
  if (timeout < std::chrono::seconds(0) ||
      timeout > std::chrono::seconds(300)) {
    throw std::invalid_argument(
        "container stop timeout must be between 0 and 300 seconds");
  }
  const auto response =
      request("POST", "/containers/" + percent_encode(id_or_name) +
                          "/stop?t=" + std::to_string(timeout.count()));
  if (response.status_code != 204 && response.status_code != 304 &&
      response.status_code != 404) {
    throw DockerApiError(response.status_code, api_message(response));
  }
}

void DockerClient::remove(const std::string &id_or_name, bool force,
                          bool remove_volumes) {
  if (!valid_container_reference(id_or_name)) {
    throw std::invalid_argument("container reference is invalid");
  }
  const auto response =
      request("DELETE", "/containers/" + percent_encode(id_or_name) +
                            "?force=" + (force ? "1" : "0") +
                            "&v=" + (remove_volumes ? "1" : "0"));
  if (response.status_code != 204 && response.status_code != 404) {
    throw DockerApiError(response.status_code, api_message(response));
  }
}

DockerLogs DockerClient::logs(const std::string &id_or_name,
                              std::size_t tail_lines, bool timestamps) {
  if (!valid_container_reference(id_or_name)) {
    throw std::invalid_argument("container reference is invalid");
  }
  if (tail_lines == 0 || tail_lines > 10000) {
    throw std::invalid_argument("tail_lines must be between 1 and 10000");
  }
  const auto response =
      request("GET", "/containers/" + percent_encode(id_or_name) +
                         "/logs?stdout=1&stderr=1&timestamps=" +
                         (timestamps ? "1" : "0") +
                         "&tail=" + std::to_string(tail_lines));
  if (response.status_code != 200) {
    throw DockerApiError(response.status_code, api_message(response));
  }
  return demultiplex_logs(response.body);
}

} // namespace minicloud::runtime

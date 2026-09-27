#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minicloud::runtime {

class DockerError : public std::runtime_error {
public:
  explicit DockerError(const std::string &message);
};

class DockerApiError : public DockerError {
public:
  DockerApiError(long status_code, const std::string &message);
  [[nodiscard]] long status_code() const noexcept;

private:
  long status_code_;
};

enum class DockerEndpointKind {
  UnixSocket,
  WindowsNamedPipe,
  Tcp,
};

struct DockerEndpoint {
  DockerEndpointKind kind{DockerEndpointKind::UnixSocket};
  std::string address;
  std::string base_url;
  bool loopback{true};

  static DockerEndpoint parse(const std::string &value);
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::vector<std::string> headers;
  std::string body;
  bool verify_tls{true};
};

struct HttpResponse {
  long status_code{0};
  std::vector<std::string> headers;
  std::string body;
};

struct TransportLimits {
  std::chrono::milliseconds connect_timeout{2000};
  std::chrono::milliseconds request_timeout{30000};
  std::size_t max_response_bytes{4U * 1024U * 1024U};
};

class IHttpTransport {
public:
  virtual ~IHttpTransport() = default;
  virtual HttpResponse perform(const DockerEndpoint &endpoint,
                               const HttpRequest &request,
                               const TransportLimits &limits) = 0;
};

// libcurl handles TCP and Unix-domain-socket Docker endpoints. On Windows the
// same transport uses the native Docker Desktop named pipe because libcurl has
// no named-pipe socket backend.
class CurlHttpTransport final : public IHttpTransport {
public:
  HttpResponse perform(const DockerEndpoint &endpoint,
                       const HttpRequest &request,
                       const TransportLimits &limits) override;
};

struct DockerClientOptions {
  DockerEndpoint endpoint;
  std::string api_version{"v1.43"};
  TransportLimits limits{};
  bool allow_remote_tcp{false};
  bool verify_tls{true};
};

struct DockerHealthConfig {
  // Docker receives ["CMD", ...test_argv]. A shell-form probe is deliberately
  // not exposed by this API.
  std::vector<std::string> test_argv;
  std::chrono::milliseconds interval{5000};
  std::chrono::milliseconds timeout{2000};
  std::chrono::milliseconds start_period{0};
  int retries{3};
};

struct DockerPortBinding {
  std::uint16_t container_port{0};
  std::string protocol{"tcp"};
  std::string host_ip{"127.0.0.1"};
  // Zero asks Docker to select an available host port.
  std::uint16_t host_port{0};
};

struct ContainerSpec {
  std::string image;
  // Docker's Entrypoint (exec form). Keeping this separate from command
  // preserves the command/arguments distinction used by the worker protocol.
  std::vector<std::string> entrypoint;
  // Docker's Cmd (exec form).
  std::vector<std::string> command;
  std::map<std::string, std::string> environment;
  std::string working_directory;
  std::string user;
  std::map<std::string, std::string> labels;
  std::int64_t cpu_millis{100};
  std::int64_t memory_mb{128};
  std::int64_t pids_limit{256};
  std::string network_mode{"bridge"};
  std::vector<DockerPortBinding> ports;
  std::optional<DockerHealthConfig> health;
  bool read_only_root_filesystem{false};
  bool drop_all_capabilities{true};
  bool no_new_privileges{true};
};

struct PublishedPort {
  std::uint16_t container_port{0};
  std::string protocol;
  std::string host_ip;
  std::uint16_t host_port{0};
};

struct ContainerInspection {
  std::string id;
  std::string name;
  std::string status;
  std::string health_status;
  int exit_code{0};
  bool running{false};
  std::map<std::string, std::string> labels;
  std::vector<PublishedPort> published_ports;
};

struct DockerLogs {
  enum class Stream {
    Stdout,
    Stderr,
  };

  struct Record {
    Stream stream{Stream::Stdout};
    std::string text;
  };

  std::string stdout_text;
  std::string stderr_text;
  // Ordered records from Docker's multiplexed stream. The aggregate strings
  // above remain convenient for callers that do not need stream ordering.
  std::vector<Record> records;
};

class IDockerClient {
public:
  virtual ~IDockerClient() = default;
  virtual std::string create(const std::string &name,
                             const ContainerSpec &spec) = 0;
  virtual void start(const std::string &id_or_name) = 0;
  virtual std::optional<ContainerInspection>
  inspect(const std::string &id_or_name) = 0;
  virtual void stop(const std::string &id_or_name,
                    std::chrono::seconds timeout) = 0;
  virtual void remove(const std::string &id_or_name, bool force,
                      bool remove_volumes) = 0;
  virtual DockerLogs logs(const std::string &id_or_name, std::size_t tail_lines,
                          bool timestamps) = 0;
};

class DockerClient final : public IDockerClient {
public:
  explicit DockerClient(DockerClientOptions options,
                        std::shared_ptr<IHttpTransport> transport =
                            std::make_shared<CurlHttpTransport>());

  std::string create(const std::string &name,
                     const ContainerSpec &spec) override;
  void start(const std::string &id_or_name) override;
  std::optional<ContainerInspection>
  inspect(const std::string &id_or_name) override;
  void stop(const std::string &id_or_name,
            std::chrono::seconds timeout) override;
  void remove(const std::string &id_or_name, bool force,
              bool remove_volumes) override;
  DockerLogs logs(const std::string &id_or_name, std::size_t tail_lines,
                  bool timestamps) override;

  [[nodiscard]] const DockerClientOptions &options() const noexcept;
  static nlohmann::json create_payload(const ContainerSpec &spec);

private:
  HttpResponse
  request(const std::string &method, const std::string &path,
          const std::optional<nlohmann::json> &payload = std::nullopt);

  DockerClientOptions options_;
  std::shared_ptr<IHttpTransport> transport_;
};

} // namespace minicloud::runtime

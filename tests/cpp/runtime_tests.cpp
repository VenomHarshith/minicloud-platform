#include "minicloud/runtime/docker_client.hpp"
#include "minicloud/runtime/worker_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using minicloud::runtime::CommandResult;
using minicloud::runtime::ContainerInspection;
using minicloud::runtime::ContainerSpec;
using minicloud::runtime::DockerApiError;
using minicloud::runtime::DockerClient;
using minicloud::runtime::DockerClientOptions;
using minicloud::runtime::DockerEndpoint;
using minicloud::runtime::DockerEndpointKind;
using minicloud::runtime::DockerError;
using minicloud::runtime::DockerHealthConfig;
using minicloud::runtime::DockerLogs;
using minicloud::runtime::DockerPortBinding;
using minicloud::runtime::HttpRequest;
using minicloud::runtime::HttpResponse;
using minicloud::runtime::IDockerClient;
using minicloud::runtime::IHttpTransport;
using minicloud::runtime::RuntimeCommand;
using minicloud::runtime::RuntimeCommandKind;
using minicloud::runtime::RuntimeRestartPolicy;
using minicloud::runtime::TransportLimits;
using minicloud::runtime::WorkerRuntime;
using minicloud::runtime::WorkloadSpec;
using minicloud::runtime::WorkloadState;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __FILE__ << ':' << __LINE__                                 \
                << ": CHECK failed: " #condition << '\n';                      \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

template <typename Exception, typename Callable>
void expect_throw(Callable &&callable) {
  try {
    callable();
    CHECK(false);
  } catch (const Exception &) {
    return;
  } catch (...) {
    CHECK(false);
  }
}

class RecordingTransport final : public IHttpTransport {
public:
  struct Call {
    DockerEndpoint endpoint;
    HttpRequest request;
    TransportLimits limits;
  };

  HttpResponse perform(const DockerEndpoint &endpoint,
                       const HttpRequest &request,
                       const TransportLimits &limits) override {
    calls.push_back({endpoint, request, limits});
    if (responses.empty()) {
      throw std::runtime_error("test transport has no response");
    }
    HttpResponse response = std::move(responses.front());
    responses.pop_front();
    return response;
  }

  std::deque<HttpResponse> responses;
  std::vector<Call> calls;
};

class FakeDocker final : public IDockerClient {
public:
  std::string create(const std::string &name,
                     const ContainerSpec &spec) override {
    calls.push_back("create:" + name);
    if (containers.count(name) != 0) {
      throw DockerApiError(409, "container already exists");
    }
    ContainerInspection inspection;
    inspection.id = "container-" + std::to_string(++next_id);
    inspection.name = name;
    inspection.status = "created";
    inspection.labels = spec.labels;
    inspection.running = false;
    containers[name] = inspection;
    specs[name] = spec;
    if (throw_after_create) {
      throw_after_create = false;
      throw DockerError("simulated lost create response");
    }
    return inspection.id;
  }

  void start(const std::string &reference) override {
    calls.push_back("start:" + reference);
    auto &inspection = find(reference);
    inspection.running = true;
    inspection.status = "running";
    inspection.health_status = specs.at(inspection.name).health.has_value()
                                   ? "starting"
                                   : std::string{};
  }

  std::optional<ContainerInspection>
  inspect(const std::string &reference) override {
    calls.push_back("inspect:" + reference);
    if (inspect_failures > 0) {
      --inspect_failures;
      throw DockerError("simulated inspect outage");
    }
    const auto named = containers.find(reference);
    if (named != containers.end()) {
      return named->second;
    }
    for (const auto &[ignored, inspection] : containers) {
      (void)ignored;
      if (inspection.id == reference) {
        return inspection;
      }
    }
    return std::nullopt;
  }

  void stop(const std::string &reference,
            std::chrono::seconds timeout) override {
    calls.push_back("stop:" + reference + ":" +
                    std::to_string(timeout.count()));
    if (stop_failures > 0) {
      --stop_failures;
      throw DockerError("simulated stop outage");
    }
    auto &inspection = find(reference);
    inspection.running = false;
    inspection.status = "exited";
    inspection.health_status.clear();
  }

  void remove(const std::string &reference, bool force, bool volumes) override {
    calls.push_back("remove:" + reference + ":" + (force ? "1" : "0") + ":" +
                    (volumes ? "1" : "0"));
    for (auto iterator = containers.begin(); iterator != containers.end();
         ++iterator) {
      if (iterator->first == reference || iterator->second.id == reference) {
        specs.erase(iterator->first);
        containers.erase(iterator);
        return;
      }
    }
  }

  DockerLogs logs(const std::string &, std::size_t, bool) override {
    DockerLogs result;
    result.stdout_text = "out";
    result.stderr_text = "err";
    return result;
  }

  ContainerInspection &find(const std::string &reference) {
    const auto named = containers.find(reference);
    if (named != containers.end()) {
      return named->second;
    }
    for (auto &[ignored, inspection] : containers) {
      (void)ignored;
      if (inspection.id == reference) {
        return inspection;
      }
    }
    throw std::runtime_error("test container not found");
  }

  std::size_t count_prefix(const std::string &prefix) const {
    std::size_t count = 0;
    for (const auto &call : calls) {
      if (call.rfind(prefix, 0) == 0) {
        ++count;
      }
    }
    return count;
  }

  std::map<std::string, ContainerInspection> containers;
  std::map<std::string, ContainerSpec> specs;
  std::vector<std::string> calls;
  int next_id{0};
  bool throw_after_create{false};
  int inspect_failures{0};
  int stop_failures{0};
};

ContainerSpec sample_container(bool health = true) {
  ContainerSpec spec;
  spec.image = "example.local/team/api@sha256:" + std::string(64, 'a');
  spec.entrypoint = {"/usr/bin/env"};
  spec.command = {"python", "-m", "app"};
  spec.environment = {{"APP_ENV", "test"}, {"PORT", "8080"}};
  spec.working_directory = "/app";
  spec.user = "10001:10001";
  spec.labels = {{"com.example.component", "api"}};
  spec.cpu_millis = 250;
  spec.memory_mb = 192;
  spec.network_mode = "bridge";
  spec.ports = {{8080, "tcp", "127.0.0.1", 0}};
  spec.read_only_root_filesystem = true;
  if (health) {
    spec.health = DockerHealthConfig{{"/app/healthcheck"}, 5s, 1s, 2s, 4};
  }
  return spec;
}

WorkloadSpec sample_workload(std::uint64_t revision = 1, bool health = true,
                             std::uint64_t controller_epoch = 1) {
  WorkloadSpec spec;
  spec.workload_id = "orders-api";
  spec.revision = revision;
  spec.controller_epoch = controller_epoch;
  spec.container = sample_container(health);
  spec.restart = RuntimeRestartPolicy{100ms, 2.0, 500ms, 2, 1s};
  spec.stop_timeout = 3s;
  return spec;
}

RuntimeCommand ensure_command(const std::string &command_id,
                              std::uint64_t revision = 1, bool health = true,
                              std::uint64_t controller_epoch = 1) {
  return {
      command_id,
      RuntimeCommandKind::Ensure,
      "orders-api",
      revision,
      sample_workload(revision, health, controller_epoch),
      std::nullopt,
      controller_epoch,
  };
}

std::string multiplex_frame(unsigned char stream, const std::string &payload) {
  std::string value(8, '\0');
  value[0] = static_cast<char>(stream);
  const auto length = static_cast<std::uint32_t>(payload.size());
  value[4] = static_cast<char>((length >> 24U) & 0xffU);
  value[5] = static_cast<char>((length >> 16U) & 0xffU);
  value[6] = static_cast<char>((length >> 8U) & 0xffU);
  value[7] = static_cast<char>(length & 0xffU);
  value += payload;
  return value;
}

void test_endpoint_parsing_and_remote_guard() {
  const auto unix_endpoint =
      DockerEndpoint::parse("unix:///var/run/docker.sock");
  CHECK(unix_endpoint.kind == DockerEndpointKind::UnixSocket);
  CHECK(unix_endpoint.address == "/var/run/docker.sock");

  const auto pipe = DockerEndpoint::parse("npipe:////./pipe/docker_engine");
  CHECK(pipe.kind == DockerEndpointKind::WindowsNamedPipe);
  CHECK(pipe.address == R"(\\.\pipe\docker_engine)");

  const auto tcp = DockerEndpoint::parse("tcp://127.0.0.1:2375");
  CHECK(tcp.kind == DockerEndpointKind::Tcp);
  CHECK(tcp.base_url == "http://127.0.0.1:2375");
  CHECK(tcp.loopback);
  CHECK(DockerEndpoint::parse("tcp://[::1]:2375").loopback);

  const auto remote = DockerEndpoint::parse("https://docker.example:2376");
  CHECK(!remote.loopback);
  auto transport = std::make_shared<RecordingTransport>();
  expect_throw<std::invalid_argument>(
      [&] { DockerClient(DockerClientOptions{remote}, transport); });
  DockerClientOptions explicitly_remote{remote};
  explicitly_remote.allow_remote_tcp = true;
  DockerClient allowed(explicitly_remote, transport);
  CHECK(allowed.options().allow_remote_tcp);

  const DockerEndpoint forged{DockerEndpointKind::Tcp,
                              "http://docker.example:2375",
                              "http://docker.example:2375", true};
  expect_throw<std::invalid_argument>(
      [&] { DockerClient(DockerClientOptions{forged}, transport); });

  expect_throw<std::invalid_argument>(
      [] { DockerEndpoint::parse("unix://relative.sock"); });
  expect_throw<std::invalid_argument>(
      [] { DockerEndpoint::parse("tcp://localhost"); });
  expect_throw<std::invalid_argument>(
      [] { DockerEndpoint::parse("tcp://2001:db8::1:2375"); });
  expect_throw<std::invalid_argument>(
      [] { DockerEndpoint::parse("http://user:password@localhost:2375"); });
  expect_throw<std::invalid_argument>(
      [] { DockerEndpoint::parse("npipe:////./not-pipe/docker_engine"); });

  DockerClientOptions excessive_timeout{
      DockerEndpoint::parse("unix:///var/run/docker.sock")};
  excessive_timeout.limits.request_timeout = 6min;
  expect_throw<std::invalid_argument>(
      [&] { DockerClient(excessive_timeout, transport); });
}

void test_create_payload_has_limits_health_ports_and_no_engine_restart() {
  const auto payload = DockerClient::create_payload(sample_container());
  CHECK(payload["Image"].get<std::string>().find("@sha256:") !=
        std::string::npos);
  CHECK(payload["Entrypoint"][0] == "/usr/bin/env");
  CHECK(payload["Cmd"].size() == 3);
  CHECK(payload["User"] == "10001:10001");
  CHECK(payload["Env"][0] == "APP_ENV=test");
  CHECK(payload["Env"][1] == "PORT=8080");
  CHECK(payload["HostConfig"]["NanoCpus"] == 250'000'000LL);
  CHECK(payload["HostConfig"]["Memory"] == 192LL * 1024LL * 1024LL);
  CHECK(payload["HostConfig"]["PidsLimit"] == 256);
  CHECK(payload["HostConfig"]["ReadonlyRootfs"] == true);
  CHECK(payload["HostConfig"]["CapDrop"][0] == "ALL");
  CHECK(payload["HostConfig"]["SecurityOpt"][0] == "no-new-privileges:true");
  CHECK(payload["HostConfig"]["RestartPolicy"]["Name"] == "no");
  CHECK(payload["HostConfig"]["PortBindings"]["8080/tcp"][0]["HostIp"] ==
        "127.0.0.1");
  CHECK(payload["Healthcheck"]["Test"][0] == "CMD");
  CHECK(payload["Healthcheck"]["Test"][1] == "/app/healthcheck");
  CHECK(payload["Healthcheck"]["Retries"] == 4);

  auto invalid = sample_container();
  invalid.ports[0].host_ip = "0.0.0.0";
  expect_throw<std::invalid_argument>(
      [&] { DockerClient::create_payload(invalid); });
  invalid = sample_container();
  invalid.labels["bad\nlabel"] = "value";
  expect_throw<std::invalid_argument>(
      [&] { DockerClient::create_payload(invalid); });
  invalid = sample_container();
  invalid.network_mode = "none";
  expect_throw<std::invalid_argument>(
      [&] { DockerClient::create_payload(invalid); });
  invalid = sample_container();
  invalid.ports.push_back({8081, "tcp", "127.0.0.1", 40123});
  invalid.ports[0].host_port = 40123;
  expect_throw<std::invalid_argument>(
      [&] { DockerClient::create_payload(invalid); });
  invalid = sample_container();
  invalid.user = "root\nInjected";
  expect_throw<std::invalid_argument>(
      [&] { DockerClient::create_payload(invalid); });
}

void test_docker_http_lifecycle_and_log_demultiplexing() {
  auto transport = std::make_shared<RecordingTransport>();
  const nlohmann::json inspection = {
      {"Id", "abc123"},
      {"Name", "/orders"},
      {"State",
       {
           {"Status", "running"},
           {"Running", true},
           {"ExitCode", 0},
           {"Health", {{"Status", "healthy"}}},
       }},
      {"Config", {{"Labels", {{"io.minicloud.managed", "true"}}}}},
      {"NetworkSettings",
       {{"Ports",
         {
             {"8080/tcp", nlohmann::json::array({{
                              {"HostIp", "127.0.0.1"},
                              {"HostPort", "49152"},
                          }})},
         }}}},
  };
  transport->responses = {
      {201, {}, R"({"Id":"abc123"})"},
      {204, {}, {}},
      {200, {}, inspection.dump()},
      {200,
       {},
       multiplex_frame(1, "hello\n") + multiplex_frame(2, "warning\n")},
      {204, {}, {}},
      {204, {}, {}},
  };
  DockerClient client(
      DockerClientOptions{DockerEndpoint::parse("unix:///var/run/docker.sock")},
      transport);

  const auto id = client.create("orders", sample_container());
  client.start(id);
  const auto observed = client.inspect(id);
  const auto logs = client.logs(id, 50, true);
  client.stop(id, 3s);
  client.remove(id, false, false);

  CHECK(id == "abc123");
  CHECK(observed.has_value());
  CHECK(observed->running);
  CHECK(observed->health_status == "healthy");
  CHECK(observed->published_ports.size() == 1);
  CHECK(observed->published_ports[0].host_port == 49152);
  CHECK(logs.stdout_text == "hello\n");
  CHECK(logs.stderr_text == "warning\n");
  CHECK(logs.records.size() == 2);
  CHECK(logs.records[0].stream == DockerLogs::Stream::Stdout);
  CHECK(logs.records[0].text == "hello\n");
  CHECK(logs.records[1].stream == DockerLogs::Stream::Stderr);
  CHECK(transport->calls.size() == 6);
  CHECK(transport->calls[0].request.method == "POST");
  CHECK(transport->calls[0].request.path ==
        "/v1.43/containers/create?name=orders");
  CHECK(transport->calls[1].request.path == "/v1.43/containers/abc123/start");
  CHECK(transport->calls[4].request.path.find("/stop?t=3") !=
        std::string::npos);
  CHECK(transport->calls[5].request.method == "DELETE");
}

void test_worker_command_replay_and_content_collision() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", docker);
  const auto now = WorkerRuntime::Clock::time_point{};

  const CommandResult first = runtime.apply(ensure_command("command-1"), now);
  const auto calls_after_first = docker->calls.size();
  const CommandResult replay =
      runtime.apply(ensure_command("command-1"), now + 1ms);

  CHECK(first.succeeded);
  CHECK(first.state == WorkloadState::Starting);
  CHECK(!first.replayed);
  CHECK(replay.replayed);
  CHECK(docker->calls.size() == calls_after_first);
  CHECK(docker->count_prefix("create:") == 1);
  CHECK(docker->count_prefix("start:") == 1);

  auto changed = ensure_command("command-1", 2);
  expect_throw<std::invalid_argument>(
      [&] { runtime.apply(changed, now + 2ms); });

  auto mutated_policy = ensure_command("changed-policy", 1);
  mutated_policy.desired->restart.max_restarts = 99;
  const auto immutable = runtime.apply(mutated_policy, now + 3ms);
  CHECK(!immutable.succeeded);
  CHECK(immutable.message.find("immutable") != std::string::npos);

  const auto name = WorkerRuntime::container_name_for("orders-api");
  const auto &labels = docker->containers.at(name).labels;
  CHECK(labels.at("io.minicloud.managed") == "true");
  CHECK(labels.at("io.minicloud.worker-id") == "worker-a");
  CHECK(labels.at("io.minicloud.worker-epoch") == "1");
  CHECK(labels.at("io.minicloud.controller-epoch") == "1");
  CHECK(labels.at("io.minicloud.workload-id") == "orders-api");
  CHECK(labels.at("io.minicloud.revision") == "1");
  CHECK(name.rfind("mc-", 0) == 0);

  auto invalid_kind = ensure_command("invalid-kind");
  invalid_kind.kind = static_cast<RuntimeCommandKind>(999);
  const auto invalid_result = runtime.apply(invalid_kind, now + 4ms);
  CHECK(!invalid_result.succeeded);
  CHECK(invalid_result.message.find("kind is invalid") != std::string::npos);
}

void test_worker_never_touches_an_unowned_name_collision() {
  auto docker = std::make_shared<FakeDocker>();
  const auto name = WorkerRuntime::container_name_for("orders-api");
  ContainerInspection foreign;
  foreign.id = "foreign-container";
  foreign.name = name;
  foreign.running = true;
  foreign.labels = {{"owner", "someone-else"}};
  docker->containers[name] = foreign;
  docker->specs[name] = sample_container();
  WorkerRuntime runtime("worker-a", docker);

  const auto result = runtime.apply(ensure_command("command-foreign"));

  CHECK(!result.succeeded);
  CHECK(result.message.find("outside MiniCloud ownership") !=
        std::string::npos);
  CHECK(docker->count_prefix("stop:") == 0);
  CHECK(docker->count_prefix("remove:") == 0);
  CHECK(docker->containers.at(name).running);
}

void test_lost_create_response_recovers_without_duplicate_container() {
  auto docker = std::make_shared<FakeDocker>();
  docker->throw_after_create = true;
  WorkerRuntime runtime("worker-a", docker);
  const auto base = WorkerRuntime::Clock::time_point{};

  const auto accepted = runtime.apply(ensure_command("lost-response"), base);
  CHECK(!accepted.succeeded);
  CHECK(accepted.retryable);
  CHECK(accepted.state == WorkloadState::Backoff);
  CHECK(docker->count_prefix("create:") == 1);
  CHECK(docker->count_prefix("start:") == 0);

  runtime.tick(base + 100ms);
  CHECK(docker->count_prefix("create:") == 1);
  CHECK(docker->count_prefix("start:") == 1);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Starting);
}

void test_transient_docker_outage_does_not_consume_restart_budget() {
  auto docker = std::make_shared<FakeDocker>();
  docker->inspect_failures = 1;
  WorkerRuntime runtime("worker-a", docker);
  const auto base = WorkerRuntime::Clock::time_point{};

  const auto accepted = runtime.apply(ensure_command("transient"), base);
  CHECK(!accepted.succeeded);
  CHECK(accepted.retryable);
  CHECK(accepted.state == WorkloadState::Backoff);
  CHECK(runtime.snapshot("orders-api")->restart_count == 0);

  runtime.tick(base + 100ms);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Starting);
  CHECK(runtime.snapshot("orders-api")->restart_count == 0);

  const std::string metrics = runtime.prometheus_metrics();
  CHECK(metrics.find(
            "minicloud_runtime_docker_errors_total{operation=\"ensure\"} 1") !=
        std::string::npos);
}

void test_retryable_stop_is_not_cached() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", docker);
  runtime.apply(ensure_command("ensure-before-stop", 1, false));
  docker->stop_failures = 1;
  const RuntimeCommand stop{"retry-stop",
                            RuntimeCommandKind::Stop,
                            "orders-api",
                            2,
                            std::nullopt,
                            7s,
                            1};

  const auto failed = runtime.apply(stop);
  CHECK(!failed.succeeded);
  CHECK(failed.retryable);
  CHECK(!failed.replayed);
  const auto retried = runtime.apply(stop);
  CHECK(retried.succeeded);
  CHECK(!retried.replayed);
  CHECK(docker->count_prefix("stop:") == 2);
  CHECK(docker->calls.back().find(":7") != std::string::npos);
  const auto replay = runtime.apply(stop);
  CHECK(replay.succeeded);
  CHECK(replay.replayed);
  CHECK(docker->count_prefix("stop:") == 2);
}

void test_worker_epoch_takeover_and_fencing() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime current("worker-a", 2, docker);
  const auto base = WorkerRuntime::Clock::time_point{};
  CHECK(current.apply(ensure_command("epoch-2"), base).succeeded);
  const auto name = WorkerRuntime::container_name_for("orders-api");
  CHECK(docker->containers.at(name).labels.at("io.minicloud.worker-epoch") ==
        "2");

  current.advance_worker_epoch(3, base + 1ms);
  current.tick(base + 1ms);
  CHECK(docker->count_prefix("remove:") == 1);
  CHECK(docker->count_prefix("create:") == 2);
  CHECK(docker->containers.at(name).labels.at("io.minicloud.worker-epoch") ==
        "3");
  expect_throw<std::invalid_argument>(
      [&] { current.advance_worker_epoch(3, base + 2ms); });

  WorkerRuntime stale("worker-a", 2, docker);
  const auto stale_result =
      stale.apply(ensure_command("stale-worker"), base + 3ms);
  CHECK(!stale_result.succeeded);
  CHECK(stale_result.message.find("newer worker epoch") != std::string::npos);
  CHECK(docker->count_prefix("remove:") == 1);
}

void test_cross_worker_handoff_requires_strictly_newer_revision() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime worker_a("worker-a", 4, docker);
  WorkerRuntime worker_b("worker-b", 2, docker);
  const auto base = WorkerRuntime::Clock::time_point{};

  CHECK(worker_a.apply(ensure_command("worker-a-v1", 1, false, 7), base)
            .succeeded);
  const auto name = WorkerRuntime::container_name_for("orders-api");
  const auto old_container_id = docker->containers.at(name).id;

  auto handoff = ensure_command("worker-b-v2", 2, false, 7);
  handoff.desired->container.image =
      "example.local/team/api@sha256:" + std::string(64, 'b');
  const auto accepted = worker_b.apply(handoff, base + 1ms);

  CHECK(accepted.succeeded);
  CHECK(docker->containers.at(name).id != old_container_id);
  CHECK(docker->containers.at(name).labels.at("io.minicloud.worker-id") ==
        "worker-b");
  CHECK(docker->containers.at(name).labels.at("io.minicloud.worker-epoch") ==
        "2");
  CHECK(docker->containers.at(name).labels.at(
            "io.minicloud.controller-epoch") == "7");
  CHECK(docker->containers.at(name).labels.at("io.minicloud.revision") == "2");
  CHECK(docker->count_prefix("stop:") == 1);
  CHECK(docker->count_prefix("remove:") == 1);

  worker_a.tick(base + 2ms);
  CHECK(worker_a.snapshot("orders-api")->state == WorkloadState::Failed);
  CHECK(docker->count_prefix("remove:") == 1);
  CHECK(docker->containers.at(name).labels.at("io.minicloud.worker-id") ==
        "worker-b");
}

void test_cross_worker_handoff_refuses_unsafe_identity_or_generation() {
  {
    auto docker = std::make_shared<FakeDocker>();
    WorkerRuntime worker_a("worker-a", 1, docker);
    WorkerRuntime worker_b("worker-b", 1, docker);
    CHECK(worker_a.apply(ensure_command("same-revision-a", 1, false, 7))
              .succeeded);

    const auto refused =
        worker_b.apply(ensure_command("same-revision-b", 1, false, 8));
    CHECK(!refused.succeeded);
    CHECK(refused.message.find("same or a newer revision") !=
          std::string::npos);
    CHECK(docker->count_prefix("remove:") == 0);
  }
  {
    auto docker = std::make_shared<FakeDocker>();
    WorkerRuntime worker_a("worker-a", 1, docker);
    WorkerRuntime worker_b("worker-b", 1, docker);
    CHECK(worker_a.apply(ensure_command("mismatch-a", 1, false, 7)).succeeded);
    const auto name = WorkerRuntime::container_name_for("orders-api");
    docker->containers.at(name).labels["io.minicloud.workload-id"] =
        "another-allocation";

    const auto refused =
        worker_b.apply(ensure_command("mismatch-b", 2, false, 7));
    CHECK(!refused.succeeded);
    CHECK(refused.message.find("outside MiniCloud ownership") !=
          std::string::npos);
    CHECK(docker->count_prefix("remove:") == 0);
  }
  {
    auto docker = std::make_shared<FakeDocker>();
    WorkerRuntime worker_a("worker-a", 1, docker);
    WorkerRuntime worker_b("worker-b", 1, docker);
    CHECK(worker_a.apply(ensure_command("new-controller-a", 1, false, 9))
              .succeeded);

    const auto refused =
        worker_b.apply(ensure_command("old-controller-b", 2, false, 8));
    CHECK(!refused.succeeded);
    CHECK(refused.message.find("newer controller epoch") != std::string::npos);
    CHECK(docker->count_prefix("remove:") == 0);
  }
}

void test_controller_epoch_refreshes_owned_container_fence() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", 3, docker);
  CHECK(runtime.apply(ensure_command("controller-7", 1, false, 7)).succeeded);
  const auto name = WorkerRuntime::container_name_for("orders-api");
  const auto old_container_id = docker->containers.at(name).id;

  const auto refreshed =
      runtime.apply(ensure_command("controller-8", 1, false, 8));
  CHECK(refreshed.succeeded);
  CHECK(docker->containers.at(name).id != old_container_id);
  CHECK(docker->containers.at(name).labels.at(
            "io.minicloud.controller-epoch") == "8");
  CHECK(docker->count_prefix("remove:") == 1);

  const auto stale =
      runtime.apply(ensure_command("stale-controller", 2, false, 7));
  CHECK(!stale.succeeded);
  CHECK(stale.message.find("stale controller epoch") != std::string::npos);
  CHECK(docker->count_prefix("remove:") == 1);
}

void test_cross_worker_remove_uses_the_same_revision_fence() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime worker_a("worker-a", 1, docker);
  WorkerRuntime worker_b("worker-b", 1, docker);
  CHECK(worker_a.apply(ensure_command("remove-source", 1, false, 7)).succeeded);

  const RuntimeCommand equal_revision{"remove-equal",
                                      RuntimeCommandKind::Remove,
                                      "orders-api",
                                      1,
                                      std::nullopt,
                                      2s,
                                      7};
  const auto refused = worker_b.apply(equal_revision);
  CHECK(!refused.succeeded);
  CHECK(docker->count_prefix("remove:") == 0);

  const RuntimeCommand newer_revision{"remove-newer",
                                      RuntimeCommandKind::Remove,
                                      "orders-api",
                                      2,
                                      std::nullopt,
                                      2s,
                                      7};
  const auto removed = worker_b.apply(newer_revision);
  CHECK(removed.succeeded);
  CHECK(docker->count_prefix("remove:") == 1);
  CHECK(docker->containers.empty());
}

void test_new_revision_replaces_only_the_owned_container() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", docker);
  const auto now = WorkerRuntime::Clock::time_point{};
  CHECK(runtime.apply(ensure_command("ensure-v1", 1), now).succeeded);

  auto v2 = ensure_command("ensure-v2", 2);
  v2.desired->container.image =
      "example.local/team/api@sha256:" + std::string(64, 'b');
  const auto updated = runtime.apply(v2, now + 1s);

  CHECK(updated.succeeded);
  CHECK(updated.revision == 2);
  CHECK(docker->count_prefix("stop:") == 1);
  CHECK(docker->count_prefix("remove:") == 1);
  CHECK(docker->count_prefix("create:") == 2);
  const auto name = WorkerRuntime::container_name_for("orders-api");
  CHECK(docker->containers.at(name).labels.at("io.minicloud.revision") == "2");

  const auto stale = runtime.apply(ensure_command("stale-v1", 1), now + 2s);
  CHECK(!stale.succeeded);
  CHECK(docker->count_prefix("remove:") == 1);
}

void test_health_and_bounded_restart_backoff() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", docker);
  const auto base = WorkerRuntime::Clock::time_point{};
  runtime.apply(ensure_command("start", 1, true), base);
  const auto name = WorkerRuntime::container_name_for("orders-api");
  docker->containers.at(name).health_status = "healthy";

  runtime.tick(base + 10ms);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Healthy);

  docker->containers.at(name).running = false;
  docker->containers.at(name).status = "exited";
  runtime.tick(base + 20ms);
  auto snapshot = runtime.snapshot("orders-api");
  CHECK(snapshot->state == WorkloadState::Backoff);
  CHECK(snapshot->restart_count == 1);
  CHECK(snapshot->next_restart_at == base + 120ms);
  const auto starts_before = docker->count_prefix("start:");

  runtime.tick(base + 119ms);
  CHECK(docker->count_prefix("start:") == starts_before);
  runtime.tick(base + 120ms);
  CHECK(docker->count_prefix("start:") == starts_before + 1);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Starting);

  docker->containers.at(name).running = false;
  runtime.tick(base + 130ms);
  snapshot = runtime.snapshot("orders-api");
  CHECK(snapshot->restart_count == 2);
  CHECK(snapshot->next_restart_at == base + 330ms);
  runtime.tick(base + 330ms);
  docker->containers.at(name).running = false;
  runtime.tick(base + 340ms);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Failed);
  CHECK(runtime.snapshot("orders-api")->last_error.find("restart limit") !=
        std::string::npos);

  const std::string metrics = runtime.prometheus_metrics();
  CHECK(metrics.find("minicloud_runtime_restarts_total{reason=\"exit\"} 2") !=
        std::string::npos);
  CHECK(metrics.find("minicloud_runtime_workloads{state=\"failed\"} 1") !=
        std::string::npos);
}

void test_startup_timeout_stops_never_ready_container() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", docker);
  const auto base = WorkerRuntime::Clock::time_point{};
  auto command = ensure_command("slow-start", 1, true);
  command.desired->restart.startup_timeout = 50ms;
  runtime.apply(command, base);

  runtime.tick(base + 49ms);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Starting);
  CHECK(docker->count_prefix("stop:") == 0);
  runtime.tick(base + 50ms);
  CHECK(runtime.snapshot("orders-api")->state == WorkloadState::Backoff);
  CHECK(runtime.snapshot("orders-api")->restart_count == 1);
  CHECK(docker->count_prefix("stop:") == 1);
}

void test_stop_remove_and_revision_fencing() {
  auto docker = std::make_shared<FakeDocker>();
  WorkerRuntime runtime("worker-a", docker);
  runtime.apply(ensure_command("ensure", 3, false));

  const RuntimeCommand stale_stop{
      "stop-old", RuntimeCommandKind::Stop, "orders-api", 2, std::nullopt, 2s,
      1};
  const auto stale = runtime.apply(stale_stop);
  CHECK(!stale.succeeded);
  CHECK(docker->count_prefix("stop:") == 0);

  const RuntimeCommand stop{"stop-current",
                            RuntimeCommandKind::Stop,
                            "orders-api",
                            4,
                            std::nullopt,
                            4s,
                            1};
  const auto stopped = runtime.apply(stop);
  CHECK(stopped.succeeded);
  CHECK(stopped.state == WorkloadState::Stopped);
  CHECK(docker->count_prefix("stop:") == 1);

  const RuntimeCommand remove{"remove-current",
                              RuntimeCommandKind::Remove,
                              "orders-api",
                              5,
                              std::nullopt,
                              5s,
                              1};
  const auto removed = runtime.apply(remove);
  CHECK(removed.succeeded);
  CHECK(removed.state == WorkloadState::Removed);
  CHECK(docker->count_prefix("remove:") == 1);
  CHECK(docker->containers.empty());
}

void run(const char *name, const std::function<void()> &test) {
  const int before = failures;
  try {
    test();
  } catch (const std::exception &error) {
    std::cerr << name << " threw: " << error.what() << '\n';
    ++failures;
  } catch (...) {
    std::cerr << name << " threw an unknown exception\n";
    ++failures;
  }
  std::cout << (failures == before ? "PASS " : "FAIL ") << name << '\n';
}

} // namespace

int main() {
  run("endpoint parsing and remote guard",
      test_endpoint_parsing_and_remote_guard);
  run("create payload",
      test_create_payload_has_limits_health_ports_and_no_engine_restart);
  run("Docker HTTP lifecycle",
      test_docker_http_lifecycle_and_log_demultiplexing);
  run("worker replay", test_worker_command_replay_and_content_collision);
  run("ownership collision",
      test_worker_never_touches_an_unowned_name_collision);
  run("lost create response",
      test_lost_create_response_recovers_without_duplicate_container);
  run("transient Docker outage",
      test_transient_docker_outage_does_not_consume_restart_budget);
  run("retryable stop", test_retryable_stop_is_not_cached);
  run("worker epoch fencing", test_worker_epoch_takeover_and_fencing);
  run("cross-worker handoff",
      test_cross_worker_handoff_requires_strictly_newer_revision);
  run("cross-worker handoff fencing",
      test_cross_worker_handoff_refuses_unsafe_identity_or_generation);
  run("controller epoch refresh",
      test_controller_epoch_refreshes_owned_container_fence);
  run("cross-worker remove fencing",
      test_cross_worker_remove_uses_the_same_revision_fence);
  run("revision replacement",
      test_new_revision_replaces_only_the_owned_container);
  run("health and restart backoff", test_health_and_bounded_restart_backoff);
  run("startup timeout", test_startup_timeout_stops_never_ready_container);
  run("stop remove fencing", test_stop_remove_and_revision_fencing);
  if (failures != 0) {
    std::cerr << failures << " runtime test assertion(s) failed\n";
    return 1;
  }
  std::cout << "All C++ runtime tests passed\n";
  return 0;
}

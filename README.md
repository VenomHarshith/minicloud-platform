# MiniCloud Platform

[![CI](https://github.com/VenomHarshith/minicloud-platform/actions/workflows/ci.yml/badge.svg)](https://github.com/VenomHarshith/minicloud-platform/actions/workflows/ci.yml)

MiniCloud is a real, zero-cost mini container platform for a trusted local
machine. You submit a Docker image and desired replica count; its C++ control
plane stores desired state, schedules replicas onto worker nodes, sends fenced
gRPC commands, supervises Docker containers, publishes healthy endpoints,
load-balances requests, streams logs, exports Prometheus metrics, and shows live
state in a React dashboard.

This is a separate project from the earlier LocalPlane repository. LocalPlane is
not replaced or removed.

For one end-to-end explanation of the product, architecture, technologies,
runtime flows, commands, troubleshooting, costs, and limitations, read the
[complete project guide](docs/COMPLETE_PROJECT_GUIDE.md).

## What it does

- Accepts container service definitions through REST or `minicloudctl`.
- Schedules with CPU, RAM, required labels, and soft anti-affinity.
- Simulates two worker nodes locally while both use the trusted local Docker
  engine; the protocol can connect workers on separate engines, with the v0.1
  failover limitation documented below.
- Applies CPU, memory, PID, capability, privilege, and network settings through
  the Docker Engine API.
- Uses desired-versus-observed reconciliation and an idempotent PostgreSQL
  command outbox.
- Fences stale worker epochs, container revisions, command leases, and container
  ownership before destructive operations.
- Uses Docker health checks, bounded restart backoff, and a restart ceiling.
- Registers ready endpoints in Valkey and load-balances them through a C++ HTTP
  gateway.
- Retains bounded recent logs in Valkey and exposes them in the dashboard.
- Exports controller, worker, gateway, scheduler, command, restart, and runtime
  metrics for Prometheus.
- Runs on Windows, macOS, and Linux through Docker Desktop/Engine. The pure C++
  scheduler and reconciliation core also builds natively with MSVC, Clang, or GCC.

## Architecture

```text
React console / minicloudctl
             |
             | REST :8090
             v
   C++ controller + reconciler ---- PostgreSQL (durable truth)
             |             |
             | gRPC :50051 +---------- Valkey (ephemeral discovery/log tails)
             v                                |
      C++ worker agents                       v
             |                         C++ gateway :8080
             | Docker Engine API               |
             v                                 v
       workload containers <------------- client traffic

All C++ components ---------- /metrics ----------> Prometheus :9090
```

The complete component, state, and failure model is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Fastest run

Prerequisites:

- Docker Desktop or Docker Engine with Compose v2.
- At least 6 GB available to Docker for the two logical workers and build.
- Git. VS Code is optional but the repository includes tasks.
- On macOS/Linux, the shortcut targets also use `make` and the end-to-end proof
  uses host `curl`.

No cloud account, paid API, hosted database, or subscription is required. The
first build downloads public container images and free/open-source build
packages; later runs reuse Docker's cache.

### Windows PowerShell

Use Docker Desktop in **Linux containers** mode:

```powershell
.\deploy\powershell\Initialize-MiniCloud.ps1
.\scripts\windows\Start-MiniCloud.ps1
.\scripts\windows\Deploy-Echo.ps1
```

See [docs/WINDOWS.md](docs/WINDOWS.md) for WSL 2, socket, native C++, and
troubleshooting details.

### macOS with Docker Desktop

```bash
make init
make up-desktop
make demo
```

### Linux with Docker Engine

```bash
make init
make up
make demo
```

Open:

- Dashboard: <http://127.0.0.1:3000>
- Workload gateway: <http://127.0.0.1:8080>
- Controller API: <http://127.0.0.1:8090>
- Prometheus: <http://127.0.0.1:9090>

`make demo` builds and deploys the included echo image with two replicas, waits
for health and discovery, sends a real request through the gateway, and exercises
terminal-failure and delayed-drain recovery against the live control plane.

Stop without deleting databases or metrics:

```bash
make down
```

The stop command also removes ephemeral MiniCloud-managed workload containers
so Compose can remove the workload network. Desired service state remains in
PostgreSQL and is reconciled when the platform starts again.

Windows:

```powershell
.\scripts\windows\Stop-MiniCloud.ps1
```

Fresh databases apply every checked-in migration automatically. Before starting
this revision with an existing prerelease PostgreSQL volume, follow the
non-destructive schema upgrade in
[docs/OPERATIONS.md](docs/OPERATIONS.md#upgrade-an-existing-prerelease-database).

## Share a safe live observer

MiniCloud can publish a **sanitized, read-only view of the real running
cluster** through a free Cloudflare Quick Tunnel. The public edge exposes only
`GET /api/v1/snapshot`; deploy, scale, restart, delete, logs, internal IDs,
container endpoints, Prometheus, the workload gateway, and the control plane
remain private. The browser never receives the controller token.

This is a temporary portfolio/demo URL, not static hosting and not a publicly
controllable cloud. It works only while the machine, Docker stack, and tunnel
remain online, and its random URL can change after restart. Do not expose the
normal administrative dashboard instead.

Follow [docs/PUBLIC_OBSERVER.md](docs/PUBLIC_OBSERVER.md) for the architecture,
security boundary, exact Linux/macOS/Windows commands, verification checks,
safe shutdown, costs, Quick Tunnel limits, and the optional OCI Always Free
path. The shorter hosting comparison is in
[docs/ONLINE_DEPLOYMENT.md](docs/ONLINE_DEPLOYMENT.md).

## Submit your own container

Create `examples/orders-service.json`:

```json
{
  "name": "orders-api",
  "image": "your-registry/orders-api@sha256:immutable-digest",
  "replicas": 2,
  "cpuMillis": 300,
  "memoryMb": 256,
  "containerPort": 8080,
  "healthPath": "/health",
  "placement": {
    "requiredLabels": {"os": "linux-containers"},
    "antiAffinityGroup": "orders"
  }
}
```

Submit it with the containerized CLI, which loads the API token from
`deploy/.env`:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli \
  deploy /workspace/examples/orders-service.json
```

For the first release, the direct `curl` workflow in
[docs/API.md](docs/API.md) is the simplest way to submit a host file. Images
must already be accessible to the local Docker engine. Immutable digests are
recommended; tags are allowed for local iteration.

## What “Windows support” means

- The supported full-stack path is Docker Desktop with WSL 2 and Linux
  containers.
- PowerShell scripts perform bootstrap, start, deploy, and stop operations.
- Every published port binds to `127.0.0.1`.
- The runtime understands both Unix Docker sockets and Windows
  `npipe:////./pipe/docker_engine` for native worker builds.
- The dependency-free scheduler/reconciler suite is compiled by Windows CI with
  MSVC.
- Paths and process-control code avoid POSIX-only assumptions.

## Honest boundaries

MiniCloud is an engineering-grade local/educational platform, not a production
replacement for Kubernetes or ECS.

- The default two workers are logical nodes sharing one Docker engine. Treat
  separate worker hosts/engines as an advanced experiment: v0.1 can reschedule
  after an agent heartbeat expires, but it has no remote-daemon garbage
  collector if the old host keeps containers running while its agent is
  unavailable.
- The local gRPC network uses a cluster token. Remote deployments must add mTLS.
- PostgreSQL is a single instance; there is no multi-controller leader election
  in v0.1.
- Valkey discovery/log tails are intentionally ephemeral; PostgreSQL remains the
  durable source of truth.
- A worker has Docker-socket authority, which is effectively host-root
  capability. Run only trusted images and never expose the controller publicly.
- CPU and memory limits use Docker/cgroups. Enforcement behavior depends on the
  Docker Desktop VM and host configuration.
- The generated HTTP health command expects `wget` in the workload image in
  v0.1. The included example satisfies that contract.

Read [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) before changing bind addresses
or Docker endpoint settings.

## Repository guide

- `cpp/core/` — deterministic scheduling and reconciliation algorithms.
- `cpp/controller/` — REST, gRPC, PostgreSQL state, outbox, and reconciliation.
- `cpp/runtime/` — Docker Engine client and safe worker state machine.
- `cpp/worker/` — gRPC worker loop, status, logs, and endpoint publication.
- `cpp/gateway/` — discovery-backed reverse proxy and round-robin balancing.
- `proto/` — controller/worker compatibility contract.
- `dashboard/` — React + TypeScript operations UI.
- `database/migrations/` — durable PostgreSQL schema.
- `deploy/` — Compose, Prometheus, credentials, and cross-platform overrides.
- `deploy/compose.public.yaml` — sanitized observer and temporary HTTPS tunnel.
- `examples/echo-service/` — real end-to-end workload.
- `tests/` — portable core/runtime proofs.
- `docs/milestones/` — one build-and-learning record per milestone.
- `information-pack/` — ordered index for the documentation-only ZIP.

Start with [RUN_INSTRUCTIONS.md](RUN_INSTRUCTIONS.md), then use
[docs/COMPLETE_PROJECT_GUIDE.md](docs/COMPLETE_PROJECT_GUIDE.md) for the full
product and technology reference. [docs/BUILD_AND_LEARN.md](docs/BUILD_AND_LEARN.md)
is the milestone-oriented what/why/when/how companion while reading the code.

If you prefer a visual workflow, [docs/VSCODE.md](docs/VSCODE.md) maps the
checked-in VS Code tasks to bootstrap, start, inspect, test, and stop operations.

Create both a source ZIP and a documentation-only ZIP with `make package` on
macOS/Linux or `.\scripts\windows\Package-Release.ps1` on Windows. Generated
archives go to ignored `dist/`. Both commands require Git and archive only the
files tracked in the committed `HEAD`, so commit the intended release inputs
first; ignored credentials, logs, dependency caches, and build output stay out.
Tagged GitHub releases publish both archives plus `SHA256SUMS`; see
[RELEASE_NOTES.md](RELEASE_NOTES.md) for their contents and verification scope.

## Clean-room and cost policy

All implementation, names, diagrams, examples, and data in this repository are
original or based only on public documentation. Do not add employer/customer
source, private infrastructure details, credentials, logs, or other non-public
material.

The normal local path uses only free/open-source components and Docker Personal
for eligible personal/educational use. A public always-on deployment needs a
machine capable of running Docker; MiniCloud does not silently create or bill a
cloud resource.

The public GitHub repository runs the complete Docker proof in an ephemeral
Actions runner, and the optional public observer can share live sanitized state
from a running host. Neither is a permanently guaranteed cluster. See
[docs/ONLINE_DEPLOYMENT.md](docs/ONLINE_DEPLOYMENT.md) for the exact distinction
and the cost boundary.

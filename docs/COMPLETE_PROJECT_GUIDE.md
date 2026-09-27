# MiniCloud complete project guide

This is the single, end-to-end guide to MiniCloud: what the project is, why it
exists, how every major part works, how to run and operate it, what each
technology contributes, and where its current boundaries are. It describes the
actual v0.1 repository; it does not describe planned features as if they already
exist.

For the shortest command-only path, use [RUN_INSTRUCTIONS.md](../RUN_INSTRUCTIONS.md).
For the current verification ledger, use [PROGRESS.md](../PROGRESS.md).

## 1. Project definition

MiniCloud is a small container orchestration platform for one trusted local
machine. An operator gives it a Docker image, a replica count, CPU and memory
requests, a container port, a health path, and optional placement rules. The
platform then:

1. stores that desired state durably;
2. creates one allocation per desired replica;
3. selects a worker with matching labels and enough declared capacity;
4. delivers a fenced command to that worker over gRPC;
5. creates and supervises the Docker container;
6. publishes only healthy endpoints for routing;
7. load-balances requests across those endpoints;
8. collects recent logs and operational metrics; and
9. displays desired and observed state in a web console.

This is an actual systems project, not a learning-content application. The
documentation explains the engineering so that building and operating the real
platform becomes the learning experience.

### The central idea: desired state versus observed state

The operator says what should be true: for example, “run two replicas of
`echo-api`.” PostgreSQL records that desired state. Workers report what is true
now: perhaps one replica is healthy and one is still starting. The controller
repeatedly compares the two views and creates work that moves the system toward
the desired result. This repeating process is reconciliation.

That model is more resilient than a one-time “start this process” command. If a
worker restarts, a command is redelivered, or a container exits, the desired
state still exists and the platform can try to converge again.

### What MiniCloud is not

MiniCloud is deliberately not:

- a replacement for Kubernetes, AWS ECS, or another production orchestrator;
- a container runtime—Docker provides images, namespaces, cgroups, networking,
  and filesystem isolation;
- a hostile multi-tenant sandbox;
- a public cloud service or an always-on free hosting provider;
- a static website whose dashboard merely simulates an orchestrator;
- a private registry client or image builder for submitted services;
- a secret manager, persistent-volume system, or TLS ingress;
- a multi-controller, highly available control plane; or
- a safe internet-facing deployment in its default configuration.

The default Compose stack starts two logical workers, but both control the same
trusted local Docker engine. This is excellent for studying placement,
reconciliation, fencing, health, and routing on one laptop. It is not equivalent
to two physically isolated machines.

## 2. Why this project has real-life value

The implementation is small enough to inspect but contains patterns used in
real deployment platforms, workflow engines, build farms, device controllers,
storage systems, and distributed services.

### Practical use cases

- **Local service orchestration:** run several trusted development services
  from declarative JSON and access them through one gateway.
- **Container-platform engineering lab:** experiment with scheduling,
  reconciliation, health failure, worker loss, retry delivery, and stale-writer
  fencing without operating a full Kubernetes cluster.
- **Architecture portfolio project:** demonstrate C++, distributed state,
  transactional messaging, resource scheduling, observability, frontend work,
  cross-platform automation, and CI in one coherent product.
- **Failure-recovery test bed:** deliberately crash a workload, stop a worker,
  or restart Valkey and observe which data survives and how the system repairs
  itself.
- **Gateway and retry lab:** see why only idempotent HTTP operations are safe to
  retry automatically after an upstream transport failure.
- **Operations training:** correlate an API request with a database record,
  reconciliation event, leased command, container, health result, discovery
  endpoint, gateway request, log tail, and Prometheus metric.
- **Foundation for advanced work:** add rolling updates, autoscaling, mTLS,
  admission policy, signed-image checks, or a multi-host laboratory behind
  tests and architecture decisions.

MiniCloud should be used only with images and machines the operator trusts. A
worker that can control the Docker socket has host-powerful authority.

## 3. Architecture at a glance

```text
Operator
  |  dashboard or minicloudctl/curl
  |  authenticated REST on 127.0.0.1:8090
  v
C++ controller
  |-- PostgreSQL: durable services, nodes, allocations, commands, events
  |-- Valkey: expiring healthy endpoints and recent log tails
  |-- gRPC: registration, heartbeats, commands, status, and logs
  v
C++ worker-a and worker-b
  |  Docker Engine API through the trusted local socket
  v
Managed workload containers on minicloud-workloads
  ^
  |  HTTP selected round-robin from ready endpoints
C++ gateway on 127.0.0.1:8080

Controller + workers + gateway -- /metrics --> Prometheus
React dashboard <-- same-origin Nginx proxy --> controller REST API
```

There are two Docker networks:

- `control` connects PostgreSQL, Valkey, controller, workers, gateway,
  dashboard, and Prometheus.
- `minicloud-workloads` connects managed workload containers to the gateway.
  Workers create those containers through the Docker API; they do not need to
  join the workload network themselves.

PostgreSQL and Valkey are not published to the host. User-facing host ports are
bound to loopback only.

## 4. Components and responsibilities

### Controller

The C++ controller is the control plane. It provides:

- REST on port `8090` for desired-state operations and snapshots;
- gRPC on port `50051` for worker registration and control traffic;
- Prometheus metrics on internal port `9091`;
- a reconciliation loop, normally once per second;
- the PostgreSQL repository and transactional command outbox; and
- Valkey publication for healthy endpoints and recent logs.

On startup, the controller verifies schema version 4, acquires a PostgreSQL
advisory lock so a second controller cannot operate concurrently, and obtains a
new durable, monotonically increasing controller epoch.

### PostgreSQL

PostgreSQL is the durable source of truth. Important tables are:

| Table | Meaning |
|---|---|
| `services` | Desired image, replicas, CPU, RAM, port, health path, environment, placement, and generation. |
| `nodes` | Worker identity, process instance, epoch, status, capacity, labels, and last heartbeat. |
| `allocations` | Stable replica slots, selected node, desired/observed state, revision, runtime ID, endpoint, and restart count. |
| `commands` | Transactional outbox entries, lease state, attempt count, retry timing, and completion. |
| `events` | Recent control-plane decisions and operational audit context. |
| `status_report_receipts` | Twenty-four-hour durable idempotency receipts for worker observations. |
| `schema_migrations` | Explicit schema compatibility history. |

The controller writes allocation changes and their commands in the same
transaction. A crash therefore cannot commit intent while silently losing the
corresponding worker work item.

### Scheduler and reconciler core

`cpp/core` is a dependency-free C++20 library containing domain objects,
scheduling policy, reconciliation planning, stable identifiers, and tests. It
uses immutable input snapshots and deterministic ordering so the same state
produces the same decision across GCC, Clang, and MSVC.

The controller repository applies the same desired/observed model to
transactional database records.

### Workers

Each C++ worker:

- registers its name, fresh process instance ID, capacity, and labels;
- receives a worker epoch from the controller;
- heartbeats and polls for bounded batches of leased commands;
- validates controller epoch, worker ownership, generation, and revision;
- translates a bounded protobuf workload into Docker Engine JSON;
- creates, starts, inspects, stops, and removes managed containers;
- supervises startup, health, exit, and bounded restart backoff;
- reports allocation state and ready endpoints; and
- uploads ordered, deduplicated recent log batches.

The default `worker-a` and `worker-b` each advertise 4,000 CPU millicores and
4,096 MiB. These are scheduling declarations, not additional physical
resources. Both workers share the local Docker daemon in the supported laptop
topology.

### Docker runtime adapter

The runtime talks to the Docker Engine API rather than constructing shell
commands. It supports Unix sockets, Windows named pipes in a native worker, and
explicitly opted-in HTTP/HTTPS TCP endpoints. Non-loopback TCP is disabled by
default; v0.1 does not configure Docker client certificates, so any remote
transport protection and authentication must be supplied outside MiniCloud.
The normal full stack uses a Unix socket inside Linux worker containers; Docker
Desktop supplies that socket through the desktop override.

For each managed container, the runtime applies:

- CPU and memory limits;
- a PID limit of 256;
- a dedicated workload network;
- all Linux capabilities dropped;
- `no-new-privileges`;
- deterministic names and ownership labels;
- revision, epoch, and specification-fingerprint labels; and
- a Docker HTTP health command.

The v0.1 generated health command uses `wget` inside the workload container, so
submitted images must contain `wget` and serve the configured health path.

### Valkey

Valkey is an intentionally disposable data plane. It stores:

- per-service sets of ready endpoints with expiration; and
- recent per-allocation log lines plus log-batch deduplication keys.

An endpoint normally has a 15-second TTL and workers report status every five
seconds. If reports stop, the route expires. Log lists default to the most recent
2,000 lines, returned through the API with a requested tail of 1–1,000 lines;
the list and its batch receipts expire after 24 hours.

Losing Valkey can briefly remove routes and log tails, but cannot remove desired
services or allocation history because correctness-critical state is in
PostgreSQL.

### Gateway

The C++ gateway accepts:

```text
ANY /services/<service-name>/<upstream-path>
```

It looks up non-expired endpoints in Valkey, chooses them round-robin, forwards
the method, body, content type, and request ID, and applies bounded connect/read
timeouts and response sizes. It returns:

- `503` when no ready endpoint exists;
- the upstream response when forwarding succeeds; or
- `502` when the selected upstream connections fail.

On transport failure, it may try at most a second endpoint for idempotent
methods: `GET`, `HEAD`, `PUT`, `DELETE`, `OPTIONS`, and `TRACE`. It never
automatically retries `POST`, because replaying a non-idempotent action could
duplicate a side effect.

### Dashboard and Nginx

The React/TypeScript dashboard polls the snapshot every three seconds. It shows:

- service desired and ready replica counts;
- service image, resources, and generation;
- node status, heartbeat age, labels, and CPU/RAM reservations;
- allocation placement, endpoint, container ID, state, and restart count;
- recent controller events and allocation log tails; and
- controls to deploy, scale, and restart services.

The production frontend is compiled to static files and served by Nginx. Nginx
also proxies `/api/` to the controller and injects the generated API bearer
token, so the token is not embedded in browser JavaScript. The UI currently has
no delete button, but deletion is available through the CLI and REST API.

That default dashboard is an administrative interface and must remain private.
The optional public-observer build uses a different Nginx configuration and a
sanitizing facade: only the live snapshot allowlist is public, and mutation,
logs, raw identities, endpoints, errors, and private configuration remain
closed. See [PUBLIC_OBSERVER.md](PUBLIC_OBSERVER.md) before sharing any URL.

### Prometheus

Prometheus scrapes controller, gateway, and both workers every five seconds. It
stores operational time series in the `prometheus-data` named volume and is
published at `127.0.0.1:9090`.

Metrics include controller reconciliation, scheduling, commands, status and
logs; gateway requests and upstream errors; and worker commands, replays,
restarts, Docker errors, and workload state. Metrics are deliberately kept at
bounded cardinality—allocation IDs belong in events or logs, not labels.

### CLI

`minicloudctl` is a C++ HTTP client for status, deploy, scale, restart, delete,
and logs. The Compose `cli` service runs it with the controller URL and API
token already supplied. Its checked-in volume exposes the repository's
`examples/` directory as `/workspace/examples` inside the CLI container.

## 5. Technology guide: definition, why, when, and how

### C++20

**Definition:** C++20 is the language standard used for the controller, worker,
runtime, gateway, CLI, and pure scheduling/reconciliation core.

**Why here:** It provides native performance, explicit ownership, strong value
types, deterministic algorithms, threads, atomics, chrono types, and portable
standard-library facilities. A dependency-free core can be compiled on three
major toolchains without Docker or a database.

**When it is useful:** Long-running systems agents, gateways, schedulers,
resource-constrained processes, cross-platform developer tools, and software
where tight control over lifetime and failure boundaries matters.

**How MiniCloud uses it:** `cpp/core` contains pure policy; `cpp/controller`
owns REST/gRPC/state orchestration; `cpp/runtime` owns Docker and the worker
state machine; `cpp/gateway` owns proxying. Strict warning flags and warnings as
errors are enabled. Supporting libraries include Boost.Asio/Beast for HTTP,
libcurl for Docker/CLI HTTP transport, and nlohmann/json for JSON.

### Docker Engine and Docker Compose

**Definition:** Docker Engine builds images and runs isolated containers.
Compose describes a group of cooperating containers, networks, volumes,
healthchecks, and environment settings.

**Why here:** Docker provides the runtime mechanisms MiniCloud should not
reimplement. Compose makes the control plane reproducible on one machine.

**When it is useful:** Packaging services consistently, isolating processes,
running integration environments, applying cgroup limits, and making a complex
local stack repeatable.

**How MiniCloud uses it:** Compose starts PostgreSQL, Valkey, controller, two
logical workers, gateway, dashboard, Prometheus, and an optional CLI. Workers
then create managed workload containers directly through the Docker Engine API;
those workloads are not Compose services. The stop scripts remove only
containers with MiniCloud's ownership label on the workload network before
bringing Compose down.

Docker Desktop requires `deploy/compose.desktop.yaml`, which maps its raw VM
socket into the Linux worker containers. Linux Docker Engine uses the base
Compose file.

### gRPC and Protocol Buffers

**Definition:** Protocol Buffers define typed, versionable messages. gRPC uses
those messages to generate client/server APIs over HTTP/2.

**Why here:** Worker and controller code need an explicit compatibility
contract for registration, heartbeats, command leasing, completion, status, and
logs. Typed fields, bounded messages, stable field numbers, and generated code
are safer than ad hoc JSON between long-lived agents.

**When it is useful:** Internal service protocols, agent/controller systems,
streaming APIs, and systems whose clients and servers may upgrade separately.

**How MiniCloud uses it:** `proto/controller_worker.proto` defines protocol
version, identities, resources, workload specifications, ensure/stop commands,
leases, status receipts, endpoint status, and streamed logs. CMake runs
`protoc` and the gRPC C++ plugin. The local channel uses a cluster token on the
private Compose network; it does not provide mTLS and must not be treated as a
remote production protocol.

### PostgreSQL and libpqxx

**Definition:** PostgreSQL is a transactional relational database. libpqxx is a
C++ client library for PostgreSQL.

**Why here:** Desired state, ownership, revisions, outbox messages, and events
must survive controller and machine restarts. Transactions allow related state
and commands to commit atomically. Constraints reject invalid state close to
the data.

**When it is useful:** Correctness-critical state, relational queries,
transactional workflows, uniqueness/idempotency constraints, and audit history.

**How MiniCloud uses it:** The repository uses libpqxx read and write
transactions, parameterized SQL, row locks, `FOR UPDATE SKIP LOCKED` command
claiming, unique dedupe keys, command leases, explicit migrations, a controller
advisory lock, and a monotonic epoch sequence. PostgreSQL—not Valkey—is the
authority used to make scheduling and ownership decisions.

### Valkey and hiredis

**Definition:** Valkey is a BSD-licensed, Redis-protocol in-memory data store.
hiredis is the C client used by the C++ Valkey adapter.

**Why here:** Ready endpoints and short log tails need low-latency access but can
be reconstructed or allowed to expire. Keeping them outside durable relational
truth makes the durability boundary explicit.

**When it is useful:** Caches, TTL-based discovery, transient coordination,
bounded recent data, and fast lookup where loss is acceptable by design.

**How MiniCloud uses it:** Sorted sets hold endpoints with expiration scores;
lists hold log tails; a small Lua operation atomically deduplicates and appends
a log batch. hiredis sends Redis-protocol commands from the controller and
gateway. Valkey persistence is disabled in the local Compose configuration on
purpose.

### Prometheus

**Definition:** Prometheus is a pull-based metrics collector and time-series
query system using its PromQL query language.

**Why here:** Operators need aggregate rates, counts, gauges, and trends that
are inefficient to derive from individual log entries.

**When it is useful:** Service health, alert inputs, capacity analysis, error
rates, latency/retry behavior, and operational dashboards.

**How MiniCloud uses it:** C++ components expose text-format `/metrics`
endpoints. A checked-in scrape configuration discovers them by Compose service
name. The Prometheus UI is available locally for queries such as
`minicloud_controller_reconcile_total`, `minicloud_runtime_commands_total`, and
`minicloud_gateway_requests_total`.

### React, Vite, and TypeScript

**Definition:** React builds the component-based UI; TypeScript adds static
types to JavaScript; Vite supplies the frontend development server and
production build pipeline.

**Why here:** The operations console needs an interactive, typed view of
related service, node, allocation, event, and log data. Vite keeps local UI
iteration fast while producing static production assets.

**When it is useful:** Operator consoles, administration interfaces, typed API
clients, and applications with frequently changing state.

**How MiniCloud uses it:** `dashboard/src/types.ts` defines the snapshot shape,
`api.ts` contains the HTTP client, and `App.tsx` renders the console and
mutations. The production Docker build runs TypeScript and Vite, then copies the
result into Nginx. The optional Vite dev server proxies `/api` to the local
controller and reads the token from `deploy/.env`.

### Nginx

**Definition:** Nginx is a web server and reverse proxy.

**Why here:** A browser needs static frontend assets and a same-origin API path.
The controller API also requires a bearer token that should not be compiled into
client-side code.

**When it is useful:** Static asset delivery, reverse proxying, same-origin API
routing, and adding controlled server-side request headers.

**How MiniCloud uses it:** The dashboard image serves Vite output on container
port `8080`, exposes `/healthz`, routes frontend paths to `index.html`, and
proxies `/api/` to the controller with `MINICLOUD_API_TOKEN` inserted as an
authorization header. Host port `3000` maps to this Nginx server.

### CMake and vcpkg

**Definition:** CMake describes the native C++ build graph. vcpkg is a
manifest-driven C/C++ dependency manager.

**Why here:** The project has generated protobuf sources, several libraries,
four executables, cross-platform warning settings, installation rules, and two
test executables. A declarative build graph keeps those relationships portable.

**When it is useful:** Multi-target C++ projects, generated sources, native
tests, and builds that must work with MSVC, Clang, and GCC.

**How MiniCloud uses it:** `CMakeLists.txt` finds dependencies, generates gRPC
sources, builds `minicloud-controller`, `minicloud-worker`,
`minicloud-gateway`, `minicloudctl`, and tests. `CMakePresets.json` supplies
development, release, and Windows/MSVC presets. `vcpkg.json` lists the native
dependencies but does not pin a registry baseline. CMake requires libpqxx 7.9.x
and rejects libpqxx 8, so the latest vcpkg ports are not assumed compatible.
Docker builds use Ubuntu packages for most dependencies and build the pinned,
checksum-verified libpqxx release as C++20 so its ABI matches the application.
vcpkg is useful for an optional native developer build only when its selected
checkout resolves a compatible libpqxx version.

### GitHub Actions and Dependabot

**Definition:** GitHub Actions runs automated workflows in temporary runners.
Dependabot proposes dependency-update pull requests for configured ecosystems.

**Why here:** Cross-platform claims and the complete container path need to be
rechecked from clean machines. Dependency pins also need regular, reviewable
updates.

**When it is useful:** Continuous integration, clean builds, regression checks,
release gates, and dependency maintenance.

**How MiniCloud uses it:** CI checks repository hygiene, compiles the portable
core with GCC, Clang, and MSVC, builds the dashboard, validates both Compose
models, builds the C++ image, starts the full stack, deploys the echo workload,
and calls it through the gateway. Dependabot checks GitHub Actions, npm, and the
Dockerfiles monthly. CI runners are ephemeral verification environments, not an
always-on MiniCloud deployment.

## 6. Core platform behavior

### Scheduling

An allocation can use a node only if all hard conditions pass:

1. the node is ready/schedulable;
2. every required label exactly matches;
3. requested CPU fits after existing reservations; and
4. requested memory fits after existing reservations.

Eligible nodes are ranked in this order:

1. fewest conflicts with the service's soft anti-affinity group;
2. lowest projected dominant utilization—the larger of CPU or memory usage;
3. lowest projected total CPU-plus-memory utilization; and
4. stable lexical node identity/name as a deterministic tie-breaker.

Utilization uses integer parts per million, avoiding compiler-specific
floating-point differences. Soft anti-affinity is a preference, not a hard
rule: replicas may share a node when capacity or labels require it. MiniCloud
does not overcommit the capacity workers declare.

In v0.1 the requested `cpuMillis` and `memoryMb` serve both as scheduler
reservations and as Docker limits. CPU is translated to Docker NanoCPUs and
memory to bytes. This prevents the scheduler from deliberately placing more
declared work than a node can hold, but it does not manufacture physical
capacity: the Docker VM and host must still have enough real resources.

If no worker fits, the allocation remains pending with
`no ready worker satisfies resources and placement` rather than violating a
constraint.

### Reconciliation

The controller normally reconciles every second. For each service it:

- creates missing replica indexes below the desired count;
- reactivates previously scaled-down indexes when scaling back up;
- marks indexes above the desired count stopped and emits stop commands;
- repairs unexpected unhealthy, stopped, lost, or old-generation allocations;
- leaves an allocation failed after its restart budget is exhausted until an
  operator requests a new service generation;
- schedules unassigned running allocations; and
- writes allocation state, commands, and events transactionally.

Reconciliation is safe to repeat. Stable replica indexes, revisions, unique
command dedupe keys, and worker idempotency prevent a retry from intentionally
creating a second logical replica.

### Deploy flow

```text
operator -> REST: POST service definition
controller -> PostgreSQL: service + service.created event
reconciler -> PostgreSQL: allocation + selected worker + ensure command
worker -> gRPC: lease ensure command
worker -> Docker: validate, create, start, inspect
worker -> gRPC: complete command and report status
controller -> PostgreSQL: observed allocation state
controller -> Valkey: publish endpoint only when ready
gateway -> Valkey: resolve service
gateway -> workload: forward request
```

### Scale flow

Scaling changes only `desired_replicas`; it does not increment service
generation or replace replicas that remain within the desired index range.

- Scale up creates missing indexes. A previously scaled-down index is
  reactivated only after its earlier stop completed and cleared the old worker
  assignment.
- Scale down increments affected allocation revisions, removes discovery when
  no longer ready, sends fenced stop commands, and retains the allocation/event
  history.
- Scaling to zero stops all replicas but retains the service definition.

If scale-down and scale-up race while a worker is unavailable, MiniCloud keeps
the current stop command and waits for its acknowledgement before reactivating
that replica. v0.1 deliberately prefers temporary unavailability to risking two
containers with the same logical replica identity and external side effects.

### Restart flow

Restart increments service generation. Reconciliation gives every retained
allocation a new revision and sends a fresh ensure command. The worker removes
the owned older revision and creates the replacement.

This is a replace-all request, not a rolling deployment. v0.1 has no
`maxSurge`, `maxUnavailable`, or ordered readiness policy, so a restart can make
all replicas temporarily unavailable.

### Delete flow

`DELETE /api/v1/services/{name}` requests an asynchronous drain/scale-to-zero
operation in v0.1. Available workers stop their workload replicas, and the
service record and events remain for inspection; this is not a hard database
purge. If an assigned remote worker is unavailable, its container may continue
until that worker returns or an operator cleans the remote daemon. The safe
local stop scripts also remove owned containers on the supported shared daemon.

### Health and restart behavior

The controller sends a Docker HTTP health definition with a three-second
interval, two-second timeout, and three failed attempts. The worker marks only a
healthy container ready, so an unhealthy container is removed from discovery.

The worker runtime supervises missing, exited, unhealthy, and startup-timeout
states. Its current default restart policy is:

- initial delay: 250 ms;
- multiplier: 2;
- maximum delay: 10 seconds;
- maximum automatic restarts: 5;
- healthy period before restart count reset: 30 seconds; and
- startup timeout: 60 seconds.

The policy is bounded to prevent a tight infinite crash loop. When the limit is
exhausted, the allocation remains failed; an operator can use the restart
operation to increment the service generation and intentionally grant a fresh
allocation revision/restart budget. Runtime state and restart counts are
reported to the controller and dashboard. The included echo service has a
`/crash` route for this drill.

### Worker heartbeat and loss

Workers normally heartbeat every three seconds. The controller's default
heartbeat timeout is 12 seconds. When a ready node expires, the controller:

- marks it not ready;
- marks superseded or non-drain active commands dead;
- preserves an exact current stop command for an allocation already draining;
- marks only still-desired running allocations lost and unassigns them;
- increments allocation revisions; and
- schedules replacements on other eligible ready workers.

Stale reports from the old assignment fail the current node, instance, epoch,
generation, or revision checks. In the supported shared-daemon topology, a
higher-revision worker can safely take over a MiniCloud-owned older container.
For a draining allocation, the old assignment and stop tombstone remain visible
until that worker returns and acknowledges removal. A rapid scale-up waits for
that acknowledgement. A separate daemon that never returns still requires
operator cleanup because v0.1 has no remote orphan collector or host-fencing
lease.

### Controller and storage failure behavior

- **Controller restart:** PostgreSQL preserves services, nodes, allocations,
  commands, and events. The new process acquires a higher controller epoch.
  Workers re-register, and expired command leases can be delivered again;
  Docker ownership labels and revisions keep those retries safe.
- **Valkey restart:** desired state is unaffected. The gateway may briefly
  return `503`, and recent logs may disappear. Periodic ready-status reports
  rebuild discovery.
- **PostgreSQL interruption:** new desired-state operations and reconciliation
  stop because the durable authority is unavailable. Existing containers keep
  running, but routes eventually expire if status publication cannot continue
  through the controller. Restore PostgreSQL before changing desired state.
- **Whole-stack stop:** the safe stop scripts remove ephemeral workload
  containers but preserve PostgreSQL and Prometheus volumes. On the next start,
  worker registration and reconciliation recreate desired workloads.

This behavior demonstrates why “the process is still running” and “the control
plane can still guarantee desired state” are different availability questions.

### Command delivery and idempotency

Commands are delivered at least once, not exactly once. A worker can lease a
command for 15 seconds; failure before completion allows a later redelivery.
The schema permits up to eight delivery attempts and applies bounded retry
delay before a command becomes dead.

Safety comes from combining:

- a unique database dedupe key;
- command ID and lease token;
- an in-process bounded receipt cache;
- deterministic managed-container names;
- Docker ownership labels;
- controller and worker epochs;
- service generation and allocation revision; and
- an immutable specification fingerprint.

After a worker restart, its in-memory receipt cache is gone, so the runtime
reconstructs safety from Docker labels and current revisions instead of
assuming memory survived.

### Epochs, generations, and revisions

These values solve different stale-action problems:

| Fence | Changes when | Prevents |
|---|---|---|
| Controller epoch | A controller starts and acquires ownership. | Commands or reports from a superseded controller lifetime. |
| Worker instance ID | A worker process starts. | A prior process instance impersonating the current one. |
| Worker epoch | The registered process for a node name changes. | Heartbeats/completions from an older agent instance. |
| Service generation | The operator requests a service restart. | An old service version being treated as current. |
| Allocation revision | An allocation is replaced, reassigned, reactivated, or stopped. | Older container actions or reports winning after a newer decision. |
| Command lease token | A command delivery is claimed. | A late completion acknowledging a newer lease. |
| Spec fingerprint | The effective Docker specification changes. | Reusing an identity for different container content. |

No single “idempotent” flag can replace these separate fences.

### Service discovery

Service discovery here is not DNS. A ready worker reports a container name and
port. After validating current ownership, the controller stores that endpoint
under the service name in Valkey with an expiration. The gateway asks Valkey for
unexpired members on every request and load-balances them.

If an allocation is no longer ready, its endpoint is removed. If a worker
silently disappears, the TTL ages the endpoint out. Restarting Valkey removes
all routes temporarily, but periodic worker reports rebuild them.

### Logging

Workers read bounded recent Docker stdout/stderr, assign ordered sequence
numbers, and stream batches of at most 128 records over gRPC. The controller
validates current allocation ownership, epoch, generation, revision, ordering,
stream type, and line size before using an atomic Valkey append/dedupe operation.

Logs are best effort and never block command or health processing. They are
recent debugging tails, not durable archival logs; gaps are possible during
worker/controller interruption or Valkey loss.

### Security model

The supported trust boundary is one trusted operator, one trusted machine, and
trusted images.

Implemented protections include:

- every controller route except `GET /health` requires a random API bearer
  token;
- worker gRPC calls require a separate cluster token;
- published ports bind to `127.0.0.1`;
- PostgreSQL and Valkey have no host-published ports;
- input names, JSON shapes, sizes, resources, ports, health paths, and batches
  are bounded and validated;
- the public service definition cannot submit arbitrary Docker JSON, mounts,
  devices, host networking, or privileged mode;
- managed containers drop capabilities and use `no-new-privileges`;
- stop/remove verifies ownership, worker identity, revision, and fingerprint;
- unsafe remote Docker TCP is rejected by default; and
- generated credentials are ignored by Git and release archives.

Important boundary: access to the Docker socket is effectively host-level
power. Only workers receive it, but a compromised worker remains a serious host
compromise. Never expose unauthenticated Docker TCP port `2375`, never expose the
default stack to the internet, and never put secrets in a service's
`environment` object. Secret-reference messages exist in the protobuf for
future evolution, but v0.1 rejects them.

## 7. Repository map

```text
minicloud-platform/
|-- cpp/
|   |-- cli/             minicloudctl commands
|   |-- common/          environment parsing, HTTP server, metrics
|   |-- controller/      REST, gRPC, PostgreSQL repository, Valkey adapter
|   |-- core/            portable domain, scheduler, reconciler
|   |-- gateway/         discovery-backed HTTP reverse proxy
|   |-- include/         public C++ headers
|   |-- runtime/         Docker client and worker runtime state machine
|   `-- worker/          worker registration/control/reporting loop
|-- dashboard/           React, TypeScript, Vite, Nginx, frontend Dockerfile
|-- database/migrations/ PostgreSQL schema versions 1 through 4
|-- deploy/              Compose, Desktop override, bootstrap, Prometheus
|-- docs/                architecture, API, operations, security, guides, ADRs
|   |-- adr/             architecture decision records
|   `-- milestones/      implementation and learning record per milestone
|-- examples/echo-service/ real test workload and service definition
|-- information-pack/    documentation-only archive index
|-- proto/               controller/worker protobuf contract
|-- scripts/             validation, E2E, safe stop, packaging, Windows scripts
|-- tests/cpp/           scheduler/reconciler and runtime tests
|-- .github/             Actions CI and Dependabot configuration
|-- .vscode/             repeatable editor tasks and extension suggestions
|-- CMakeLists.txt       native build graph
|-- CMakePresets.json    development/release/Windows presets
|-- Dockerfile           C++ build/test and component runtime images
|-- Makefile             macOS/Linux convenience commands
`-- vcpkg.json           native C++ dependency manifest
```

Suggested code-reading order:

1. `examples/echo-service/service.json`;
2. `cpp/cli/main.cpp` and `cpp/controller/api.cpp`;
3. `database/migrations/001_initial.sql` and `cpp/controller/repository.cpp`;
4. `cpp/core/scheduler.cpp` and `cpp/core/reconciler.cpp`;
5. `proto/controller_worker.proto` and `cpp/controller/grpc_service.cpp`;
6. `cpp/worker/main.cpp`;
7. `cpp/runtime/worker_runtime.cpp` and `docker_client.cpp`;
8. `cpp/controller/valkey_store.cpp` and `cpp/gateway/proxy.cpp`; and
9. `dashboard/src/api.ts`, `types.ts`, and `App.tsx`.

## 8. Prerequisites

### Required for the normal containerized path

- Docker Desktop on Windows/macOS, or Docker Engine on Linux;
- Docker Compose v2 (`docker compose`, not legacy `docker-compose`);
- Linux containers mode;
- Git to obtain and update the source;
- approximately 6 GiB available to Docker for a smooth first build; and
- internet access for the first download of public images and build packages.

macOS/Linux helpers also use `make` and host `curl`. VS Code is optional. The
normal Docker path does not require a host installation of CMake, a C++
compiler, Node, PostgreSQL, Valkey, gRPC, or Prometheus.

Confirm Docker before continuing:

```text
docker version
docker compose version
```

On Windows also run:

```powershell
docker info --format '{{.OSType}}'
```

It must print `linux`.

### Optional native-development prerequisites

For a host-native C++ build, install CMake 3.24+, Ninja where the preset uses
it, a C++20 compiler, and the packages in `vcpkg.json`. For native dashboard
development, install Node.js and npm. These are optional because Docker builds
and tests the application inside the image. If vcpkg supplies the native
packages, use a checkout whose `libpqxx` port resolves to 7.9.x; the manifest is
not a promise that the latest vcpkg ports are compatible.

## 9. Exact quick start

Run all commands from the repository root. The credential initializer creates
`deploy/.env` once and refuses to overwrite it. Preserve that file while the
PostgreSQL volume exists because its database password belongs to the initialized
volume.

### Windows 11 with Docker Desktop

Use PowerShell and Docker Desktop with WSL 2 and Linux containers:

```powershell
Set-Location C:\path\to\minicloud-platform
.\deploy\powershell\Initialize-MiniCloud.ps1
.\scripts\windows\Start-MiniCloud.ps1
.\scripts\windows\Deploy-Echo.ps1
```

If reviewed local scripts are blocked, change policy only for the current
PowerShell process:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
```

The start script automatically adds the Docker Desktop socket override.

### macOS with Docker Desktop

```bash
cd /path/to/minicloud-platform
make init
make up-desktop
make demo
```

`make demo` runs `scripts/e2e.sh`, which automatically uses the Desktop
override on macOS.

### Linux with Docker Engine

```bash
cd /path/to/minicloud-platform
make init
make up
make demo
```

### Expected proof

The demo builds `minicloud/echo-service:dev`, submits two replicas, waits for
health and service discovery, and calls the service through the gateway. The
POSIX E2E script additionally verifies that a terminal failure stays failed
until an explicit restart and that a delayed drain cannot create a duplicate
runtime; it then prints a cluster snapshot. Both proof paths end with:

```text
MiniCloud end-to-end verification passed.
```

The first build can be slow because it downloads and compiles dependencies.
Later runs use Docker's cache.

## 10. How to use MiniCloud

### Open the tools

| Tool | Local address | Purpose |
|---|---|---|
| Dashboard | <http://127.0.0.1:3000> | Visual deploy, scale, restart, status, events, and logs. |
| Gateway | <http://127.0.0.1:8080> | Route requests to healthy service replicas. |
| Controller health/API | <http://127.0.0.1:8090> | Health plus authenticated REST API. |
| Prometheus | <http://127.0.0.1:9090> | Metrics query and target status. |

The host-mapped gRPC port is `127.0.0.1:50051`, but normal operators should use
the REST API or CLI. Internal metrics ports are scraped by Prometheus and are
not published to the host.

### Check platform containers

macOS/Linux:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml ps
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=100
```

Windows PowerShell uses the same Docker commands. Add
`-f deploy/compose.desktop.yaml` when recreating or starting worker services on
Docker Desktop.

### Define a service

Create a JSON file under `examples/`, for example
`examples/orders-service.json`:

```json
{
  "name": "orders-api",
  "image": "your-registry/orders-api@sha256:immutable-digest",
  "replicas": 2,
  "cpuMillis": 300,
  "memoryMb": 256,
  "containerPort": 8080,
  "healthPath": "/health",
  "environment": {
    "APP_MODE": "demo"
  },
  "placement": {
    "requiredLabels": {
      "os": "linux-containers"
    },
    "antiAffinityGroup": "orders"
  }
}
```

Rules that matter:

- service names are 1–63 character lowercase DNS labels: they start with a
  letter, contain only letters, digits, or `-`, and end with a letter or digit;
- replicas are from 0 through 50;
- CPU is expressed in millicores and memory in MiB;
- the application must listen on `containerPort` and answer `healthPath`;
- the worker adds `PORT` and `MINICLOUD_ALLOCATION_ID`, so do not override them;
- environment values are stored durably and visible to authorized operators,
  so they must not contain secrets;
- required labels are exact hard constraints; and
- an immutable image digest is preferred for reproducibility.

MiniCloud does not implicitly pull images or accept registry credentials. Build
or pull the image into the Docker engine first. If the registry is private,
authenticate Docker itself outside MiniCloud.

### Run CLI commands

The following commands work in PowerShell, bash, and zsh because the CLI runs
inside Compose. Files passed to `deploy` must be visible under the mounted
`examples/` directory.

Status:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli status
```

Deploy:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli \
  deploy /workspace/examples/orders-service.json
```

Scale to three replicas:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli \
  scale orders-api 3
```

Restart all retained replicas:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli \
  restart orders-api
```

Fetch the last 200 lines for an allocation ID shown by `status` or the
dashboard:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli \
  logs ALLOCATION_UUID 200
```

Drain/delete the service:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli \
  delete orders-api
```

On PowerShell, place each command on one line or replace the bash `\` line
continuation with PowerShell's backtick.

### Use the REST API directly

On macOS/Linux, load the token without printing it:

```bash
MINICLOUD_API_TOKEN=$(awk -F= '/^MINICLOUD_API_TOKEN=/{print $2}' deploy/.env)
```

Get a snapshot:

```bash
curl -fsS http://127.0.0.1:8090/api/v1/snapshot \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN"
```

Deploy a host file:

```bash
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN" \
  -H 'content-type: application/json' \
  --data-binary @examples/orders-service.json
```

Scale:

```bash
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services/orders-api/scale \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN" \
  -H 'content-type: application/json' \
  --data '{"replicas":3}'
```

Restart:

```bash
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services/orders-api/restart \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN"
```

Logs:

```bash
curl -fsS 'http://127.0.0.1:8090/api/v1/allocations/ALLOCATION_UUID/logs?tail=200' \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN"
```

Drain/delete:

```bash
curl -fsS -X DELETE http://127.0.0.1:8090/api/v1/services/orders-api \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN"
```

Only `GET /health` is unauthenticated. Full request/response details are in
[API.md](API.md).

### Call a deployed service

The gateway strips `/services/<name>` and forwards the remaining path and
query. For the included echo service:

```bash
curl -fsS 'http://127.0.0.1:8080/services/echo-api/echo?message=hello'
```

Crash one selected replica and observe recovery:

```bash
curl -fsS http://127.0.0.1:8080/services/echo-api/crash
docker compose --env-file deploy/.env -f deploy/compose.yaml \
  logs -f worker-a worker-b
```

### Stop safely

macOS/Linux:

```bash
make down
```

Windows:

```powershell
.\scripts\windows\Stop-MiniCloud.ps1
```

The scripts stop platform services, select only MiniCloud-managed workload
containers on `minicloud-workloads`, remove them, and bring Compose down. The
PostgreSQL and Prometheus named volumes are preserved. Desired state therefore
returns when the platform starts again.

Do not add `--volumes` unless the explicit goal is to destroy durable local
state.

### Create offline release packages

macOS/Linux:

```bash
make package
```

Windows:

```powershell
.\scripts\windows\Package-Release.ps1
```

Both commands require Git and archive the exact committed `HEAD`, so commit the
intended release inputs first. They write a complete source ZIP and a
documentation-only information pack under ignored `dist/`; untracked or ignored
credentials, Git metadata, build outputs, dependency caches, and logs stay out.
A version tag runs the checked-in release workflow, which publishes both
archives plus `SHA256SUMS` on GitHub.

## 11. Dashboard and endpoint reference

### Dashboard operations

- **Deploy service:** name, image, replicas, CPU, memory, port, and health path.
- **Scale:** `+1` and `-1` buttons change desired replica count.
- **Restart:** increments generation and replaces retained allocations.
- **Logs:** opens the recent Valkey tail for a selected allocation.
- **Observe:** services, convergence, nodes, reservations, labels, allocations,
  endpoints, events, and restarts refresh every three seconds.

For placement rules or environment variables, submit the complete JSON through
the CLI or REST API because the current deploy form exposes only the basic
fields.

### HTTP endpoints

| Component | Endpoint | Authentication | Result |
|---|---|---|---|
| Controller | `GET /health` | None | Controller process health. |
| Controller | `GET /api/v1/snapshot` | API bearer | Overview, services, nodes, allocations, events. |
| Controller | `POST /api/v1/services` | API bearer | Create desired service. |
| Controller | `POST /api/v1/services/{name}/scale` | API bearer | Change replicas without generation change. |
| Controller | `POST /api/v1/services/{name}/restart` | API bearer | Increment generation and replace retained replicas. |
| Controller | `DELETE /api/v1/services/{name}` | API bearer | Drain to zero while retaining record/history. |
| Controller | `GET /api/v1/allocations/{id}/logs?tail=N` | API bearer | Recent ephemeral log lines. |
| Gateway | `GET /health` | None, loopback only | Gateway process health. |
| Gateway | `ANY /services/{name}/...` | None, loopback only | Forward to a ready workload endpoint. |
| Dashboard | `GET /healthz` | None, loopback only | Nginx/dashboard container health. |
| Prometheus | `GET /-/healthy` | None, loopback only | Prometheus health. |

## 12. Testing and verification

Different tests prove different layers; one successful unit test does not prove
the full Docker path.

### Fast portable core tests

On macOS/Linux with a C++20 compiler:

```bash
make core-test
```

This directly compiles the dependency-free scheduler/reconciler suite with
strict warnings.

### Repository contract

```bash
python3 scripts/check_repository.py
```

This validates JSON, local Markdown links, personal absolute-path patterns, and
common credential patterns while excluding generated/cache directories.

### Native C++ build and tests

With all native dependencies installed:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On Windows with `VCPKG_ROOT` pointing to a vcpkg checkout that resolves
libpqxx 7.9.x, and with Visual Studio 2022 C++ tools installed:

```powershell
cmake --preset windows-dev
cmake --build --preset windows-dev
ctest --preset windows-dev
```

The test binaries cover portable scheduler/reconciler invariants and Docker
runtime payload, ownership, revision, replay, health, and restart behavior.

### Combined developer check

On a machine with native C++ dependencies, Ninja, and Docker Compose:

```bash
./scripts/check.sh
```

It configures, builds, runs CTest, and validates the Compose model using the
checked-in example environment.

### Dashboard production build

With Node.js/npm installed:

```bash
cd dashboard
npm install --no-audit --no-fund
npm run build
```

### Full local end-to-end proof

After Docker is running:

```bash
./scripts/e2e.sh
```

Windows (start the platform before running the workload proof):

```powershell
.\scripts\windows\Start-MiniCloud.ps1
.\scripts\windows\Deploy-Echo.ps1
```

This is the strongest local check: it builds the platform, builds a real
workload, submits desired state, waits for scheduling and health, resolves
service discovery, and sends traffic through the gateway.

### GitHub CI proof

The checked-in workflow has these jobs:

- repository and documentation contract;
- portable C++ core on Ubuntu/GCC, macOS/Clang, and Windows/MSVC;
- React dashboard production build;
- Linux and Docker Desktop Compose validation; and
- full Ubuntu Docker image and end-to-end test, including terminal-failure and
  delayed-drain regressions.

The Docker job prints container diagnostics after failure and deletes ephemeral
runner volumes during cleanup. A green workflow proves that revision passed on
the tested runner images; it does not make the project an always-running cloud
service.

## 13. Troubleshooting

### Docker is missing or unavailable

- `docker: command not found`: install Docker Desktop/Engine or fix `PATH`.
- Cannot connect to daemon: start Docker and wait until `docker version` shows
  both client and server.
- Windows reports `OSType=windows`: switch Docker Desktop to Linux containers.
- Windows named-pipe access denied: add the user to `docker-users`, then sign
  out and back in.

### Credentials or database startup fail

- Missing `deploy/.env`: run the initializer once.
- Initializer refuses overwrite: this is intentional; keep existing
  credentials for the existing PostgreSQL volume.
- API returns `401`: use the current `MINICLOUD_API_TOKEN` from `deploy/.env`.
- Existing prerelease database reports schema below 4: do not recreate secrets.
  Follow the backup and migration procedure in [OPERATIONS.md](OPERATIONS.md#upgrade-an-existing-prerelease-database).

### Workers cannot control Docker

- On macOS/Windows Docker Desktop, start with the Desktop override or the
  checked-in start script.
- On ordinary Linux Engine, use only the base Compose file.
- Never solve socket trouble by enabling unauthenticated TCP `2375`.
- An `exec format error` usually means the image architecture does not match the
  Docker VM; build or pull `linux/amd64` or `linux/arm64` as appropriate.

### Allocation remains pending

Compare the service's CPU, memory, and required labels from the CLI/API snapshot
with ready workers in the dashboard. The current service table does not render
placement rules. The scheduler will not overcommit declared capacity. Default
workers always add `os=linux-containers` and expose their configured `zone` and
`arch` labels.

### Container cannot be created

- Build or pull the image into the same Docker engine first.
- Authenticate Docker outside MiniCloud for a private registry.
- Verify the image platform matches the Docker host.
- Verify the application listens on the declared container port.
- Inspect `worker-a` and `worker-b` logs for validation or Docker errors.

### Container never becomes ready

- Confirm the health path returns success on `127.0.0.1:<containerPort>` inside
  the container.
- Confirm the image contains `wget`.
- Confirm startup completes within 60 seconds.
- Inspect the allocation state/restart count and worker logs.

### Gateway errors

- `503`: discovery has no unexpired ready endpoint. Inspect health, worker
  status reports, and Valkey recovery.
- `502`: endpoints existed but upstream transport failed. Inspect the workload
  process and `minicloud-workloads` network.
- Wrong path: calls must start with `/services/<service-name>/`.

### Dashboard is stale or unavailable

- If the API snapshot is also wrong, debug controller/PostgreSQL first.
- If the authenticated snapshot is correct, inspect dashboard/Nginx logs and
  browser network errors.
- A Vite development server needs the controller stack and a readable
  `deploy/.env` so its proxy can add authentication.

### Port conflicts

Host ports `3000`, `50051`, `8080`, `8090`, and `9090` must be free. Change only
the host side of mappings in a personal, uncommitted Compose override; keep
container ports unchanged unless updating all dependent configuration.

### Useful diagnostic commands

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml ps --all
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=200 controller
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=200 worker-a worker-b
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=200 gateway
curl -fsS http://127.0.0.1:8090/health
curl -fsS http://127.0.0.1:8080/health
curl -fsS http://127.0.0.1:9090/-/healthy
```

## 14. Cost and online-hosting truth

### What costs nothing in the default path

The application code and normal supporting stack use free/open-source
components. No paid API, hosted database, cloud account, domain, managed
metrics service, or registry subscription is required. Docker Personal is free
for eligible personal/educational use under Docker's terms.

There are still ordinary operator costs: local disk, CPU, memory, electricity,
and internet downloads.

### What “online” means for this repository

The source, complete guide, and tagged source/information archives can be public
on GitHub. GitHub Actions can also start the entire stack temporarily to verify
it. For a public repository, standard hosted CI runners provide the project's
intended zero-cost online proof.

That runner is destroyed when the workflow ends. It is not a permanent URL for
the dashboard or gateway.

GitHub Pages cannot host the actual product because Pages serves static files;
MiniCloud requires a Docker daemon, C++ controller and workers, PostgreSQL,
Valkey, live workload containers, and Prometheus. Publishing only the dashboard
would omit the orchestrator and would be misleading.

The checked-in public-observer overlay is a different boundary: it publishes a
sanitized read-only view backed by the actual running controller. A free
Cloudflare Quick Tunnel can provide a temporary random HTTPS URL while the host
remains online. It does not expose the administrative tool or workloads and has
no uptime guarantee. Exact commands and cost/security limits are in
[PUBLIC_OBSERVER.md](PUBLIC_OBSERVER.md).

### Running the actual product online

The real platform must run on a Windows/macOS/Linux machine with Docker. A
spare personal Linux machine can be a zero-new-subscription lab, but it should
be reached through a private network or VPN and hardened first. An always-on
general cloud VM can run it, but that compute commonly costs money. Codespaces
has personal quotas and becomes metered after those quotas.

Before an **interactive** internet-facing deployment, the project needs at
least authenticated users/roles, mTLS worker identity, authenticated TLS at the
edge, private Docker endpoints, firewalling, backup/restore, external secret
management, controller high availability, signed/scanned image admission,
quotas, rate limiting, and a real incident runbook. The default Compose ports
must not simply be changed to `0.0.0.0`; the read-only observer is not a
substitute for those controls.

MiniCloud never creates a paid resource automatically. Provider, security,
shutdown policy, and spending limit require an explicit operator decision.

## 15. Learning roadmap using the actual project

### Stage 1: establish the end-to-end mental model

1. Run the echo proof.
2. Open the dashboard and locate service, allocation, worker, endpoint, event,
   and restart data.
3. Trace one request from `service.json` through REST, PostgreSQL, gRPC, Docker,
   Valkey, and the gateway.

Outcome: explain the difference between control plane, data plane, desired
state, observed state, and reconciliation.

### Stage 2: understand deterministic policy

1. Read `cpp/core/domain.cpp`, `scheduler.cpp`, and `reconciler.cpp` with their
   tests.
2. Reverse input node order and confirm placement does not change.
3. Submit one matching and one impossible required-label constraint.

Outcome: explain hard filters, soft preferences, stable tie-breaking, and why a
pending allocation is safer than violating a constraint.

### Stage 3: understand durable delivery

1. Read migrations, repository transactions, command leasing, and gRPC
   completion.
2. Stop a worker after work is leased, then let the lease expire.
3. Observe that redelivery does not create a second logical container.

Outcome: explain the transactional outbox, at-least-once delivery,
idempotency, and retry backoff.

### Stage 4: understand fencing

1. Map controller epoch, worker epoch, generation, revision, lease token, and
   fingerprint to the stale action each blocks.
2. Read the foreign-container and stale-revision runtime tests.
3. Restart a worker and follow replayed desired state.

Outcome: explain why distributed safety needs several independent identities.

### Stage 5: understand runtime and failure recovery

1. Inspect Docker create JSON and compare it with `docker inspect`.
2. Trigger `/crash` and watch health, discovery removal, backoff, restart, and
   republishing.
3. Stop a worker and observe the 12-second liveness boundary and reassignment.

Outcome: distinguish process health, readiness, worker liveness, and desired
state repair.

### Stage 6: understand ephemeral versus durable data

1. Restart Valkey and observe route/log loss followed by endpoint rebuild.
2. Restart the controller and observe PostgreSQL state survive.
3. Stop the whole stack safely and start it again.

Outcome: explain why data classification comes before choosing a database or
cache.

### Stage 7: understand observability and operations

1. Correlate a deploy response, event, allocation, worker log, endpoint,
   gateway count, and Prometheus query.
2. Use the troubleshooting fault tree instead of opening all logs at once.
3. Run the same checks through VS Code tasks and CI.

Outcome: explain metrics, events, and logs as complementary signals.

The milestone records in [milestones/](milestones/) and the exercises in
[BUILD_AND_LEARN.md](BUILD_AND_LEARN.md) provide deeper checkpoints.

## 16. High-value extension ideas

Implement extensions one at a time, starting with failing tests and an
architecture decision record.

1. **Rolling updates:** add `maxSurge`, `maxUnavailable`, ordered readiness,
   rollback, and progress deadlines.
2. **Autoscaling:** derive replica recommendations from Prometheus with min/max,
   cooldown, and stabilization windows.
3. **mTLS worker identity:** issue, rotate, and revoke certificates; bind node
   identity to certificate identity.
4. **Secret references:** implement a local encrypted provider so desired state
   stores references rather than values.
5. **Persistent volumes:** model claims, attachment ownership, topology, and
   safe rescheduling.
6. **Admission policy:** require trusted registries, immutable digests,
   vulnerability policy, and image signatures.
7. **Controller high availability:** add leader election/lease renewal,
   standby behavior, and failover tests.
8. **Remote orphan collection:** make separate-daemon failover detect and clean
   old-host containers safely.
9. **Advanced scheduling:** taints/tolerations, topology spread, priorities,
   preemption, disk/GPU resources, and explainable scheduling output.
10. **Durable log pipeline:** ship structured logs to archival storage with
    retention and query controls.
11. **Ingress TLS and policy:** add host-based routing, certificates,
    authentication, quotas, and rate limiting.
12. **Chaos and soak testing:** automate worker loss, controller restart,
    database interruption, Valkey loss, and repeated convergence checks.

These are future projects, not claims about v0.1.

## 17. Current limitations

- One active controller and one PostgreSQL primary; no highly available control
  plane or distributed leader election.
- The supported two-worker setup shares one Docker daemon.
- Separate-host/daemon mode is experimental and lacks remote orphan garbage
  collection if an old agent is unreachable while its containers continue.
- Local gRPC uses a cluster token without mTLS or network confidentiality.
- The gateway provides HTTP only, no public TLS, user authentication, rate
  limiting, or host-based ingress.
- Restart replaces all retained allocations; there is no rolling ordering,
  surge, unavailable budget, or automatic rollback.
- No horizontal autoscaler, job/cron workload, priority, preemption, or quota
  system.
- No volumes, secret provider, GPU scheduling, disk resource, or device model.
- Submitted images must already exist on each selected Docker engine; no
  implicit pull or registry-credential API exists.
- Health checks are HTTP-only in the current REST-to-worker path and require
  `wget` inside the workload image.
- Workload environment is configuration, not secure secret storage.
- Logs are bounded, expiring tails and can have gaps; they are not archival.
- Prometheus is a single local instance with no alert rules or remote storage.
- Service deletion drains to zero and retains the database record.
- Dashboard deployment exposes basic fields only; advanced placement and
  environment settings require JSON through CLI/REST.
- Declared node capacity is logical. Docker Desktop's VM and the physical host
  may have less real capacity.
- Docker socket possession remains host-powerful despite input validation.
- The default configuration is trusted-local and must not be exposed directly
  to the internet.

## 18. Clean-room and contribution boundary

MiniCloud is original, public-facing work based only on public interfaces and
documentation. Do not add employer/customer source, private architecture,
internal hostnames, tickets, logs, credentials, certificates, production data,
or non-public screenshots.

Before publishing a change:

1. run the repository contract check;
2. confirm `deploy/.env` remains ignored and untracked;
3. inspect the Git diff for credentials and personal paths;
4. build/test at the layer affected by the change;
5. run the Docker end-to-end proof for control/runtime changes; and
6. update the relevant architecture, API, operations, and milestone documents.

## 19. Recommended daily workflow

1. Open the repository root in VS Code.
2. Run **MiniCloud: Verify Docker**.
3. Initialize credentials only on the first fresh installation.
4. Run **MiniCloud: Start platform**.
5. Run **MiniCloud: Run full E2E demo**.
6. Make one bounded code change with a corresponding test.
7. Run the smallest relevant test, then the full E2E proof.
8. Use the dashboard, events, component logs, and Prometheus to explain the
   result.
9. Run the repository contract before committing.
10. Stop with **MiniCloud: Stop platform** so volumes are preserved safely.

The checked-in task definitions and editor recommendations are explained in
[VSCODE.md](VSCODE.md). Operational failure drills are in
[OPERATIONS.md](OPERATIONS.md), and the full security boundary is in
[THREAT_MODEL.md](THREAT_MODEL.md).

# MiniCloud architecture

## Product boundary

MiniCloud is a trusted-local container orchestration platform. It demonstrates
the same categories of engineering used in systems such as ECS and Kubernetes,
but it deliberately keeps one small, inspectable implementation.

It is not a container runtime. Docker supplies image, namespace, cgroup, and
filesystem isolation; MiniCloud supplies desired state, scheduling, dispatch,
health/restart policy, discovery, routing, logs, metrics, and operator UX.

## Components

### Controller

The C++ controller exposes two interfaces:

- REST on port 8090 for users and the React console.
- gRPC on port 50051 for authenticated workers.

Its reconciliation loop reads PostgreSQL desired and observed state, detects
missing or stale allocations, chooses a node, and writes allocation changes plus
commands in one transaction. Commands use a unique deduplication key and a
lease, so worker delivery is at least once while side effects remain idempotent.

### PostgreSQL

PostgreSQL is the durable source of truth:

- `services` stores image, replicas, resources, health, placement, and generation.
- `nodes` stores capacity, labels, worker instance/epoch, and heartbeat.
- `allocations` maps a service replica to a node and records observed runtime state.
- `commands` is the transactional outbox with leases and retry state.
- `events` is the bounded-shape audit and troubleshooting stream.
- `schema_migrations` provides explicit schema compatibility.

Migration files run automatically, in filename order, only when PostgreSQL
initializes an empty data directory. Existing prerelease volumes must apply new
migration files explicitly before starting a controller that requires them; the
safe procedure is in [OPERATIONS.md](OPERATIONS.md).

No scheduling decision depends on Valkey surviving a restart.

### Scheduler and reconciler core

`cpp/core` is a pure C++20 library with no platform or third-party dependency.
It uses immutable snapshots and deterministic ordering. Hard filters are:

1. node is schedulable;
2. all required labels match exactly;
3. requested CPU fits after current reservations;
4. requested memory fits after current reservations.

Eligible nodes are ranked by soft anti-affinity conflicts, dominant projected
utilization, total projected utilization, and stable lexical node ID. Integer
parts-per-million scoring avoids floating-point differences across MSVC, Clang,
and GCC.

The reconciler creates deterministic allocation/command identities, carries
expected revisions for optimistic concurrency, and fences older controller and
worker actions.

### Worker

Each C++ worker registers capacity and labels, heartbeats, leases commands, and
translates the protobuf `WorkloadSpec` into a Docker Engine request. It never
passes a command through a host shell.

The worker runtime enforces:

- deterministic container names;
- platform ownership labels;
- immutable spec fingerprints per revision;
- stale revision rejection;
- bounded in-process command receipt replay, with Docker-label reconstruction
  after a worker restart;
- foreign-container refusal before stop/remove;
- CPU, memory, PID, capability, privilege, and network settings;
- bounded startup timeout and exponential restart backoff;
- a maximum restart count.

After that restart budget is exhausted, the allocation remains failed. An
operator must request a service restart, which advances generation and creates
a deliberately fresh revision and restart budget.

The default Compose application starts `worker-a` and `worker-b`. They are logical
nodes sharing one local Docker engine. This lets a laptop exercise placement and
failure behavior without running two VMs. The gRPC protocol and Docker endpoint
abstraction can connect one worker per separate host/engine, but that topology
is experimental in v0.1: an agent loss cannot garbage-collect a container on an
otherwise reachable remote daemon until that node is recovered or an operator
intervenes.

Workers join only the Compose control network. Through their mounted Docker
socket they create workload containers on `minicloud-workloads`; only the
gateway also joins that workload network for request routing.

### Valkey

Valkey is a Redis-protocol, BSD-licensed ephemeral data plane. It stores:

- ready endpoint records with expirations;
- bounded recent log tails.

Workers periodically republish ready endpoints, so discovery reconstructs after
Valkey or controller restart. Expired workers cannot keep a route alive.

### Gateway

The C++ gateway resolves `/services/<name>/...` through Valkey, selects endpoints
round-robin, forwards bounded HTTP bodies, preserves a request ID, applies
timeouts, and only retries idempotent requests. It reports 503 when no ready
endpoint exists and 502 when selected endpoints cannot be reached.

### Dashboard and Prometheus

The React/TypeScript dashboard reads the authenticated snapshot endpoint every
three seconds and issues authenticated mutation requests through its Nginx
same-origin proxy. It shows services, convergence, workers, reservations,
allocations, events, logs, scaling, and restarts.

Prometheus scrapes fixed-cardinality metrics from controller, workers, and
gateway every five seconds. Metrics are operational state, not durable truth.

## Main flows

### Deploy and converge

```text
User -> REST: create service generation 1
REST -> PostgreSQL: commit service + event
Reconciler -> PostgreSQL: create allocation, select node, enqueue ensure command
Worker -> gRPC: lease command
Worker -> Docker: create/start labeled container with limits
Worker -> gRPC: complete command + report status/endpoint
Controller -> PostgreSQL: store observation
Controller -> Valkey: publish expiring endpoint
Gateway -> Valkey: resolve endpoint
Gateway -> container: forward request
```

### Scale

Scaling changes `desired_replicas` without changing the service generation.
Reconciliation creates or reactivates only missing replica indexes. Decreasing
replicas marks excess allocations stopped and enqueues fenced stop commands;
successful completion records stopped state without deleting the audit history.
If the service is scaled back up before that completion, v0.1 waits for the old
worker to acknowledge the stop before reactivating the replica index. This
chooses temporary unavailability over risking two containers performing the
same logical replica's work.

### Restart

A restart request increments the service generation. Reconciliation increments
each retained allocation revision, and the worker compares that desired
revision/fingerprint to the owned container, stops and removes the old revision,
creates the replacement, and rejects replayed older commands.

### Worker loss

After the heartbeat timeout, the controller marks the node not ready and its
still-desired running allocations lost. Reconciliation selects another eligible
node and increments allocation revisions. A current stop command for a replica
already being drained remains durable and assigned to the old node; the same
worker process can claim it after reconnecting. Observations from superseded
assignments no longer match node/revision fences.

## Consistency and delivery guarantees

- Desired-state writes and command creation are PostgreSQL transactions.
- Commands are at-least-once, leased, bounded-retry messages.
- Command IDs and Docker labels make worker side effects idempotent.
- Observations require matching node instance, worker epoch, service generation,
  and allocation revision.
- Discovery and log tails are eventually consistent and disposable.
- The gateway never routes an endpoint after its discovery TTL expires.

## Cross-platform design

- Portable core code uses only C++20 standard facilities.
- HTTP and gRPC use cross-platform libraries.
- Docker transport supports Unix sockets, Windows named pipes, and HTTP/HTTPS
  TCP endpoints. Non-loopback TCP requires explicit opt-in; remote protection
  and authentication must be supplied outside v0.1.
- PowerShell and POSIX bootstrap paths generate the same configuration contract.
- Full-stack Windows runs Linux containers under Docker Desktop/WSL 2.

## Deliberate v0.1 limits

- One controller and one PostgreSQL primary; no distributed leader election.
- Logical laptop workers share a Docker daemon.
- Separate-daemon failover has no remote orphan garbage collector in v0.1.
- Local gRPC is token-authenticated on a private Compose network, not mTLS.
- No volumes, secrets provider, GPU scheduling, ingress TLS, or rolling-update
  surge/unavailable policy yet.
- HTTP health currently generates a direct `wget` command inside the container.
- Logs are recent tails, not an archival log platform.

Each limit is a natural later milestone rather than a hidden claim.

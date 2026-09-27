# Build and learn companion

MiniCloud is the product. This file is the supporting map for extracting
senior-level engineering lessons while building and operating it.

## How to use this guide

For each topic:

1. Read the “why” and identify the invariant.
2. Open the named implementation and test files together.
3. Run the exercise against the real stack.
4. Explain the failure mode in your own words before changing code.

## C++20 control-plane core

**What:** Immutable resource, node, workload, and allocation models plus pure
scheduler/reconciler functions.

**Why:** Pure functions separate policy from I/O, make decisions deterministic,
and let Windows/Linux/macOS prove identical behavior without databases or Docker.

**When:** Use this pattern whenever business decisions must be reproducible,
reviewable, fuzzable, and safe to retry.

**How:** Read `cpp/core/domain.cpp`, `scheduler.cpp`, and `reconciler.cpp`, then
their tests. Change node input order and verify the same node wins.

Hands-on exercise: add a `diskMb` resource, update dominance scoring, and write
the failing tests before implementation.

## Desired versus observed state

**What:** `services` says what should exist; `allocations` says what the platform
currently observes.

**Why:** Imperative “start container” APIs lose intent when a process crashes.
A reconciler can repeatedly compare and repair state.

**When:** Controllers, deployment engines, job systems, device management, and
configuration management.

**How:** Scale `echo-api` to three, stop one managed container manually, and
watch the worker/runtime restore convergence.

## Scheduling and bin packing

**What:** Hard filters reject impossible nodes; stable ranking chooses among
eligible nodes.

**Why:** Mixing filters and preferences causes accidental constraint violation.
Deterministic tie-breaking prevents placement flapping.

**When:** Containers, jobs, shards, build agents, storage placement, or any
finite-capacity assignment problem.

**How:** Give one worker a required label, submit matching and nonmatching
services, and inspect pending allocation evidence.

## Transactional outbox and at-least-once delivery

**What:** The controller stores allocation intent and worker command in the same
PostgreSQL transaction. Workers lease commands and acknowledge outcomes.

**Why:** Writing state and then publishing separately creates a crash window.
At-most-once can silently lose work; at-least-once plus idempotency is usually
the safer contract.

**When:** Payments, provisioning, emails, workflow engines, and integration
events that cross process boundaries.

**How:** Stop a worker after it leases a command. After lease expiry, restart it
and verify redelivery does not create a second container.

## Fencing and idempotency

**What:** Worker epoch, controller epoch, service generation, allocation
revision, command ID, ownership label, and spec fingerprint fence different
classes of stale action.

**Why:** “Retry-safe” is not one flag. A retry, an old process, an old desired
generation, and a name collision are separate threats.

**When:** Distributed locks, storage writers, deployment agents, queues, and
lease-based work.

**How:** Read `WorkerRuntime::apply` and the foreign-container/revision tests.
Try reusing a command ID with different content and see it rejected.

## Docker Engine API and resource isolation

**What:** The worker talks to Docker over HTTP through a Unix socket, a Windows
named pipe, or an explicitly opted-in HTTP/HTTPS TCP endpoint. MiniCloud does
not configure remote client certificates; remote transport protection and
authentication are an external operator responsibility. It creates JSON
specifications with NanoCPUs, memory, PIDs, capabilities, security options,
networks, labels, and health commands.

**Why:** A CLI subprocess is harder to validate, structure, timeout, and test.
The API keeps data as data and avoids shell interpretation.

**When:** Platform integrations where a stable machine API exists.

**How:** Inspect `DockerClient::create_payload` and compare the generated JSON
with `docker inspect` for the echo allocation.

## gRPC and protocol evolution

**What:** Protobuf defines registration, heartbeat, leased commands, completion,
batched observations, endpoints, and client-streamed logs.

**Why:** Workers may upgrade separately from controllers. Field numbers,
bounded batches, explicit epochs, and additive evolution form a compatibility
contract.

**When:** Internal service-to-service protocols with multiple implementations or
long-lived clients.

**How:** Add an optional node attribute with a new field number; do not rename or
reuse an existing number. Regenerate code and preserve old-worker behavior.

## PostgreSQL versus Valkey

**What:** PostgreSQL stores correctness-critical truth; Valkey stores expiring,
reconstructable endpoint and log data.

**Why:** Choosing storage by access pattern is useful only after classifying
durability. Treating a cache as truth creates recovery surprises.

**When:** Any architecture mixing relational state and fast ephemeral data.

**How:** Restart Valkey and observe temporary gateway 503s followed by endpoint
republication without losing services or allocations.

## Load balancing and safe retries

**What:** The gateway selects ready endpoints round-robin and retries only
idempotent methods after connection failure.

**Why:** Retrying a POST can duplicate an external side effect. A proxy must know
what it can safely repeat.

**When:** Gateways, SDKs, message consumers, and storage clients.

**How:** Stop one echo replica and send repeated GET requests. Then explain why
the gateway does not replay arbitrary POST bodies.

## Observability

**What:** Metrics answer aggregate questions, events explain controller
decisions, and logs show workload detail.

**Why:** One signal cannot answer every operational question. High-cardinality
IDs belong in logs/events, not metric labels.

**When:** Every long-running service.

**How:** Correlate a deploy from API response to event, allocation, command,
worker log, endpoint, gateway count, and Prometheus query.

## React operations console

**What:** A typed frontend consumes a bounded snapshot and performs explicit
mutations through a same-origin authenticated proxy.

**Why:** Operators need the relationship between desired and observed state,
not just a collection of raw database tables.

**When:** Admin systems, deployment consoles, and incident tools.

**How:** Trace `Snapshot` from `dashboard/src/types.ts` through the API client to
service, node, allocation, and event components.

## Windows portability

**What:** MSVC-compilable core, Win32 named-pipe Docker transport, PowerShell
automation, and Docker Desktop Linux-container deployment.

**Why:** “Cross-platform” must cover paths, transports, compilers, scripts, and
runtime assumptions—not merely source syntax.

**When:** Developer platforms and tooling expected to run on personal machines.

**How:** Run the MSVC core tests and the PowerShell end-to-end proof described in
`docs/WINDOWS.md`.

## Suggested higher-level extensions

Implement these in order, each behind tests and an architecture decision record:

1. rolling updates with `maxSurge` and `maxUnavailable`;
2. controller lease/epoch stored in PostgreSQL;
3. mTLS worker identities and certificate rotation;
4. secret references backed by a local encrypted provider;
5. persistent volumes and topology constraints;
6. autoscaling from Prometheus queries with stabilization windows;
7. admission policies and signed-image requirements;
8. multi-host lab deployment and chaos testing.

Those additions move the project from a strong local platform toward deeper
distributed-systems and staff-level design work.

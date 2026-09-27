# M2 — durable control plane

## Outcome

Built a C++ controller with REST and gRPC interfaces backed by PostgreSQL. It
persists desired and observed state and dispatches worker work through a
transactional, leased command outbox.

## What was built

- Versioned service create, scale, restart, list, snapshot, event, and log APIs.
- Node registration, heartbeat, command lease/completion, observations,
  endpoints, and log messages in Protobuf.
- PostgreSQL schema for services, nodes, allocations, commands, events, and
  durable status receipts, with ordered migrations.
- Atomic allocation plus command creation.
- Lease expiry and bounded command attempts.
- Monotonic controller epochs and a singleton advisory lease, so a restarted
  controller fences commands issued by its predecessor.
- Idempotent command completion, 24-hour status-report receipts, and atomic
  log-batch deduplication.
- Worker-loss detection, reservation reconstruction, and allocation revision
  increments before replacement.

## Why

A direct REST-to-Docker call loses intent across crashes. Durable desired state
plus reconciliation makes temporary failure recoverable. The outbox prevents
the database from saying work exists while the delivery channel loses it.

## Files to study

- `database/migrations/001_initial.sql`
- `database/migrations/002_controller_epoch.sql`
- `database/migrations/003_status_report_receipts.sql`
- `proto/controller_worker.proto`
- `cpp/controller/repository.cpp`
- `cpp/controller/api.cpp`
- `cpp/controller/grpc_service.cpp`

## Verification

Run the stack, deploy the echo service, stop a worker after it leases a command,
and restart it after the lease expires. The same allocation must converge
without creating a second owned container.

## Practical lesson

At-least-once delivery is safe only when paired with idempotency and fencing.
“Exactly once” is not assumed; observable outcomes are made repeat-safe.

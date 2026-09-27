# ADR 0002: at-least-once commands with idempotency and fencing

- Status: accepted
- Date: 2026-09-27

## Context

Controller or worker processes can fail after a command is persisted, leased,
applied, or acknowledged. Delivery and side effects cannot be made atomically
exactly-once across PostgreSQL, gRPC, and Docker.

## Decision

Commands are leased and delivered at least once. Command IDs provide bounded
in-process receipt deduplication. After a worker restart, controller/worker
epochs, service generation, allocation revision, ownership labels, and spec
fingerprints reconstruct idempotency and fence stale or conflicting effects.

## Consequences

Temporary failures are retryable and old actors cannot silently overwrite new
intent. The model has more explicit metadata, and every new side effect must
define its own idempotency and fence contract.

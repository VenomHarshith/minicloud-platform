# M4 — service discovery and gateway

## Outcome

Built reconstructable service discovery in Valkey and a C++ reverse proxy that
load-balances only ready, unexpired allocation endpoints.

## What was built

- Expiring endpoint records keyed by service and allocation.
- Replacement of an allocation's older endpoint during republish.
- Gateway parsing for `/services/<service>/...`.
- Round-robin endpoint choice, bounded bodies, deadlines, request IDs, and
  idempotent-method retry behavior.
- Clear 503/502 failure semantics and request metrics.

## Why

Discovery is a live view, not the source of truth. A TTL prevents a dead worker
from routing forever, while periodic republishing reconstructs the view after a
Valkey restart.

## Files to study

- `cpp/controller/valkey_store.cpp`
- `cpp/gateway/proxy.cpp`
- `cpp/gateway/main.cpp`
- `deploy/compose.yaml`

## Verification

Send repeated echo requests and observe allocation IDs alternate. Stop one
allocation and verify it disappears after expiry. Restart Valkey and verify
routes recover after worker republishing without losing service definitions.

## Practical lesson

Durability is a per-dataset decision. Fast ephemeral infrastructure is safe
when every record has a durable origin and a defined reconstruction path.

# ADR 0001: PostgreSQL truth and Valkey ephemeral views

- Status: accepted
- Date: 2026-09-27

## Context

MiniCloud needs transactional desired/observed state, but routing and recent log
lookups benefit from expiring, low-latency records.

## Decision

PostgreSQL stores services, nodes, allocations, commands, and events. Valkey
stores only TTL-bound endpoints and bounded recent log tails. No scheduling or
recovery decision requires Valkey data to survive.

## Consequences

Controller transactions can preserve correctness. Valkey loss causes temporary
routing/log degradation but workers reconstruct endpoints. The design operates
two stores and must test reconstruction explicitly.

# M0 — product architecture and boundaries

## Outcome

Defined MiniCloud as a trusted-local container orchestration platform: Docker is
the container runtime; MiniCloud owns desired state, placement, command
delivery, recovery, discovery, traffic, telemetry, and the operator experience.

## Why this comes first

Senior system design begins with boundaries and invariants, not a framework
list. Without a boundary, a small platform quietly turns into an unsafe and
unfinished clone of a much larger product.

## Decisions made

- PostgreSQL is durable truth; Valkey holds only reconstructable data.
- The controller decides desired state; workers own local side effects.
- Every cross-process action is retryable, idempotent, and fenced.
- All default listeners are local-only; the Docker socket is trusted authority.
- The full local stack uses free/open-source components and no Cisco material.
- Windows support means Docker Desktop Linux containers plus native MSVC proof
  for the portable algorithmic core.

## Files to study

- `docs/ARCHITECTURE.md`
- `docs/THREAT_MODEL.md`
- `docs/CLEAN_ROOM.md`
- `docs/DEPENDENCIES.md`
- `docs/adr/0001-durable-and-ephemeral-state.md`
- `docs/adr/0002-at-least-once-and-fencing.md`
- `docs/adr/0003-docker-socket-trust-boundary.md`

## Verification

Review each component and answer: what state does it own, what happens if it
restarts, and what prevents an old process from applying a new side effect?

## Practical lesson

Architecture quality is visible in failure behavior. A component diagram is
useful only when paired with state ownership, delivery guarantees, trust
boundaries, and explicit non-goals.

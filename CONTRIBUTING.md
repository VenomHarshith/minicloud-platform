# Contributing

MiniCloud favors small, testable changes that preserve its state, delivery, and
trust invariants.

## Before changing code

1. Read `docs/ARCHITECTURE.md`, `docs/THREAT_MODEL.md`, and the relevant ADR.
2. State which component owns the state being changed.
3. Define retry, crash, stale-writer, and compatibility behavior.
4. Add a test or a reproducible end-to-end assertion.

## Local checks

The fast dependency-free check is:

```bash
make core-test
```

With native dependencies installed:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

With Docker available, the release-level proof is:

```bash
make init
./scripts/e2e.sh
```

Use `compose.desktop.yaml` through the documented commands on Docker Desktop.

## Engineering rules

- Keep scheduler/reconciler policy deterministic and dependency-free.
- Treat command delivery as at least once; every side effect needs idempotency
  and a stale-writer fence.
- PostgreSQL owns correctness-critical truth; Valkey data must be reconstructable.
- Never build Docker requests by concatenating shell commands.
- Do not add unbounded bodies, batches, labels, retries, logs, or metric
  cardinality.
- Evolve Protobuf additively; never reuse a field number.
- Preserve Windows/MSVC compilation in portable code.
- Update the matching milestone, ADR, API, operation, and threat-model text when
  behavior changes.

## Clean-room rule

Only original implementation, synthetic examples, public standards, and public
official documentation may be contributed. Employer/customer code, private
architecture, internal names, tickets, logs, screenshots, credentials, and
production data are forbidden.

# M1 — portable scheduling and reconciliation core

## Outcome

Built a dependency-free C++20 library for domain models, scheduling, and
desired-versus-observed reconciliation. Its decisions are deterministic across
input order and supported compilers.

## What was built

- Resource arithmetic for CPU millicores and memory MiB.
- Hard filters for readiness, labels, and remaining capacity.
- Soft anti-affinity and projected-utilization ranking.
- Stable lexical tie-breaking using integer scores.
- Reconciliation actions for missing, excess, stale, and replacement replicas.
- Deterministic allocation and command identities.

## Why

Keeping policy pure separates difficult reasoning from network, database, and
Docker failures. It also creates a small surface that can be compiled natively
on Windows, macOS, and Linux.

## Files to study

- `cpp/include/minicloud/core/domain.hpp`
- `cpp/include/minicloud/core/scheduler.hpp`
- `cpp/include/minicloud/core/reconciler.hpp`
- `cpp/core/`
- `tests/cpp/core_tests.cpp`

## Verification

```bash
make core-test
```

Current evidence: 13 core tests pass under Apple Clang with conversion, sign,
shadow, and pedantic warnings enabled. Cross-compiler proof is part of M6 CI.

## Hands-on extension

Add disk capacity as a third resource. Write filter and ranking tests before
changing the scheduler, then explain why stable tie-breaking still holds.

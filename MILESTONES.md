# MiniCloud milestones

MiniCloud is delivered as one working product, with a durable record of the
engineering decisions made at each stage. The milestone files are not a
separate tutorial application; they explain the actual platform code.

| Milestone | Outcome | Evidence |
|---|---|---|
| M0 | Product boundary, architecture, threat model, and cost policy | `docs/milestones/M0_PRODUCT_ARCHITECTURE.md` |
| M1 | Portable C++ domain, scheduler, reconciler, and deterministic tests | `docs/milestones/M1_PORTABLE_CORE.md` |
| M2 | PostgreSQL control plane, REST API, gRPC protocol, and transactional command outbox | `docs/milestones/M2_DURABLE_CONTROL_PLANE.md` |
| M3 | Docker Engine workers, resource limits, health, restart, logs, and fencing | `docs/milestones/M3_DOCKER_WORKERS.md` |
| M4 | Valkey discovery and C++ load-balancing gateway | `docs/milestones/M4_DISCOVERY_GATEWAY.md` |
| M5 | Prometheus signals and React operations dashboard | `docs/milestones/M5_OBSERVABILITY_DASHBOARD.md` |
| M6 | Windows/macOS/Linux workflows, CI, end-to-end proof, and release pack | `docs/milestones/M6_CROSS_PLATFORM_RELEASE.md` |
| M7 | Sanitized live observer, restricted public edge, and temporary zero-cost sharing workflow | `docs/milestones/M7_PUBLIC_OBSERVER.md` |

For current verification status, read [PROGRESS.md](PROGRESS.md). For exact
commands, start with [RUN_INSTRUCTIONS.md](RUN_INSTRUCTIONS.md).

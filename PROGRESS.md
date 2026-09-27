# Project progress

Last updated: 2026-09-27

## Product implementation

- [x] M0 — architecture, clean-room boundary, cost policy, and threat model.
- [x] M1 — dependency-free C++20 scheduler/reconciler core.
- [x] M2 — PostgreSQL desired/observed model, REST API, gRPC worker protocol,
  command leasing, and transactional outbox.
- [x] M3 — Docker Engine worker runtime, limits, health, bounded restarts,
  ownership checks, status, and log reporting.
- [x] M4 — expiring Valkey service discovery and C++ reverse proxy.
- [x] M5 — React operations console and Prometheus metrics.
- [ ] M6 — full cross-platform release validation and distributable archive.

## Verification ledger

| Check | Result | Notes |
|---|---|---|
| Portable C++ core tests | Passed | 13 tests compiled with local Apple Clang using strict warnings. |
| Portable core sanitizers | Passed | The same 13 tests passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled. |
| Repository contract | Passed | 113 source/document files: JSON, local documentation links, personal-path/credential patterns, and POSIX script syntax. |
| YAML and JSON parsing | Passed | CI, Dependabot, Compose, Prometheus, VS Code, dashboard, and vcpkg definitions parse locally. |
| Credential bootstrap | Passed | POSIX bootstrap generated three 64-character secrets with owner-only permissions and refused overwrite. |
| Echo workload smoke test | Passed | The example server answered `/health` and `/echo` on a loopback test port. |
| Release archives | Passed | Source and information ZIPs were regenerated; archive inspection found no `.env`, Git metadata, build output, caches, dependencies, or logs. |
| Static runtime compilation audit | Passed with test shim | Runtime source was checked with a temporary JSON interface shim; the real dependency build remains part of container CI. |
| Full CMake dependency build | Pending | This host does not currently have CMake/gRPC/Protobuf/PostgreSQL/Valkey development packages. |
| Dashboard production build | Pending | This host does not currently have Node/npm. |
| Compose validation | Pending | This host does not currently have Docker. |
| Real container end-to-end proof | Pending | Requires Docker Desktop/Engine; no paid service is required. |
| Windows MSVC core test | Pending CI | Defined in GitHub Actions; must pass before v0.1 is tagged. |
| Public GitHub Actions workflow | Pending push | The repository exists, but the first push and online run still require GitHub authentication to complete. |

The source-level milestones being complete does not mean the release is claimed
as fully verified. M6 closes only after CI and the real Docker proof pass.

## Original-project safety

This repository is `minicloud-platform`. It does not replace, delete, or rewrite
the existing LocalPlane GitHub repository. The two projects have different
product boundaries, source trees, and future Git histories.

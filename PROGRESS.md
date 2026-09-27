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
- [x] M6 — full cross-platform release validation and distributable archive.

## Verification ledger

| Check | Result | Notes |
|---|---|---|
| Portable C++ core tests | Passed | All 13 tests pass with strict warnings on CI GCC, Clang, and MSVC. |
| Portable core sanitizers | Passed | The same 13 tests passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled. |
| Repository contract | Passed | 117 source/document files: JSON, local documentation links, personal-path/credential patterns, credentials, and POSIX script syntax. |
| YAML and JSON parsing | Passed | CI, Dependabot, Compose, Prometheus, VS Code, dashboard, and vcpkg definitions parse locally. |
| Credential bootstrap | Passed | POSIX bootstrap generated three 64-character secrets with owner-only permissions and refused overwrite. |
| Echo workload smoke test | Passed | The example server answered `/health` and `/echo` on a loopback test port. |
| Release archives | Passed | Source and information ZIPs were regenerated; archive inspection found no `.env`, Git metadata, build output, caches, dependencies, or logs. |
| Full CMake dependency build | Passed | The Ubuntu 24.04 Docker builder compiled every component with C++20 and ran both CTest suites. |
| Dashboard production build | Passed | GitHub CI produced the React/TypeScript/Vite release bundle with Node.js 22. |
| Compose validation | Passed | CI validated the Linux base model and Docker Desktop override. |
| Real container end-to-end proof | Passed | CI started the entire stack, deployed two echo replicas, routed through the gateway, and exercised terminal-failure plus delayed-drain recovery. |
| Windows release checks | Passed | Windows CI compiled/runs the portable core with MSVC and parses every checked-in PowerShell script. |
| Public GitHub Actions workflow | Passed | The [public CI history](https://github.com/VenomHarshith/minicloud-platform/actions/workflows/ci.yml) provides reproducible online evidence. |

M6 is closed. The supported full platform path remains Docker Desktop/Engine;
native host dependencies are optional for component development.

## Original-project safety

This repository is `minicloud-platform`. It does not replace, delete, or rewrite
the existing LocalPlane GitHub repository. The two projects have different
product boundaries, source trees, and future Git histories.

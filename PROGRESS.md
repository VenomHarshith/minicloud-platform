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

## Post-v0.1 public-sharing milestone

- [x] M7a — separate public React build with administrative controls removed.
- [x] M7b — server-side exact-route allowlist and sanitized snapshot facade.
- [x] M7c — hardened Cloudflare Quick Tunnel Compose service with no exposed
  controller, gateway, metrics, database, cache, gRPC, or Docker endpoint.
- [x] M7d — zero-cost/OCI boundaries, safe start/verify/stop procedures,
  Windows notes, and learning concepts documented.

Any generated Quick Tunnel address is intentionally not recorded here: the
checked-in workflow is prepared, but each launch is an explicit operator
action, and its URL is temporary rather than a stable deployment. Every tunnel
session must repeat the checks in
[docs/PUBLIC_OBSERVER.md](docs/PUBLIC_OBSERVER.md) before the link is shared.

## Continuation readiness

- [x] A from-scratch Windows guide now covers personal GitHub authentication
  through HTTPS/Git Credential Manager, repository-local no-reply identity,
  fresh machine-local secrets, Docker Desktop/WSL 2 validation, feature
  branches, testing, pull requests, multi-laptop synchronization, line endings,
  and credential-safe troubleshooting.

## Verification ledger

| Check | Result | Notes |
|---|---|---|
| Portable C++ core tests | Passed | All 13 tests pass with strict warnings on CI GCC, Clang, and MSVC. |
| Portable core sanitizers | Passed | The same 13 tests passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled. |
| Repository contract | Passed | Source/document files: JSON, local documentation links, personal-path/credential patterns, credentials, and POSIX script syntax. |
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
| Public snapshot sanitization | Passed | The focused unit proof rejects representative secrets, raw IDs, endpoints, errors, event payloads, and private labels. |
| Public local edge boundary | Passed | The observer page and live snapshot return successfully while the mutation path is blocked before the controller. |
| Temporary internet URL | Prepared; per session | Quick Tunnel has a random hostname and no uptime guarantee; launch requires explicit operator approval and verification instead of treating one URL as a release artifact. |
| Fresh Windows continuation path | Documented | Clone/auth/run/change/test/push and multi-laptop sync are covered without copying a deploy key or local secrets. |

M6 is closed. The supported full platform path remains Docker Desktop/Engine;
native host dependencies are optional for component development.

## Original-project safety

This repository is `minicloud-platform`. It does not replace, delete, or rewrite
the existing LocalPlane GitHub repository. The two projects have different
product boundaries, source trees, and future Git histories.

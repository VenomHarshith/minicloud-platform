# Dependencies, cost, and licences

MiniCloud application source is MIT licensed. Its normal local stack uses these
separate components over documented APIs:

| Component | Purpose | Licence family | Cost in this project |
|---|---|---|---|
| C++20 standard library | platform implementation | implementation-specific | no application fee |
| gRPC + Protocol Buffers | controller/worker RPC | Apache-2.0 / BSD-style | free |
| Boost.Asio/Beast | HTTP servers and gateway | Boost Software License | free |
| libcurl | Docker/CLI HTTP transport | curl licence | free |
| nlohmann/json | JSON encoding/validation | MIT | free |
| libpqxx + PostgreSQL | durable control state | BSD-style/PostgreSQL | free |
| hiredis + Valkey | discovery/log tails | BSD-style | free |
| React + Vite + TypeScript | dashboard | MIT | free |
| Prometheus | metrics collection/query | Apache-2.0 | free |
| Nginx | dashboard static/proxy server | BSD-2-Clause | free |
| Docker Engine/Desktop | local container runtime | product-specific | Personal is free for eligible personal/educational use |

Pinned versions live in `deploy/compose.yaml`, `dashboard/package.json`, the
Dockerfiles, and `vcpkg.json`. Before a release, CI builds from a clean cache and
dependency scanners should review new transitive packages.

Why Valkey instead of Redis server: Valkey speaks the Redis protocol and is a
Linux Foundation, BSD-licensed project, keeping the local dependency choice
simple and permissive. The C++ code uses hiredis, so the data-access concepts are
directly transferable to Redis deployments.

No paid SaaS, cloud database, hosted metrics provider, registry subscription, or
API key is required. Downloads and local CPU/disk/network usage still belong to
the operator. An always-on public deployment requires compute and may cost
money; it is not part of the default workflow.

This document is an engineering inventory, not legal advice. Review dependency
licences for the exact versions and intended distribution.

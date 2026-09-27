# Changelog

All notable changes to MiniCloud are recorded here.

## Unreleased

### Added

- A separate public-observer dashboard build backed by the real MiniCloud
  control plane, with administrative controls and log access removed.
- A read-only facade that allowlists operational snapshot fields and replaces
  or removes raw service/node/allocation identities, environments, container
  IDs, endpoints, errors, event payloads, and private labels.
- A hardened Compose overlay for the public facade and a free, temporary
  Cloudflare Quick Tunnel that targets only the restricted dashboard edge.
- Cross-platform public start, verification, shutdown, cost, OCI, security,
  and learning guidance in `docs/PUBLIC_OBSERVER.md`.
- Unit coverage for the public snapshot sanitization contract.

### Security

- Public Nginx permits only the exact snapshot read route, rejects all other
  API paths and write methods, adds browser security headers, and rate-limits
  snapshot requests.
- The public facade and tunnel run unprivileged with read-only root filesystems,
  dropped capabilities, and `no-new-privileges`; neither receives a Docker
  socket or a host-published port.
- Controller, gateway, gRPC, Prometheus, PostgreSQL, Valkey, Docker endpoints,
  logs, and administrative actions remain outside the public tunnel.

### Known limits

- Cloudflare Quick Tunnel is temporary testing infrastructure with a random
  URL, no SLA, and availability tied to the operator's running host.
- The public facade internally holds the current full controller token because
  scoped read-only controller credentials do not exist yet.
- Public metadata still includes service/image/node names, selected labels,
  resources, placement state, timestamps, and high-level event types.
- The worker-only SELinux label override used for rootful Podman needs live
  runtime verification on each Docker Desktop environment.

## 0.1.0 - 2026-09-27

### Added

- C++20 controller, worker, gateway, CLI, scheduler, and reconciler.
- PostgreSQL durable desired/observed state and transactional command outbox.
- Token-authenticated gRPC worker registration, heartbeats, leases,
  observations, endpoint publication, and log streaming.
- Docker Engine API runtime supporting Unix sockets, Windows named pipes, and
  explicitly opted-in HTTP/HTTPS TCP endpoints.
- CPU, RAM, PID, privilege, capability, network, ownership, health, restart,
  and revision controls for workloads.
- Valkey-backed expiring discovery and bounded log tails.
- React/TypeScript operations dashboard and Prometheus metrics.
- Docker Compose development cluster, real echo workload, PowerShell/POSIX
  automation, cross-platform CI, and milestone learning records.
- Complete project/technology guide plus automated source and information-pack
  GitHub Release archives with SHA-256 checksums.

### Security

- All published development ports bind to loopback.
- Local secrets are generated into an ignored file with restrictive POSIX
  permissions where supported.
- Workers reject foreign or stale containers before destructive operations.
- Unsafe unauthenticated remote Docker endpoints are rejected by default.
- API, gateway, and database resource names share one strict DNS-label rule.
- Exhausted restart budgets remain failed until an explicit generation change;
  delayed stop tombstones survive temporary worker loss.

### Known limits

- One controller and PostgreSQL primary.
- Laptop workers are logical nodes sharing one Docker engine.
- Trusted-local token authentication instead of remote-production mTLS.
- Remote Docker TCP is an explicit opt-in and requires protection outside v0.1.
- No volumes, secrets provider, autoscaling, or rolling-update surge policy yet.

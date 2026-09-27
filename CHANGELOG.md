# Changelog

All notable changes to MiniCloud are recorded here.

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

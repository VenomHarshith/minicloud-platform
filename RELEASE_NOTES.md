# MiniCloud v0.1.0

MiniCloud is a working local mini container platform built for hands-on systems
engineering. Its C++ control plane schedules Docker workloads, delivers fenced
gRPC commands to workers, supervises health and bounded restarts, publishes
healthy endpoints through Valkey, load-balances traffic, stores desired state
in PostgreSQL, exports Prometheus metrics, and presents operations in a React
dashboard.

## Download choices

- `minicloud-platform-0.1.0-source.zip` contains the complete buildable project.
- `minicloud-information-pack-0.1.0.zip` contains the ordered learning,
  architecture, operations, Windows, API, security, and milestone guides.
- `SHA256SUMS` lets you verify both archives after download.

Both ZIP files are generated from the release tag's committed Git tree. Local
credentials, logs, dependency caches, build output, and other untracked files
are not release inputs.

The repository's `docs/COMPLETE_PROJECT_GUIDE.md` is the single detailed entry
point for definitions, architecture, technology choices, request and failure
flows, use cases, commands, troubleshooting, cost boundaries, limitations, and
next engineering exercises. Start the platform with `RUN_INSTRUCTIONS.md`.

## Verification

GitHub Actions compiles the portable C++ core with GCC, Clang, and MSVC; builds
the full C++ dependency graph and React production bundle; validates both Linux
and Docker Desktop Compose models; starts PostgreSQL, Valkey, Prometheus,
controller, gateway, and workers; deploys two real echo containers; and routes
a request through MiniCloud's gateway. The live proof also checks bounded
terminal-failure recovery and a delayed worker-drain race.

## Cost and hosting boundary

The normal local workflow uses free/open-source components and requires no paid
cloud account or API. GitHub hosts the source, documentation, packages, and
ephemeral automated proof. It does not provide a permanently running MiniCloud
cluster: an always-on public cluster needs a trusted Docker host and may cost
money. No such paid resource is created by this release.

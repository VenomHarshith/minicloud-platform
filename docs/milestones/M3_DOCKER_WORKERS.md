# M3 — Docker workers and self-healing workloads

## Outcome

Built a C++ worker that leases commands, applies structured Docker Engine API
requests, observes containers, streams bounded logs, advertises healthy
endpoints, and performs bounded restart recovery.

## What was built

- Unix-socket, Windows-named-pipe, and explicitly opted-in HTTP/HTTPS Docker
  transports; remote protection remains an operator responsibility.
- CPU, memory, PID, read-only-root, privilege, capabilities, security options,
  port, and network configuration.
- Platform ownership labels and immutable spec fingerprints.
- Stale revision, command replay, and foreign-container protection.
- Health observation, exponential restart backoff, and restart ceilings.
- Worker capacity/labels, heartbeats, status batches, logs, and metrics.

## Why

The worker is the highest-risk boundary: it holds Docker authority and mutates
host runtime state. Commands therefore remain data, never shell strings, and a
destructive action requires proof of ownership plus a matching revision.

## Files to study

- `cpp/runtime/docker_client.cpp`
- `cpp/runtime/worker_runtime.cpp`
- `cpp/worker/main.cpp`
- `tests/cpp/runtime_tests.cpp`
- `docs/THREAT_MODEL.md`

## Verification

Run the end-to-end proof, call `/services/echo-api/crash`, and watch the restart
counter and events. Then create an unrelated Docker container with a colliding
name; the runtime must refuse to remove it when ownership labels do not match.

## Practical lesson

Retries, health checks, and restarts are not enough. Safe orchestration needs
ownership, revision, epoch, and content fences around every local side effect.

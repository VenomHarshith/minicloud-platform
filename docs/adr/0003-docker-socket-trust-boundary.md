# ADR 0003: Docker socket is a trusted-local boundary

- Status: accepted
- Date: 2026-09-27

## Context

A worker must create and inspect containers. Access to a Docker daemon usually
implies broad control of the daemon host.

## Decision

Only worker containers receive the Docker socket. Default services and ports
bind to loopback, user workloads are considered trusted, and unauthenticated
remote TCP endpoints are rejected. Remote production-style deployments require
TLS/mTLS and host isolation beyond v0.1.

## Consequences

The local platform is simple and useful for engineering practice, but it is not
a multi-tenant sandbox. A malicious workload or compromised worker can affect
the host; this limitation is displayed in the README and threat model.

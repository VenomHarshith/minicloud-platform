# Threat model

## Trust boundary

MiniCloud is for one trusted developer/operator and trusted local images. It is
not a hostile multi-tenant sandbox. The controller, workers, Docker daemon,
PostgreSQL, Valkey, and local browser are inside the trusted-machine boundary.

## Highest-risk capability: Docker daemon access

A worker controlling the Docker socket can create privileged host mounts or
otherwise obtain host-equivalent power if its validation is bypassed. Therefore:

- only worker containers receive the socket;
- external API input maps to a bounded `ContainerSpec`, not raw Docker JSON;
- volumes, privileged mode, host network, devices, and arbitrary security
  options are not exposed;
- containers drop Linux capabilities and set `no-new-privileges` by default;
- stop/remove first verify managed, worker, workload, revision, and fingerprint
  labels;
- TCP Docker endpoints are loopback-only unless an operator explicitly enables
  protected remote access;
- unauthenticated Docker TCP port 2375 is forbidden.

Docker-socket possession still remains host-powerful. Validation reduces
accidental/malicious API input; it does not sandbox the worker itself.

## API threats

Every controller route except `GET /health` requires a random bearer token,
including the read-only snapshot. Compose binds published ports to `127.0.0.1`,
JSON content type and strict parsing reduce browser form CSRF, request
bodies/targets are bounded, and unknown fields fail closed. Do not change the
host bind to `0.0.0.0`; a remote deployment needs an authenticated TLS proxy.

## Worker impersonation and stale writers

The local private-network gRPC interface requires a separate cluster token.
Worker instance IDs and epochs fence restarted agents. Commands carry controller
epoch, service generation, allocation revision, command ID, and lease token.
Observations must match current assignment state.

The default profile does not provide confidentiality against a compromised
Compose network. Multi-host deployment requires mutually authenticated TLS and
certificate rotation.

The supported local profile shares one daemon, so a higher allocation revision
can safely take over an old container after a logical-worker loss. With separate
daemons, a replacement worker cannot stop a container on the unreachable old
daemon. Its discovery record expires, but the old process may continue making
external side effects. Treat separate-host mode as experimental until remote
orphan collection and stronger node fencing are implemented.

## Supply-chain and image threats

Submitting an image authorizes Docker to execute it as a workload. MiniCloud
v0.1 deliberately does not handle registry credentials or perform implicit
pulls; the image must already be present on the selected Docker engine.
Operators should:

- use trusted registries and immutable digest references;
- pull or build the image outside MiniCloud, authenticating Docker there when
  needed; never send registry passwords in the service JSON;
- scan/sign images before submission;
- use non-root users and minimal base images;
- avoid mounting host paths (MiniCloud v0.1 does not offer workload volumes);
- review image architecture for the target worker.

## Secret handling

`deploy/.env` is ignored and generated with random values. It must not be added
to Git, pasted into issues, printed in screenshots, or included in support ZIPs.
Service environment is durable configuration, not secret storage. Protobuf
defines secret references for a future provider, but v0.1 rejects them.

## Network threats

- API, gateway, dashboard, Prometheus, and gRPC host mappings are loopback-only.
- Workloads join a dedicated Docker network reachable by the gateway. Workers
  manage them through the Docker API without joining that network.
- The gateway bounds target/body/response sizes and upstream timeouts.
- Only connection failures for idempotent methods are retried.
- There is no ingress TLS; this is not an internet-facing configuration.

## Denial of service

Limits exist for replicas, resource values, JSON sizes, gRPC batches/messages,
HTTP bodies, response bodies, log line/tail size, command attempts, restart
attempts, and metric label cardinality. A trusted image can still consume its
assigned resources or stress the shared Docker daemon.

## Clean-room boundary

Never add employer/customer code, private diagrams, internal hostnames, tickets,
logs, credentials, production data, or non-public architecture information.
MiniCloud contains no Cisco information and needs none.

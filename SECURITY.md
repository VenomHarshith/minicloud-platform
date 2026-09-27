# Security policy

## Supported version

MiniCloud is pre-1.0. Security fixes target the latest `main` branch until the
first tagged release.

## Report a vulnerability

Do not open a public issue containing an exploit, token, host detail, or private
log. Use GitHub's private vulnerability reporting feature for this repository.
Include the affected component, impact, minimal synthetic reproduction, and the
commit tested. Never include employer/customer infrastructure or data.

## Deployment boundary

The default Compose deployment is for one trusted operator, trusted images, and
a local machine. Published ports bind to loopback. It is not an internet-facing
or hostile multi-tenant configuration. The optional
[public observer](docs/PUBLIC_OBSERVER.md) is a separate anonymous, sanitized,
read-only edge; it does not expose the administrative product or workload
gateway.

Treat the Docker socket as host-equivalent authority. Only workers receive it,
but a compromised worker can still control the daemon. Do not mount the socket
into the controller, gateway, dashboard, or arbitrary workload containers.

Read `docs/THREAT_MODEL.md` before changing network binds, Docker endpoints,
authentication, container privileges, or workload validation.

## Secrets

- Never commit `deploy/.env`.
- Never place registry credentials or application secrets in service JSON.
- Never attach databases, log archives, or support bundles without reviewing
  them for credentials and private data.
- Rotate the API/cluster/database values while the cluster is stopped if they
  may have been disclosed.

The project packaging scripts intentionally exclude credentials, Git metadata,
runtime data, dependency caches, and build output.

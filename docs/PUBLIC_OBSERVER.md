# Public live observer deployment

This guide publishes a **live, read-only view** of a real MiniCloud cluster. It
does not expose the administrative controller, workload gateway, Prometheus,
PostgreSQL, Valkey, gRPC port, Docker socket, logs, or write operations.

Use this mode when you want another person to see the platform reconcile real
containers without giving that person control of the host. The normal local
dashboard at `http://127.0.0.1:3000` becomes the read-only build while this
Compose overlay is active. Continue to operate the cluster with
`minicloudctl`, the authenticated loopback API, or a separate private checkout.

The first public option below is free and can be started immediately, but it is
a temporary demonstration URL. The OCI section explains the closest ongoing
zero-cost VM path and its limits; it is not an automatic or guaranteed hosting
service.

## What is actually online

The public page is not a static mock. Its numbers come from the running C++
controller and the real PostgreSQL/worker/Docker state:

```text
Internet browser
      |
      | HTTPS to a random *.trycloudflare.com URL
      v
Cloudflare Quick Tunnel (outbound connector; no inbound host port)
      |
      v
public Nginx + React dashboard
      |
      | only GET /api/v1/snapshot
      v
Python public-api sanitizer
      |
      | authenticated request on the private Compose network
      v
C++ controller --> PostgreSQL / Valkey / workers --> Docker workloads
```

The public URL shows live service counts, desired/ready replicas, worker
capacity, placement state, restart counts, and sanitized event types. It does
**not** let a visitor deploy, scale, restart, delete, read logs, call the
workload gateway, query Prometheus, or reach a Docker API.

## Security boundary

The public boundary is enforced on the server, not only by hiding buttons:

- `dashboard/nginx.public.conf` allows the exact snapshot route and rejects all
  other `/api/` routes. Non-GET requests are denied.
- `deploy/public_api.py` uses the private bearer token only inside the Compose
  network, then constructs a new response from an allowlist.
- raw UUIDs, environments, container IDs, workload endpoints, error strings,
  log lines, event payloads, and non-public node labels are omitted;
- the facade runs as UID/GID `65532`, with a read-only root filesystem, all
  Linux capabilities dropped, `no-new-privileges`, and only a small `/tmp`;
- Nginx applies a Content Security Policy, frame denial, MIME sniffing
  protection, a restrictive permissions policy, and snapshot rate limiting;
- the tunnel targets only public Nginx. Base Compose ports remain bound to
  `127.0.0.1`, and databases and worker metrics have no host port;
- `cloudflared` makes an outbound connection, so this procedure does not open a
  router port or change the host firewall.

The controller does not yet issue a scoped read-only credential. The
`public-api` container therefore holds the full controller token internally;
read-only behavior is enforced by its small facade and Nginx routing. This is a
meaningful residual risk and one reason this edge is a demonstration boundary,
not a production security claim.

Some metadata is intentionally public: service names, image references,
resource requests, replica health, worker display names, `zone`/`arch`/`os`
labels, and high-level event types. Do not use private registry paths, customer
names, internal hostnames, secrets, or sensitive labels in a cluster you share.
A random URL is not authentication: anyone who receives it can view the
sanitized snapshot.

The workers still mount the Docker socket. That socket remains host-powerful,
even though it is not internet-accessible. The public overlay disables SELinux
label separation for only those two worker containers so the rootful Podman
socket can be reached; the public edge containers retain their confinement.
Run only images you trust.

## Requirements

- Docker Desktop in Linux-container mode on Windows/macOS, or Docker Engine
  plus Compose v2 on Linux;
- at least 6 GiB available to Docker;
- internet access to download images and connect the tunnel; and
- an initialized, ignored `deploy/.env` file.

No Cloudflare account, domain, API token, or paid service is needed for a Quick
Tunnel. Your computer, Docker, the stack, and the `cloudflared` container must
remain running for the link to work.

## Temporary free deployment on Linux

Run every command from the repository root.

### 1. Initialize once

```bash
./deploy/scripts/initialize-minicloud.sh
```

The initializer refuses to overwrite existing credentials. Never commit or
share `deploy/.env`.

### 2. Start the real cluster but keep the tunnel closed

```bash
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.public.yaml \
  up -d --build \
  postgres valkey controller worker-a worker-b gateway dashboard prometheus public-api
```

### 3. Deploy and verify the included real workload

For a fresh database:

```bash
docker build -t minicloud/echo-service:dev examples/echo-service
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.public.yaml \
  run --rm cli deploy /workspace/examples/echo-service/service.json
curl -fsS http://127.0.0.1:8080/services/echo-api/
```

If `echo-api` already exists, use the CLI `scale` and `restart` commands rather
than submitting the same name again. Do not run `scripts/e2e.sh` after the
tunnel opens: that script intentionally rebuilds the normal administrative
dashboard. Run the full E2E proof before applying the public overlay if you
want its deeper failure-recovery checks.

### 4. Verify the public boundary locally

```bash
curl -fsS http://127.0.0.1:3000/ >/dev/null
curl -fsS http://127.0.0.1:3000/api/v1/snapshot
curl -i -X POST http://127.0.0.1:3000/api/v1/services
python3 -m unittest tests/test_public_api.py
```

The page and snapshot should return `200`. The POST must not reach the
controller; the public Nginx configuration returns `404`. The unit test checks
that representative secrets, internal identifiers, endpoints, raw errors,
event payloads, and private labels are absent from the sanitized response.

### 5. Open the public tunnel only after those checks pass

```bash
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.public.yaml \
  up -d cloudflared
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.public.yaml \
  logs --no-color cloudflared
```

Find the printed `https://...trycloudflare.com` address, open it in a private
browser window, and repeat the snapshot/blocked-POST checks with that origin.
Share only that HTTPS URL.

### 6. Close public access before stopping the platform

```bash
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.public.yaml \
  stop cloudflared
./scripts/stop.sh
```

The stop script preserves PostgreSQL and Prometheus volumes. Do not add
`--volumes` unless deleting the stored cluster state is intentional.

## Temporary free deployment on macOS

Use the Linux procedure, but add the Docker Desktop socket override between the
base and public files in every Compose command:

```bash
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.desktop.yaml \
  -f deploy/compose.public.yaml \
  up -d --build \
  postgres valkey controller worker-a worker-b gateway dashboard prometheus public-api
```

Use the same three `-f` arguments for `run`, `up cloudflared`, `logs`, and
`stop cloudflared`. Finish with `./scripts/stop.sh` after the tunnel is closed.

## Temporary free deployment on Windows

Use Windows 11, Docker Desktop with WSL 2, and Linux containers. From PowerShell
in the repository root:

```powershell
.\deploy\powershell\Initialize-MiniCloud.ps1

docker compose --env-file deploy/.env `
  -f deploy/compose.yaml `
  -f deploy/compose.desktop.yaml `
  -f deploy/compose.public.yaml `
  up -d --build `
  postgres valkey controller worker-a worker-b gateway dashboard prometheus public-api

.\scripts\windows\Deploy-Echo.ps1
Invoke-RestMethod http://127.0.0.1:3000/api/v1/snapshot
py -m unittest tests/test_public_api.py
```

If `deploy/.env` already exists, skip the initializer. `Deploy-Echo.ps1` is
safe to use with the public build because it does not rebuild Nginx. Then open
the tunnel and read its URL:

```powershell
docker compose --env-file deploy/.env `
  -f deploy/compose.yaml `
  -f deploy/compose.desktop.yaml `
  -f deploy/compose.public.yaml `
  up -d cloudflared

docker compose --env-file deploy/.env `
  -f deploy/compose.yaml `
  -f deploy/compose.desktop.yaml `
  -f deploy/compose.public.yaml `
  logs --no-color cloudflared
```

Close public access first, then use the normal safe stop script:

```powershell
docker compose --env-file deploy/.env `
  -f deploy/compose.yaml `
  -f deploy/compose.desktop.yaml `
  -f deploy/compose.public.yaml `
  stop cloudflared

.\scripts\windows\Stop-MiniCloud.ps1
```

The Windows named-pipe runtime support is not used in this containerized path;
the Desktop override maps Docker's Linux VM socket into each worker.
The merged Compose model validates cross-platform, but the current public
overlay's worker-only `label=disable` option was introduced for a rootful Podman
socket and still needs a live Docker Desktop runtime check on each Windows
setup. Do not publish the URL until both workers register, the echo replicas are
ready, and every local boundary check above passes.

## Quick Tunnel limitations

Cloudflare documents Quick Tunnels as development/testing only:
<https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/>.

- the generated hostname is random and normally changes when the tunnel
  container is recreated;
- there is no uptime or service-level guarantee;
- the documented limit is 200 concurrent in-flight requests;
- Server-Sent Events are unsupported; and
- the link stops when the host, Docker, stack, network, or tunnel stops.

This is useful for a portfolio review or live demonstration. It is not an
always-on production deployment.

## Closest ongoing zero-cost VM path: OCI Always Free

Oracle's current Always Free documentation lists an Ampere A1 allowance
equivalent to 2 Arm OCPUs and 12 GiB of memory for an Always Free tenancy, plus
200 GB total Always Free block-volume storage in the tenancy's home region:
<https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm>.
That is the only mainstream ongoing free allowance identified for this project
that is plausibly large enough to run the complete stack on one VM.

A cautious OCI plan is:

1. The account owner personally creates the OCI account, completes identity and
   payment-card verification, and chooses the home region. MiniCloud never asks
   for or stores those account/payment details.
2. Create one Always Free-eligible Ubuntu Arm64 A1 VM at 2 OCPUs/12 GiB, keeping
   boot and block storage within the displayed Always Free allowance.
3. Permit SSH only from the operator's IP. Do not open ports `2375`, `3000`,
   `50051`, `8080`, `8090`, `9090`, PostgreSQL, or Valkey to the internet.
4. Install Docker Engine and Compose from Docker's official repository, clone
   this repository, generate `deploy/.env`, and run the Linux public-observer
   procedure above.
5. Confirm every image supports `linux/arm64`, enable security updates, back up
   the database, and configure host restart behavior and monitoring.
6. Keep the outbound Quick Tunnel for a temporary random URL. A stable hostname
   requires a separately configured named tunnel and a domain/zone you control,
   or another properly TLS-terminated ingress. Those account/domain choices are
   outside this repository and a domain may cost money.

Important constraints:

- signup normally requires phone/card verification and may place a temporary
  verification authorization; do not proceed until the provider shows the
  resource as Always Free-eligible and the estimated charge as zero;
- A1 capacity can be unavailable in the selected home region;
- Oracle states that idle Always Free compute may be reclaimed;
- the VM, database, and dashboard still have no SLA or high availability;
- Arm64 compatibility must be tested for every workload image; and
- resources outside the free eligibility, region, or allowance can be billed.

For those reasons, an OCI deployment cannot be completed responsibly without
the account owner handling signup, region choice, account security, and the
provider's final cost screen. No OCI resource is created by this repository.

## Cost boundary

| Item | Expected project cost | Important condition |
|---|---:|---|
| Source, C++ stack, PostgreSQL, Valkey, Prometheus, React | $0 | Free/open-source components; local hardware usage still exists. |
| Cloudflare Quick Tunnel | $0 | Testing-only random URL; no SLA; host must stay online. |
| Local demonstration | $0 new subscription | Uses your electricity, storage, bandwidth, and eligible Docker license. |
| OCI Always Free A1 | $0 only within current eligibility | Requires account verification; capacity/reclamation and billing boundaries apply. |
| Stable custom domain | Not guaranteed $0 | A named tunnel needs a controlled zone; domain registration may be paid. |
| Production-grade HA, backups, support | Usually paid | Not supplied by this demo architecture. |

Check provider pricing and the checkout/estimate screen on the day you create a
resource. Free-tier terms can change. Stop before creating anything that shows
a non-zero estimate.

## Troubleshooting

- **No public URL in logs:** inspect `cloudflared` logs for outbound DNS/TLS
  errors; Quick Tunnel creation needs internet access.
- **Public page returns 503:** inspect `public-api` and `controller` logs. The
  facade deliberately converts upstream failures into a generic message.
- **Page has deploy/restart controls:** stop the tunnel immediately and rebuild
  `dashboard` with `compose.public.yaml` last in the file list.
- **POST reaches the controller:** stop the tunnel immediately. Confirm the
  dashboard image uses `dashboard/nginx.public.conf` and rerun the blocked-POST
  check before reopening it.
- **Snapshot reveals an unsuitable name or image:** close the tunnel, rename or
  remove that local service, and only then share a new link.
- **Echo image has `exec format error`:** build an image for the VM/Docker
  architecture; OCI A1 is Arm64.
- **Dashboard works but workload does not:** the observer is healthy while the
  data plane is not. Inspect private worker logs, health checks, discovery, and
  the local gateway.

## What this deployment teaches

- **Control plane versus presentation plane:** the real scheduler and workers
  remain authoritative while a separate read model serves observers.
- **Backend-for-frontend/facade:** the public API constructs a deliberately
  smaller contract instead of forwarding an internal response unchanged.
- **Least privilege and allowlisting:** one read route is opened; everything
  else remains closed by default.
- **Defense in depth:** UI removal, Nginx method/path policy, sanitization,
  container hardening, loopback binds, and an outbound-only tunnel overlap.
- **Data classification:** operational data is divided into publishable counts
  and private identities, endpoints, errors, logs, configuration, and secrets.
- **Compose overlays:** the same platform gets an environment-specific edge
  without duplicating the base stack.
- **Egress tunnels:** a service behind NAT can be shared without opening an
  inbound router port, while the tunnel provider becomes part of the trust and
  availability model.
- **Reliability versus price:** a zero-cost testing URL, an reclaimable free VM,
  and a production service offer very different guarantees.
- **Architecture portability:** a real deployment forces Arm64 image, resource,
  restart, storage, firewall, TLS, and operations decisions that a static site
  never exercises.

The next security milestone for a genuinely interactive public platform is not
to expose the existing admin API. It is to design authenticated users, roles,
tenant quotas, image admission, per-tenant isolation, TLS/mTLS identity, audit
retention, abuse controls, backup/restore, and a safe workload ingress model.

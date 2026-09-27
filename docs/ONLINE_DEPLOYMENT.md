# Online availability and deployment

MiniCloud now has three different forms of online availability. They solve
different problems and should not be confused:

| Form | What is online | Duration | Public authority |
|---|---|---|---|
| GitHub repository and CI | Source, guides, releases, and an automated full-stack proof | Persistent source; temporary CI runner | No access to your cluster |
| Public live observer | Sanitized state from a real cluster through a Quick Tunnel | While your host and tunnel run | Read-only snapshot only |
| Cloud VM | The same real stack on an operator-managed Docker host | While the VM/account remain available | Depends on the edge you configure |

The exact public-observer architecture, commands, security checks, Windows
path, OCI option, costs, and limitations are in
[PUBLIC_OBSERVER.md](PUBLIC_OBSERVER.md).

## Public source and repeatable online proof

The public GitHub repository is the canonical online copy. Its CI workflow:

1. compiles the portable core on Linux, macOS, and Windows;
2. builds the React dashboard;
3. validates Linux and Docker Desktop Compose models;
4. builds every C++ container image;
5. starts PostgreSQL, Valkey, controller, workers, gateway, dashboard, and
   Prometheus on an ephemeral Ubuntu runner; and
6. deploys the real echo container and calls it through service discovery and
   the gateway.

GitHub documents standard hosted runners as free for public repositories:
<https://docs.github.com/en/actions/concepts/billing-and-usage>.

This is reproducible evidence that the product works. The runner is destroyed
after the job, so it is not a public always-on cluster.

## Temporary live public observer

`deploy/compose.public.yaml` adds a sanitized `public-api`, builds the dashboard
without administrative actions, applies a server-side read-route allowlist,
and connects only that restricted edge to a Cloudflare Quick Tunnel. A visitor
sees real desired and observed state but cannot deploy, scale, restart, delete,
read logs, reach workloads, query metrics, or contact the control plane.

Quick Tunnels need no account or domain and generate a random public HTTPS URL.
Cloudflare explicitly describes them as testing/development only, with no SLA,
a 200-concurrent-request limit, and a URL tied to the tunnel process:
<https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/>.

This mode costs no new subscription, but it uses the operator's machine,
electricity, storage, bandwidth, and internet connection. It is appropriate for
a portfolio demonstration, not production hosting.

## Why the full tool is not a static website

MiniCloud controls a Docker daemon, starts containers, applies CPU/RAM limits,
stores state in PostgreSQL, publishes discovery through Valkey, and scrapes live
metrics. Static hosting such as GitHub Pages can publish HTML/CSS/JavaScript but
cannot provide a Docker daemon or long-running controller/worker processes. The
GitHub Pages boundary is documented here:
<https://docs.github.com/en/pages/getting-started-with-github-pages/what-is-github-pages>.

The observer is therefore backed by the actual local/cloud stack; it is not a
simulated frontend. The administrative tool remains private by design.

## Closest ongoing zero-cost VM option

OCI documents an Always Free Ampere A1 allowance equivalent to 2 Arm OCPUs and
12 GiB RAM for an Always Free tenancy, plus 200 GB total eligible block storage
in the home region:
<https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm>.

That capacity is plausibly sufficient for this single-VM lab, but it is not a
guaranteed hosting entitlement. Account signup normally requires identity,
phone, and payment-card verification; capacity may be unavailable; idle free
instances can be reclaimed; Arm64 workload compatibility is required; and
resources outside the displayed free eligibility can incur charges. The
account owner must personally verify the provider's zero-cost estimate before
creating anything.

MiniCloud does not create OCI resources, accept cloud/payment credentials, or
silently enable billable services. A stable hostname also needs a separately
configured TLS edge and a domain/zone the operator controls, which may introduce
a cost.

## What must stay private

Never expose the default dashboard or change the base port binds from
`127.0.0.1` to `0.0.0.0`. The default Nginx proxy adds an administrative bearer
token, and each worker has host-powerful Docker-socket access.

Keep these private:

- controller REST and gRPC;
- normal administrative dashboard;
- workload gateway until a real ingress/authentication policy exists;
- Prometheus, PostgreSQL, and Valkey;
- Docker socket/endpoints and worker credentials; and
- `deploy/.env`, internal logs, container endpoints, and raw event payloads.

The public overlay narrows one observer path. It does not convert MiniCloud into
a secure hostile multi-tenant cloud. An interactive public platform still needs
authenticated users and roles, tenant isolation/quotas, mTLS worker identity,
image admission, secret management, TLS workload ingress, abuse protection,
backup/restore, high availability, audit retention, and an incident runbook.

## Options that may become paid

- compute, storage, snapshots, traffic, or IPs outside a cloud free allowance;
- GitHub Codespaces beyond its included personal quota;
- a domain name, managed load balancer, managed database, or metrics service;
- production support, monitoring, backup retention, or high availability.

GitHub documents that personal Codespaces becomes metered beyond the included
quota:
<https://docs.github.com/en/billing/concepts/product-billing/github-codespaces>.

Check current provider terms immediately before provisioning. Stop before any
screen that shows a non-zero estimate unless you explicitly choose to pay.

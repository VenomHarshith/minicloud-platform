# Online availability and deployment

## What is online for free

The public GitHub repository is the canonical online copy. Its CI workflow:

1. compiles the portable core on Linux, macOS, and Windows;
2. builds the React dashboard;
3. validates Linux and Docker Desktop Compose models;
4. builds every C++ container image;
5. starts PostgreSQL, Valkey, controller, workers, gateway, dashboard, and
   Prometheus on an ephemeral Ubuntu runner;
6. deploys the real echo container and calls it through service discovery and
   the gateway.

GitHub documents standard hosted runners as free for public repositories:
<https://docs.github.com/en/actions/concepts/billing-and-usage>.

This gives online, repeatable evidence that the product works. The runner is
destroyed after the job, so it is deliberately not a public always-on cluster.

## Why the full tool is not a static website

MiniCloud controls a Docker daemon, starts containers, applies CPU/RAM limits,
stores state in PostgreSQL, publishes discovery through Valkey, and scrapes live
metrics. Static hosting such as GitHub Pages can publish HTML/CSS/JavaScript but
cannot provide a Docker daemon or long-running controller/worker processes. The
GitHub Pages boundary is documented here:
<https://docs.github.com/en/pages/getting-started-with-github-pages/what-is-github-pages>.

Publishing only a dashboard mock would look deployed while omitting the actual
orchestrator, so this repository does not present that as the product.

## Zero-cost ways to run the real platform

- Run it on your own Windows/macOS/Linux machine with Docker Desktop/Engine.
- Run it on a spare personal Linux machine and reach it through a private VPN;
  add TLS/mTLS and remove the local-only assumptions first.
- Use an ephemeral GitHub Actions run for automated end-to-end proof.

All use existing hardware or standard public-repository CI. Electricity,
network, and local disk usage still belong to the operator.

## Options that may become paid

- A general cloud VM capable of running Docker continuously.
- GitHub Codespaces after its personal monthly compute/storage quota is used.
- A domain name, public load balancer, managed database, or managed metrics
  service.

GitHub documents that personal Codespaces include a monthly quota and become
metered beyond that quota:
<https://docs.github.com/en/billing/concepts/product-billing/github-codespaces>.

MiniCloud will not create any of those resources without an explicit decision
about provider, security, shutdown policy, and spending limit.

## Requirements before an internet-facing deployment

The default Compose stack must not be exposed directly. A real public deployment
requires at minimum:

- mTLS worker identity and certificate rotation;
- authenticated TLS for every API and dashboard read route;
- a protected Docker endpoint per isolated worker host;
- a firewall and private control network;
- PostgreSQL backup/restore and controller leader election;
- external secret management;
- signed/scanned images and an admission policy;
- quotas, rate limiting, audit retention, and an incident runbook.

Those are explicit production-hardening milestones, not hidden switches in the
local configuration.

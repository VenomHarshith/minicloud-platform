# Run MiniCloud

This is the exact operating guide. Run commands from the repository root.

VS Code users can run the same checked-in commands from **Terminal → Run Task**;
the ordered workflow and what each task does are in
[docs/VSCODE.md](docs/VSCODE.md).

This file starts the private administrative dashboard. To share a live
read-only URL, follow [docs/PUBLIC_OBSERVER.md](docs/PUBLIC_OBSERVER.md) instead.
Never place the normal dashboard behind a public tunnel.

## 1. Install the one mandatory platform dependency

Install Docker Desktop on Windows/macOS, or Docker Engine plus Compose v2 on
Linux. Confirm:

```text
docker version
docker compose version
```

Docker Desktop must be using Linux containers. No local CMake, C++ libraries,
Node, PostgreSQL, Valkey, gRPC, or Prometheus installation is required for the
containerized path; the Docker build supplies them. The macOS/Linux end-to-end
helper also requires host `curl`. `make` is optional shorthand for the explicit
commands in this guide.

## 2. Generate local credentials once

Windows PowerShell:

```powershell
.\deploy\powershell\Initialize-MiniCloud.ps1
```

macOS/Linux:

```bash
./deploy/scripts/initialize-minicloud.sh
```

This writes ignored `deploy/.env` with three random local values: PostgreSQL
password, worker cluster token, and controller API token, plus local Docker and
architecture settings. It refuses to overwrite an existing file. Preserve this
file while its PostgreSQL volume exists; generating a new password does not
change the password stored in an initialized database.

## 3. Start the platform

Fresh databases apply all checked-in migrations automatically. If this checkout
will reuse a prerelease PostgreSQL volume below schema version 4, first follow
[the non-destructive migration procedure](docs/OPERATIONS.md#upgrade-an-existing-prerelease-database).

Windows PowerShell:

```powershell
.\scripts\windows\Start-MiniCloud.ps1
```

macOS Docker Desktop:

```bash
docker compose --env-file deploy/.env \
  -f deploy/compose.yaml \
  -f deploy/compose.desktop.yaml \
  up -d --build
```

Linux Docker Engine:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml up -d --build
```

The first build is slower because Docker downloads the compiler/dependencies and
base images. Those downloads are free but require internet access and disk space.

Check state:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml ps
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=100
```

## 4. Prove the complete path with a real container

Windows:

```powershell
.\scripts\windows\Deploy-Echo.ps1
```

macOS/Linux:

```bash
./scripts/e2e.sh
```

The proof builds `minicloud/echo-service:dev`, submits two replicas, waits for
scheduling and health, and calls it through the load-balancing gateway. The
POSIX proof also checks that terminal failures require an explicit restart and
that a stop delayed by worker interruption cannot create a duplicate runtime.

Expected final line:

```text
MiniCloud end-to-end verification passed.
```

## 5. Use the UI

Open <http://127.0.0.1:3000>. The console shows:

- desired and ready replica counts;
- worker CPU/RAM reservations and labels;
- allocation/container placement and restart counts;
- recent reconciliation events;
- bounded container log tails;
- deploy, scale, and restart controls.

Prometheus is at <http://127.0.0.1:9090>. Try queries such as:

```promql
minicloud_controller_reconcile_total
minicloud_runtime_commands_total
minicloud_gateway_requests_total
```

## 6. Exercise failure recovery

Request the example's intentional crash endpoint:

```bash
curl -fsS http://127.0.0.1:8080/services/echo-api/crash
```

Watch the worker and dashboard:

```bash
docker compose --env-file deploy/.env -f deploy/compose.yaml logs -f worker-a worker-b
```

The owning worker observes the exited/unhealthy container, applies bounded
backoff, and restarts it until the configured ceiling.

## 7. Stop safely

Windows:

```powershell
.\scripts\windows\Stop-MiniCloud.ps1
```

macOS/Linux:

```bash
./scripts/stop.sh
```

`down` stops containers but preserves named PostgreSQL and Prometheus volumes.
The checked-in stop scripts also remove ephemeral MiniCloud-managed workload
containers so the workload network can be removed cleanly; desired service
state remains in PostgreSQL and is recreated after the next start. Do not add
`--volumes` unless you intentionally want to destroy local state.

## 8. Native developer build

The Docker path is the supported beginner path. For native C++ work, install
CMake, Ninja, a C++20 compiler, and the dependencies in `vcpkg.json`. CMake
requires libpqxx 7.9.x and rejects libpqxx 8; the manifest does not pin a vcpkg
registry baseline, so do not assume the latest vcpkg checkout is compatible.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Windows native setup is in [docs/WINDOWS.md](docs/WINDOWS.md).

## Troubleshooting

- `docker: command not found`: Docker is not installed or its CLI is not on PATH.
- Docker reports Windows containers: switch Docker Desktop to Linux containers.
- Worker cannot open the socket: use `compose.desktop.yaml` on Docker Desktop;
  use the base file on ordinary Linux Docker Engine.
- API returns 401: make sure the caller uses the current
  `MINICLOUD_API_TOKEN` from `deploy/.env`; do not regenerate credentials while
  retaining an initialized PostgreSQL volume.
- Gateway returns 503: no healthy endpoint exists; inspect allocations, worker
  logs, and whether the workload image contains `wget` for the v0.1 health probe.
- Container creation reports a missing image: pull it with Docker first (and
  authenticate Docker there if needed), or build it locally. MiniCloud v0.1
  does not accept registry credentials or perform implicit pulls.
- Apple Silicon/Windows ARM: use a multi-platform image with `linux/arm64`.
- Ports 3000/8080/8090/9090 are busy: stop the other process or change the host
  side of the mapping in a personal Compose override.

Never expose the Docker daemon on unauthenticated TCP port 2375.

If a public-observer tunnel is running, do not rerun this guide's start command,
`make up`, `make demo`, `scripts/e2e.sh`, or `Start-MiniCloud.ps1`: those paths
do not include the public overlay and may replace the restricted dashboard with
the administrative build while the tunnel remains alive. Close the tunnel
first, or use every compose file specified by the public-observer guide.

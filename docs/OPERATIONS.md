# Operations and failure drills

## Quick health map

```text
docker compose --env-file deploy/.env -f deploy/compose.yaml ps
curl -fsS http://127.0.0.1:8090/health
docker compose --env-file deploy/.env -f deploy/compose.yaml run --rm cli status
curl -fsS http://127.0.0.1:8080/health
curl -fsS http://127.0.0.1:9090/-/healthy
```

Use the desktop override in Docker Desktop commands that start worker services.
The containerized CLI obtains the API token from `deploy/.env`; direct snapshot
requests must send the same bearer token.

## Logs

```text
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=200 controller
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=200 worker-a worker-b
docker compose --env-file deploy/.env -f deploy/compose.yaml logs --tail=200 gateway
```

Never publish `deploy/.env` or verbose request headers.

## Drill: container crash

Call `/services/echo-api/crash`. Expected: one allocation leaves ready state,
its endpoint expires, the gateway uses the other replica, the worker applies
restart backoff, health returns, and the endpoint is republished.

## Drill: worker interruption

Stop `worker-a`. Expected after heartbeat timeout: node becomes not ready,
allocations become lost, replacement revisions schedule on an eligible worker,
and stale reports from the prior assignment are rejected.

## Drill: drain during worker interruption

Pause a worker process, then scale its service down before the heartbeat
timeout. The controller retains the current stop command instead of losing the
drain intent when the node becomes not ready. If the service is immediately
scaled back up, that replica remains stopping until the same worker resumes and
acknowledges removal; only then can the replica be scheduled again. This is an
intentional v0.1 safety tradeoff: temporary availability is lower, but two
containers cannot be created for the same logical replica merely because a stop
acknowledgement was delayed. `scripts/e2e.sh` automates this same-process drill.

If a separate remote daemon never returns, MiniCloud v0.1 cannot prove that its
old container stopped. Its durable allocation remains visibly stopping and an
operator must recover or clean that daemon. If ownership was already lost
before the scale-down request, remote orphan collection is outside v0.1.

## Drill: Valkey restart

Restart Valkey. Expected: desired state is unchanged, gateway may briefly return
503, then periodic worker reports reconstruct discovery. Log tails may be lost.

## Drill: controller restart

Restart controller. PostgreSQL retains services, allocations, commands, and
events. Workers reconnect and register. Leased commands become available after
lease timeout; runtime IDs/labels prevent duplicate side effects.

## Drill: PostgreSQL unavailable

The controller cannot reconcile or accept desired-state changes. Existing
containers continue running; gateway discovery continues until endpoint TTLs
expire unless workers can report to a recovered controller. Restore PostgreSQL
before changing desired state.

## Capacity debugging

If allocations stay pending, compare request CPU/RAM/labels with ready-node free
capacity in the dashboard. The scheduler never overcommits declared capacity in
v0.1. Docker Desktop's real VM capacity can be lower than the logical values, so
align the worker declarations with Desktop settings for realistic drills.

## Data preservation

`docker compose down` preserves named volumes. `down --volumes` destroys the
PostgreSQL and Prometheus volumes and is intentionally absent from normal
scripts. Before destructive reset, export needed evidence and verify the exact
Compose project name.

Use `make down` on macOS/Linux or `.\scripts\windows\Stop-MiniCloud.ps1` on
Windows instead of invoking `docker compose down` directly. Workload containers
are created through the Docker API rather than as Compose services. The stop
scripts select only containers with `io.minicloud.managed=true` on
`minicloud-workloads`, stop and remove them, and then remove the Compose stack.
The workload containers are ephemeral; desired state remains in PostgreSQL.

## Upgrade an existing prerelease database

On a fresh empty PostgreSQL volume, the official image runs
`database/migrations/001_initial.sql`, `002_controller_epoch.sql`, and
`003_status_report_receipts.sql`, followed by
`004_dns_label_constraints.sql`, in filename order. PostgreSQL init scripts do
not run again for a non-empty volume. The current controller therefore refuses
to start when an older volume reports a schema version below 4.

Keep the existing `deploy/.env`: its database password belongs to the existing
volume. Do not rerun a credential initializer to perform a schema upgrade. If an
older file lacks `MINICLOUD_ARCH`, add `MINICLOUD_ARCH=amd64` or
`MINICLOUD_ARCH=arm64` for the Docker host without replacing its secrets.

For a version-1, version-2, or version-3 prerelease volume, stop database
writers and start PostgreSQL alone:

```text
docker compose --env-file deploy/.env -f deploy/compose.yaml stop controller worker-a worker-b
docker compose --env-file deploy/.env -f deploy/compose.yaml up -d postgres
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres psql -X -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c "SELECT version, description FROM schema_migrations ORDER BY version;"
```

Take a logical backup outside the repository before changing the schema:

```text
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres pg_dump -U minicloud -d minicloud --format=custom --file=/tmp/minicloud-before-schema-v4.dump
docker compose --env-file deploy/.env -f deploy/compose.yaml cp postgres:/tmp/minicloud-before-schema-v4.dump ../minicloud-before-schema-v4.dump
```

Migration 004 deliberately rejects a pre-existing service or node name ending
in `-`, because those names were accepted by an early REST/database rule but
cannot be used safely as worker resource identities. Check first:

```text
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres psql -X -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c "SELECT 'service' AS kind,name FROM services WHERE name !~ '^[a-z][a-z0-9-]{0,62}$' OR name ~ '-$' UNION ALL SELECT 'node',name FROM nodes WHERE name !~ '^[a-z][a-z0-9-]{0,62}$' OR name ~ '-$';"
```

If that query returns a row, stop and either rename the prerelease record to a
unique valid DNS label after reviewing its references, or restore/discard the
prerelease data. The migration never guesses a replacement identity.

Apply the remaining idempotent migrations in order. Reapplying one already
present is safe because its objects and migration record use `IF NOT EXISTS`,
replace an equivalent named constraint, or use `ON CONFLICT`:

```text
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres psql -X -v ON_ERROR_STOP=1 -U minicloud -d minicloud -f /docker-entrypoint-initdb.d/002_controller_epoch.sql
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres psql -X -v ON_ERROR_STOP=1 -U minicloud -d minicloud -f /docker-entrypoint-initdb.d/003_status_report_receipts.sql
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres psql -X -v ON_ERROR_STOP=1 -U minicloud -d minicloud -f /docker-entrypoint-initdb.d/004_dns_label_constraints.sql
docker compose --env-file deploy/.env -f deploy/compose.yaml exec -T postgres psql -X -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c "SELECT version, description FROM schema_migrations ORDER BY version;"
```

Confirm versions 1, 2, 3, and 4 are present, then start the platform with the
normal command for the host. The dump can contain private service configuration;
store it securely and remove it when it is no longer needed. If all prerelease
state is disposable, a deliberate volume reset is another option, but no
MiniCloud script performs or implies that destructive choice.

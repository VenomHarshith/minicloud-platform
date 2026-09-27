# REST API

The API binds to `127.0.0.1:8090` through Compose. Only `GET /health` is
unauthenticated. The snapshot, mutation, and log routes require the random
bearer token in ignored `deploy/.env`.

Load it without printing it:

```bash
MINICLOUD_API_TOKEN=$(awk -F= '/^MINICLOUD_API_TOKEN=/{print $2}' deploy/.env)
```

## Health

```http
GET /health
```

## Cluster snapshot

```http
GET /api/v1/snapshot
Authorization: Bearer TOKEN
```

Returns fixed top-level `overview`, `services`, `nodes`, `allocations`, and
`events` fields used by the dashboard.

```bash
curl -fsS http://127.0.0.1:8090/api/v1/snapshot \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN"
```

## Create service

```http
POST /api/v1/services
Authorization: Bearer TOKEN
Content-Type: application/json
```

```json
{
  "name": "echo-api",
  "image": "minicloud/echo-service:dev",
  "replicas": 2,
  "cpuMillis": 200,
  "memoryMb": 128,
  "containerPort": 8080,
  "healthPath": "/health",
  "environment": {"APP_MODE": "demo"},
  "placement": {
    "requiredLabels": {"os": "linux-containers"},
    "antiAffinityGroup": "echo"
  }
}
```

Unknown fields, invalid names, oversized bodies, out-of-range resources, and
invalid placement shapes are rejected. Do not put secrets in `environment`;
desired state is stored in PostgreSQL and returned to authorized operators.

Example:

```bash
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services \
  -H "Authorization: Bearer $MINICLOUD_API_TOKEN" \
  -H 'content-type: application/json' \
  --data-binary @examples/echo-service/service.json
```

## Scale

```http
POST /api/v1/services/{name}/scale
Authorization: Bearer TOKEN
Content-Type: application/json

{"replicas":3}
```

Supported range is 0–50.

Scaling changes only the desired replica count. It does not increment the
service generation or replace retained replicas. Scaling up creates or
reactivates missing replica indexes; scaling down drains indexes above the new
desired count.

## Replacement/restart request

```http
POST /api/v1/services/{name}/restart
Authorization: Bearer TOKEN
```

This increments the service generation. v0.1 replaces every retained allocation
through normal reconciliation, but does not provide rolling ordering or a
configurable surge/unavailable policy.

## Delete/drain service

```http
DELETE /api/v1/services/{name}
Authorization: Bearer TOKEN
```

Deletion requests an asynchronous scale-to-zero/drain. The service record and
events remain for inspection in v0.1. A container on an unavailable remote
worker may remain until that worker returns or the remote daemon is cleaned;
the supported local stop scripts clean owned containers on the shared daemon.

## Allocation logs

```http
GET /api/v1/allocations/{allocation-id}/logs?tail=200
Authorization: Bearer TOKEN
```

`tail` is bounded to 1–1000. Logs are recent, expiring Valkey tails and may have
gaps during worker/controller interruption.

## Gateway

```http
ANY /services/{service-name}/{upstream-path}
```

Example:

```bash
curl -fsS 'http://127.0.0.1:8080/services/echo-api/echo?message=hello'
```

The gateway retries GET, HEAD, PUT, DELETE, OPTIONS, and TRACE after connection
failure, but never retries POST. It does not provide public TLS or public
authentication and must remain loopback-bound.

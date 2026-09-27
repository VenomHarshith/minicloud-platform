#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compose_file="$root/deploy/compose.yaml"
env_file="$root/deploy/.env"

set -- --env-file "$env_file" -f "$compose_file"
if [ "$(uname -s)" = "Darwin" ] || [ "${MINICLOUD_COMPOSE_DESKTOP:-0}" = "1" ]; then
  set -- "$@" -f "$root/deploy/compose.desktop.yaml"
fi

compose() {
  docker compose "$@"
}

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/minicloud-e2e.XXXXXX")
failure_worker=""
failure_worker_container=""
failure_worker_paused=0
drain_worker=""
drain_worker_container=""
drain_worker_paused=0
cleanup() {
  if [ "$failure_worker_paused" -eq 1 ] && [ -n "$failure_worker_container" ]; then
    docker unpause "$failure_worker_container" >/dev/null 2>&1 || true
  fi
  if [ "$drain_worker_paused" -eq 1 ] && [ -n "$drain_worker_container" ]; then
    docker unpause "$drain_worker_container" >/dev/null 2>&1 || true
  fi
  rm -rf "$temporary_directory"
}
trap cleanup EXIT HUP INT TERM

if [ ! -f "$env_file" ]; then
  "$root/deploy/scripts/initialize-minicloud.sh"
fi

compose "$@" up -d --build
docker build -t minicloud/echo-service:dev "$root/examples/echo-service"

attempt=0
until curl -fsS http://127.0.0.1:8090/health >/dev/null 2>&1; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 60 ]; then
    compose "$@" logs --tail=200
    echo "controller did not become healthy" >&2
    exit 1
  fi
  sleep 2
done

api_token=$(awk -F= '/^MINICLOUD_API_TOKEN=/{print $2}' "$env_file")
status=$(curl -sS -o "$temporary_directory/deploy-response.json" -w '%{http_code}' \
  -X POST http://127.0.0.1:8090/api/v1/services \
  -H "Authorization: Bearer $api_token" \
  -H 'content-type: application/json' \
  --data-binary "@$root/examples/echo-service/service.json")
if [ "$status" != "201" ] && [ "$status" != "409" ]; then
  cat "$temporary_directory/deploy-response.json" >&2
  exit 1
fi

# Make reruns converge even when a previous interrupted proof left the retained
# service drained. Scaling an already-two-replica service is harmless.
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services/echo-api/scale \
  -H "Authorization: Bearer $api_token" \
  -H 'content-type: application/json' \
  --data-binary '{"replicas":2}' >/dev/null
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services/echo-api/restart \
  -H "Authorization: Bearer $api_token" >/dev/null

attempt=0
until curl -fsS http://127.0.0.1:8080/services/echo-api/ >"$temporary_directory/echo-response.json" 2>/dev/null; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 90 ]; then
    compose "$@" logs --tail=250
    echo "echo service did not become routable" >&2
    exit 1
  fi
  sleep 2
done

cat "$temporary_directory/echo-response.json"
printf '\n'

# Regression: the controller must preserve the durable terminal state produced
# by an accepted worker report until an explicit operator restart changes
# service generation. Set that state directly to isolate repository
# reconciliation, while a paused worker prevents unrelated status updates.
failure_worker=$(compose "$@" exec -T postgres psql -X -At \
  -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
  "SELECT n.name FROM allocations a JOIN services s ON s.service_id=a.service_id JOIN nodes n ON n.node_id=a.node_id WHERE s.name='echo-api' AND a.desired_state='running' AND a.observed_state='running' AND a.endpoint IS NOT NULL AND n.status='ready' AND n.name IN ('worker-a','worker-b') ORDER BY n.name,a.replica LIMIT 1;")
case "$failure_worker" in
  worker-a|worker-b) ;;
  *)
    echo "failed to select a ready echo allocation owner" >&2
    exit 1
    ;;
esac

failure_worker_container=$(compose "$@" ps -q "$failure_worker")
if [ -z "$failure_worker_container" ]; then
  echo "$failure_worker container was not found" >&2
  exit 1
fi
docker pause "$failure_worker_container" >/dev/null
failure_worker_paused=1

failed_row=$(compose "$@" exec -T postgres psql -X -At \
  -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
  "SELECT a.allocation_id::text || '|' || a.allocation_revision::text || '|' || a.service_generation::text FROM allocations a JOIN services s ON s.service_id=a.service_id JOIN nodes n ON n.node_id=a.node_id WHERE s.name='echo-api' AND n.name='$failure_worker' AND a.desired_state='running' ORDER BY a.replica LIMIT 1;")
case "$failed_row" in
  *'|'*'|'*) ;;
  *)
    echo "failed to select an allocation on $failure_worker" >&2
    exit 1
    ;;
esac
failed_allocation=${failed_row%%|*}
failed_fences=${failed_row#*|}
failed_revision=${failed_fences%%|*}
failed_generation=${failed_fences#*|}

compose "$@" exec -T postgres psql -X -q \
  -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
  "UPDATE allocations SET observed_state='failed',restart_count=5,last_error='injected terminal failure for E2E',updated_at=now() WHERE allocation_id='$failed_allocation';"

sleep 3
stable_failure=$(compose "$@" exec -T postgres psql -X -At \
  -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
  "SELECT COUNT(*) FROM allocations a WHERE a.allocation_id='$failed_allocation' AND a.observed_state='failed' AND a.allocation_revision=$failed_revision AND a.service_generation=$failed_generation;")
if [ "$stable_failure" -ne 1 ]; then
  compose "$@" logs --tail=250
  echo "controller reset a terminal failure without an operator restart" >&2
  exit 1
fi

curl -fsS -X POST http://127.0.0.1:8090/api/v1/services/echo-api/restart \
  -H "Authorization: Bearer $api_token" >/dev/null

attempt=0
while :; do
  deliberate_recovery=$(compose "$@" exec -T postgres psql -X -At \
    -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
    "SELECT COUNT(*) FROM allocations a WHERE a.allocation_id='$failed_allocation' AND a.observed_state='pending' AND a.allocation_revision>$failed_revision AND a.service_generation>$failed_generation;")
  if [ "$deliberate_recovery" -eq 1 ]; then
    break
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 6 ]; then
    compose "$@" logs --tail=250
    echo "operator restart did not advance the failed allocation" >&2
    exit 1
  fi
  sleep 1
done

docker unpause "$failure_worker_container" >/dev/null
failure_worker_paused=0

attempt=0
while :; do
  ready_after_restart=$(compose "$@" exec -T postgres psql -X -At \
    -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
    "SELECT COUNT(*) FROM allocations a JOIN services s ON s.service_id=a.service_id WHERE s.name='echo-api' AND a.desired_state='running' AND a.observed_state='running' AND a.endpoint IS NOT NULL;")
  if [ "$ready_after_restart" -eq 2 ]; then
    break
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 90 ]; then
    compose "$@" logs --tail=250
    echo "explicit restart did not recover both echo replicas" >&2
    exit 1
  fi
  sleep 1
done

# Regression: a drain requested while a worker process is unavailable must
# preserve the exact stop command. A rapid scale-up must wait for that stop to
# complete instead of creating a duplicate runtime on another worker.
drain_worker=$(compose "$@" exec -T postgres psql -X -At \
  -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
  "SELECT n.name FROM allocations a JOIN services s ON s.service_id=a.service_id JOIN nodes n ON n.node_id=a.node_id WHERE s.name='echo-api' AND a.desired_state='running' AND a.observed_state='running' AND a.endpoint IS NOT NULL AND n.status='ready' AND n.name IN ('worker-a','worker-b') ORDER BY n.name,a.replica LIMIT 1;")
case "$drain_worker" in
  worker-a|worker-b) ;;
  *)
    compose "$@" logs --tail=250
    echo "failed to select a ready echo allocation owner for the drain proof" >&2
    exit 1
    ;;
esac

drain_worker_container=$(compose "$@" ps -q "$drain_worker")
if [ -z "$drain_worker_container" ]; then
  echo "$drain_worker container was not found" >&2
  exit 1
fi
docker pause "$drain_worker_container" >/dev/null
drain_worker_paused=1

curl -fsS -X DELETE http://127.0.0.1:8090/api/v1/services/echo-api \
  -H "Authorization: Bearer $api_token" >/dev/null

attempt=0
while :; do
  durable_stop=$(compose "$@" exec -T postgres psql -X -At \
    -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
    "SELECT COUNT(*) FROM allocations a JOIN services s ON s.service_id=a.service_id JOIN nodes n ON n.node_id=a.node_id JOIN commands c ON c.allocation_id=a.allocation_id AND c.node_id=a.node_id WHERE s.name='echo-api' AND n.name='$drain_worker' AND n.status='not_ready' AND a.desired_state='stopped' AND a.observed_state='stopping' AND c.kind='stop' AND c.status IN ('pending','retry','leased') AND c.service_generation=a.service_generation AND c.allocation_revision=a.allocation_revision;")
  if [ "$durable_stop" -ge 1 ]; then
    break
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 30 ]; then
    compose "$@" logs --tail=250
    echo "drain stop command did not survive worker expiry" >&2
    exit 1
  fi
  sleep 1
done

curl -fsS -X POST http://127.0.0.1:8090/api/v1/services/echo-api/scale \
  -H "Authorization: Bearer $api_token" \
  -H 'content-type: application/json' \
  --data-binary '{"replicas":2}' >/dev/null
sleep 2

blocked_reactivation=$(compose "$@" exec -T postgres psql -X -At \
  -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
  "SELECT COUNT(*) FROM allocations a JOIN services s ON s.service_id=a.service_id JOIN nodes n ON n.node_id=a.node_id JOIN commands c ON c.allocation_id=a.allocation_id AND c.node_id=a.node_id WHERE s.name='echo-api' AND n.name='$drain_worker' AND a.desired_state='stopped' AND a.observed_state='stopping' AND c.kind='stop' AND c.status IN ('pending','retry','leased') AND c.service_generation=a.service_generation AND c.allocation_revision=a.allocation_revision;")
if [ "$blocked_reactivation" -lt 1 ]; then
  compose "$@" logs --tail=250
  echo "scale-up superseded an unacknowledged drain" >&2
  exit 1
fi

docker unpause "$drain_worker_container" >/dev/null
drain_worker_paused=0

attempt=0
while :; do
  ready_replicas=$(compose "$@" exec -T postgres psql -X -At \
    -v ON_ERROR_STOP=1 -U minicloud -d minicloud -c \
    "SELECT COUNT(*) FROM allocations a JOIN services s ON s.service_id=a.service_id WHERE s.name='echo-api' AND a.desired_state='running' AND a.observed_state='running' AND a.endpoint IS NOT NULL;")
  if [ "$ready_replicas" -eq 2 ]; then
    break
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 90 ]; then
    compose "$@" logs --tail=250
    echo "drained allocation did not stop and reactivate after worker recovery" >&2
    exit 1
  fi
  sleep 1
done

curl -fsS http://127.0.0.1:8090/api/v1/snapshot \
  -H "Authorization: Bearer $api_token"
printf '\n'
echo "MiniCloud end-to-end verification passed."

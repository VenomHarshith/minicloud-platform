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
cleanup() {
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
curl -fsS http://127.0.0.1:8090/api/v1/snapshot \
  -H "Authorization: Bearer $api_token"
printf '\n'
echo "MiniCloud end-to-end verification passed."

#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
env_file="$root/deploy/.env"
compose_file="$root/deploy/compose.yaml"

if [ ! -f "$env_file" ]; then
  echo "deploy/.env is missing; refusing to guess credentials or Compose state" >&2
  exit 1
fi

set -- --env-file "$env_file" -f "$compose_file"
if [ "$(uname -s)" = "Darwin" ] || [ "${MINICLOUD_COMPOSE_DESKTOP:-0}" = "1" ]; then
  set -- "$@" -f "$root/deploy/compose.desktop.yaml"
fi

compose() {
  docker compose "$@"
}

compose "$@" stop --timeout 10

container_ids=$(mktemp "${TMPDIR:-/tmp}/minicloud-stop.XXXXXX")
cleanup() {
  rm -f "$container_ids"
}
trap cleanup EXIT HUP INT TERM

docker ps --all --quiet \
  --filter label=io.minicloud.managed=true \
  --filter network=minicloud-workloads >"$container_ids"

while IFS= read -r container_id; do
  [ -n "$container_id" ] || continue
  case "$container_id" in
    *[!0-9a-f]*)
      echo "refusing unexpected Docker container ID: $container_id" >&2
      exit 1
      ;;
  esac
  if [ "${#container_id}" -lt 12 ] || [ "${#container_id}" -gt 64 ]; then
    echo "refusing unexpected Docker container ID length" >&2
    exit 1
  fi
  docker container stop --time 10 "$container_id" >/dev/null
  docker container rm "$container_id" >/dev/null
done <"$container_ids"

compose "$@" down --remove-orphans --timeout 10
echo "MiniCloud stopped; PostgreSQL and Prometheus volumes were preserved."

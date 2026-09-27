#!/usr/bin/env sh
set -eu

deploy_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
env_file="$deploy_dir/.env"

if [ -e "$env_file" ]; then
  echo "deploy/.env already exists; refusing to overwrite it" >&2
  exit 1
fi

umask 077
random_hex() {
  LC_ALL=C od -An -N32 -tx1 /dev/urandom | tr -d ' \n'
}
db_password=$(random_hex)
cluster_token=$(random_hex)
api_token=$(random_hex)
architecture=$(uname -m)
case "$architecture" in
  aarch64|arm64) architecture=arm64 ;;
  *) architecture=amd64 ;;
esac

sed \
  -e "s/replace-with-a-long-random-value/$db_password/" \
  -e "s/replace-with-a-second-long-random-value/$cluster_token/" \
  -e "s/replace-with-a-third-long-random-value/$api_token/" \
  -e "s/MINICLOUD_ARCH=arm64/MINICLOUD_ARCH=$architecture/" \
  "$deploy_dir/.env.example" > "$env_file"

echo "Created deploy/.env with owner-only local credentials."
echo "Next: docker compose --env-file deploy/.env -f deploy/compose.yaml up --build"

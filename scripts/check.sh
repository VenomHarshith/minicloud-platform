#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${MINICLOUD_BUILD_DIR:-"$root/build/dev"}

cmake -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMINICLOUD_BUILD_TESTS=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

docker compose --env-file "$root/deploy/.env.example" \
  -f "$root/deploy/compose.yaml" config --quiet

echo "C++ build, tests, and Compose validation passed."

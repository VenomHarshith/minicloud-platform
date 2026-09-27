#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project_name=minicloud-platform
version=0.1.0
dist_directory="$root/dist"
source_archive="$dist_directory/$project_name-$version-source.zip"
info_archive="$dist_directory/minicloud-information-pack-$version.zip"

if ! command -v zip >/dev/null 2>&1; then
  echo "zip is required to create release archives" >&2
  exit 1
fi

mkdir -p "$dist_directory"
rm -f "$source_archive" "$info_archive"

(
  cd "$(dirname "$root")"
  zip -rq "$source_archive" "$project_name" \
    -x "$project_name/.git/*" \
       "$project_name/deploy/.env" \
       "$project_name/build/*" \
       "$project_name/dist/*" \
       "$project_name/dashboard/node_modules/*" \
       "$project_name/dashboard/dist/*" \
       "$project_name/.cache/*" \
       "$project_name/*.log" \
       "$project_name/.DS_Store"
)

(
  cd "$root"
  zip -rq "$info_archive" \
    README.md RUN_INSTRUCTIONS.md MILESTONES.md PROGRESS.md CHANGELOG.md \
    SECURITY.md CONTRIBUTING.md LICENSE \
    information-pack docs
)

printf 'Created:\n  %s\n  %s\n' "$source_archive" "$info_archive"

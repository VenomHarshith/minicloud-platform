#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project_name=minicloud-platform

if ! command -v git >/dev/null 2>&1; then
  echo "git is required to create release archives" >&2
  exit 1
fi

version=$(
  git -C "$root" show HEAD:CMakeLists.txt 2>/dev/null \
    | sed -n 's/^project(MiniCloud VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\) LANGUAGES CXX)$/\1/p'
)
if [ -z "$version" ]; then
  echo "could not read the MiniCloud version from committed CMakeLists.txt" >&2
  exit 1
fi

dist_directory="$root/dist"
source_archive="$dist_directory/$project_name-$version-source.zip"
info_archive="$dist_directory/minicloud-information-pack-$version.zip"

for required_path in RELEASE_NOTES.md docs/COMPLETE_PROJECT_GUIDE.md; do
  if ! git -C "$root" cat-file -e "HEAD:$required_path" 2>/dev/null; then
    echo "$required_path is not committed at HEAD; commit release inputs before packaging" >&2
    exit 1
  fi
done

mkdir -p "$dist_directory"
rm -f "$source_archive" "$info_archive"

git -C "$root" archive \
  --format=zip \
  --prefix="$project_name/" \
  --output="$source_archive" \
  HEAD

git -C "$root" archive \
  --format=zip \
  --output="$info_archive" \
  HEAD -- \
  README.md RUN_INSTRUCTIONS.md RELEASE_NOTES.md MILESTONES.md PROGRESS.md CHANGELOG.md \
  SECURITY.md CONTRIBUTING.md LICENSE \
  information-pack docs

printf 'Created:\n  %s\n  %s\n' "$source_archive" "$info_archive"

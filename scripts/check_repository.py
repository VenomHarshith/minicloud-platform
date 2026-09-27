#!/usr/bin/env python3
"""Dependency-free repository/documentation contract checks."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parent.parent
IGNORED_PARTS = {".git", "build", "dist", "node_modules", ".cache"}
JSON_GLOBS = ("*.json",)
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
PRIVATE_PATH = re.compile(
    r"(?:/" + r"Users/[^/\s]+|/" + r"home/[^/\s]+|[A-Za-z]:\\Users\\[^\\\s]+)"
)
SECRET_PATTERNS = (
    re.compile(r"ghp_[A-Za-z0-9]{20,}"),
    re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
    re.compile(r"AKIA[0-9A-Z]{16}"),
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
)


def source_files() -> list[Path]:
    result: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or any(part in IGNORED_PARTS for part in path.parts):
            continue
        result.append(path)
    return result


def check_json(files: list[Path], failures: list[str]) -> None:
    for path in files:
        if not any(path.match(pattern) for pattern in JSON_GLOBS):
            continue
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            failures.append(f"invalid JSON: {path.relative_to(ROOT)}: {error}")


def clean_link_target(raw: str) -> str:
    target = raw.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    # An optional Markdown title follows the URL after whitespace.
    target = target.split(maxsplit=1)[0]
    return unquote(target.split("#", 1)[0])


def check_markdown(files: list[Path], failures: list[str]) -> None:
    for path in files:
        if path.suffix.lower() != ".md":
            continue
        text = path.read_text(encoding="utf-8")
        for raw in MARKDOWN_LINK.findall(text):
            target = clean_link_target(raw)
            if not target or target.startswith(("http://", "https://", "mailto:")):
                continue
            resolved = (path.parent / target).resolve()
            try:
                resolved.relative_to(ROOT.resolve())
            except ValueError:
                failures.append(
                    f"link escapes repository: {path.relative_to(ROOT)} -> {raw}"
                )
                continue
            if not resolved.exists():
                failures.append(
                    f"broken local link: {path.relative_to(ROOT)} -> {raw}"
                )


def check_hygiene(files: list[Path], failures: list[str]) -> None:
    for path in files:
        if path.suffix.lower() in {".zip", ".png", ".jpg", ".jpeg", ".gif"}:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        relative = path.relative_to(ROOT)
        private_path = PRIVATE_PATH.search(text)
        if private_path:
            failures.append(
                f"personal absolute path in {relative}: {private_path.group(0)}"
            )
        for pattern in SECRET_PATTERNS:
            if pattern.search(text):
                failures.append(f"possible credential in {relative}: {pattern.pattern}")


def main() -> int:
    files = source_files()
    failures: list[str] = []
    check_json(files, failures)
    check_markdown(files, failures)
    check_hygiene(files, failures)
    if failures:
        print("Repository contract checks failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print(
        f"Repository contract checks passed for {len(files)} source/document files."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# M6 — cross-platform validation and release

## Outcome required

Ship a reproducible release that a Windows, macOS, or Linux user can open in VS
Code, start with Docker, validate with a real workload, and package without
credentials or runtime data.

## Release gates

- [x] GCC/Clang/MSVC portable core builds and tests pass in CI.
- [x] Full dependency CMake build and runtime tests pass in the Docker builder.
- [x] React/TypeScript production build passes.
- [x] Compose configuration validates with generated credentials.
- [x] Ubuntu CI completes the real Docker deploy/health/discovery/gateway proof.
- [x] Windows PowerShell scripts parse and the MSVC core test passes.
- [x] Documentation links and commands are audited locally.
- [x] Source and information archives exclude `.env`, databases, logs, build
  output, dependency caches, and Git metadata.

## Supported workflows

- Windows: Docker Desktop + WSL 2 + Linux containers, driven by PowerShell.
- macOS: Docker Desktop with the raw-socket Compose override.
- Linux: Docker Engine + Compose v2.
- Native algorithm development: CMake/Ninja with MSVC, Clang, or GCC.

## Files to study

- `.github/workflows/ci.yml`
- `.vscode/`
- `RUN_INSTRUCTIONS.md`
- `docs/WINDOWS.md`
- `scripts/e2e.sh`
- `scripts/windows/`
- `docs/CLEAN_ROOM.md`

## Current status

Complete. The public workflow validates all portable compiler jobs, the
dashboard, both Compose models, the full dependency build, runtime tests, and a
real workload through the gateway. See `PROGRESS.md` for the verification
ledger and the public Actions link.

## Practical lesson

“Works on my machine” is not a release criterion. Portability covers compilers,
paths, scripts, transports, container architecture, documentation, and the
ability to reproduce proof from a clean checkout.

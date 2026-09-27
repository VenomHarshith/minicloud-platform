# Windows guide

## Supported path

Use Windows 11, Docker Desktop with WSL 2, and Linux containers. This runs the
same Linux workload images and Compose definition used on macOS/Linux while all
operator commands remain PowerShell-native.

Verify Docker before starting:

```powershell
docker version
docker compose version
docker info --format '{{.OSType}}'
```

The last command must print `linux`. If it prints `windows`, use Docker Desktop's
menu to switch to Linux containers.

## Start and verify

From the repository root:

```powershell
.\deploy\powershell\Initialize-MiniCloud.ps1
.\scripts\windows\Start-MiniCloud.ps1
.\scripts\windows\Deploy-Echo.ps1
```

Generate credentials only for a fresh installation. Preserve `deploy/.env`
while its PostgreSQL volume exists. If an older prerelease volume is below
schema version 4, use the PowerShell-compatible commands in
[OPERATIONS.md](OPERATIONS.md#upgrade-an-existing-prerelease-database) before
starting the full platform.

If local execution policy blocks a reviewed repository script, run that script
for the current process only:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
```

The start script adds `deploy/compose.desktop.yaml`. That override mounts Docker
Desktop's VM socket into each Linux worker. The Docker Engine endpoint inside the
worker remains `unix:///var/run/docker.sock`.

## Why the code also supports a Windows named pipe

A natively compiled worker can talk to Docker Desktop through:

```text
npipe:////./pipe/docker_engine
```

`cpp/runtime/docker_client.cpp` implements overlapped Win32 named-pipe I/O with
timeouts and bounded response parsing. The normal full-stack workflow still uses
Linux worker containers because the managed application images and resource
controls are Linux-container oriented.

Never enable unauthenticated `tcp://localhost:2375` as a shortcut. Docker daemon
access is equivalent to powerful host access.

## Native MSVC core build

Install Visual Studio Build Tools with Desktop development with C++, CMake, Git,
and vcpkg. Native builds currently require libpqxx 7.9.x; CMake rejects
libpqxx 8. The manifest does not pin a vcpkg registry baseline, so a fresh clone
of vcpkg's current branch is not guaranteed to be compatible. Prefer the Docker
workflow, or prepare a vcpkg checkout whose `libpqxx` port resolves to 7.9.x.
Then, in an x64 Native Tools PowerShell:

```powershell
$env:VCPKG_ROOT = "C:\path\to\compatible-vcpkg-checkout"
& "$env:VCPKG_ROOT\bootstrap-vcpkg.bat"

cmake -S . -B build\windows -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DMINICLOUD_BUILD_TESTS=ON
cmake --build build\windows --config Debug
ctest --test-dir build\windows -C Debug --output-on-failure
```

This downloads the free C++ packages listed in `vcpkg.json` from the selected
vcpkg checkout. Docker builds do not require native Visual Studio or vcpkg.

## Resource settings

Docker Desktop runs containers in its WSL 2 VM. MiniCloud's two logical workers
advertise 4 CPU cores and 4 GiB each for scheduling exercises; this is logical
capacity, not extra physical capacity. Give Docker at least 6 GiB for a smooth
build, or lower both worker values in a personal Compose override.

Docker enforces each workload's CPU/RAM limits inside the VM. Windows Task
Manager may show VM-level aggregation rather than one native process per Linux
container.

## File placement and performance

For best Docker bind/build performance, clone the repository inside the WSL 2
Linux filesystem and open it using VS Code's WSL integration. Running from an
ordinary Windows path is supported but large rebuilds can be slower.

## Windows troubleshooting

- Named pipe access denied: ensure your account is in `docker-users`, sign out,
  and sign back in.
- `docker info` cannot connect: start Docker Desktop and wait for the engine.
- Socket mount missing: confirm the desktop Compose override is present in the
  command and Docker Desktop is using Linux containers.
- `exec format error`: the workload image lacks the platform matching Docker
  Desktop's VM; build/pull a multi-platform image.
- PowerShell says a script is unsigned: use process-scoped policy only after
  reviewing the checked-in script; do not weaken machine-wide policy.
- Port conflict: create an uncommitted Compose override with another host-side
  port while leaving container ports unchanged.

Stop without deleting volumes:

```powershell
.\scripts\windows\Stop-MiniCloud.ps1
```

The script stops and removes only MiniCloud-labeled containers on the
`minicloud-workloads` network before bringing down Compose. This lets Docker
remove the network while preserving PostgreSQL and Prometheus named volumes.

## Share a read-only public observer

Do not expose the dashboard created by `Start-MiniCloud.ps1`; it is the private
administrative build. The safe public path requires the desktop and public
Compose overlays in that order and publishes only a sanitized snapshot through
a temporary tunnel. Follow the PowerShell procedure and checks in
[PUBLIC_OBSERVER.md](PUBLIC_OBSERVER.md).

While that tunnel is open, do not rerun `Start-MiniCloud.ps1` or another start
command that omits `compose.public.yaml`. Close public access first. The public
overlay's worker-only Podman SELinux option also requires a live validation on
the target Docker Desktop installation before the URL is shared.

# VS Code workflow

VS Code is an interface for the same repository commands; it does not hide the
platform. Open the whole `minicloud-platform` folder so `${workspaceFolder}` is
the repository root.

## Recommended extensions

When VS Code offers workspace recommendations, install:

- C/C++ for navigation and native debugging;
- CMake Tools for C++ presets;
- Docker for container, image, and Compose inspection;
- YAML for workflow and Compose validation.

They improve the editor experience but are not required by the containerized
run path.

## First run

Open **Terminal → Run Task** and run these in order:

1. **MiniCloud: Verify Docker** — confirms the engine and Compose plugin.
2. **MiniCloud: Initialize credentials (once)** — creates ignored
   `deploy/.env`; it refuses to overwrite an existing file.
3. **MiniCloud: Start platform** — selects the Docker Desktop override on
   macOS/Windows and the base Compose model on Linux.
4. **MiniCloud: Show platform status** — shows health and restart state.
5. **MiniCloud: Run full E2E demo** — builds and deploys the real echo image,
   waits for routing, and fails if the complete path does not converge.

Open the dashboard at <http://127.0.0.1:3000> after the start task.

## While learning the code

Use a split editor and follow one request in this order:

1. `dashboard/src/api.ts` or `cpp/cli/main.cpp`;
2. `cpp/controller/api.cpp`;
3. `cpp/controller/repository.cpp` and `database/migrations/001_initial.sql`;
4. `cpp/controller/grpc_service.cpp` and `proto/controller_worker.proto`;
5. `cpp/worker/main.cpp`;
6. `cpp/runtime/worker_runtime.cpp` and `docker_client.cpp`;
7. `cpp/controller/valkey_store.cpp`;
8. `cpp/gateway/proxy.cpp`.

Keep `docs/BUILD_AND_LEARN.md` open beside the code. The milestone files map
each subsystem to its invariants and verification exercise.

## Useful development tasks

- **MiniCloud: Follow platform logs** — combined container logs.
- **MiniCloud: Dashboard dev server** — Vite frontend development; start the
  controller stack first for its `/api` proxy.
- **MiniCloud: Dashboard build** — TypeScript plus production Vite build.
- **MiniCloud: Native configure/build/test** — CMake path for a machine with the
  native dependencies installed.
- **MiniCloud: Validate Compose** — catches interpolation and merge errors.

Use the CMake task only when you intentionally installed native dependencies.
The normal Docker path does not require local CMake, Node, PostgreSQL, Valkey,
gRPC, or Prometheus packages.

## Stop

Run **MiniCloud: Stop platform**. It preserves PostgreSQL and Prometheus named
volumes. Do not manually delete volumes while debugging unless you have decided
to discard the local state.

## Debugging approach

Start from the failed invariant rather than opening every log:

- no service record → REST/controller/database;
- pending allocation → node labels or CPU/RAM scheduling;
- leased command → gRPC/worker connectivity;
- container missing/unhealthy → worker/runtime/Docker health;
- ready allocation but gateway 503 → endpoint publication/Valkey TTL;
- gateway 502 → workload network or upstream process;
- UI stale but API correct → dashboard proxy/browser.

That fault tree is the same reasoning used in larger production platforms.

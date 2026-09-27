# MiniCloud information pack

This is the reading order for understanding, running, and extending the actual
MiniCloud product. `scripts/package-release.sh` and
`scripts/windows/Package-Release.ps1` collect these canonical files into a
documentation-only ZIP, so the explanations do not drift into duplicate copies.

## Start here

1. `docs/COMPLETE_PROJECT_GUIDE.md` — the complete product, architecture,
   technology, operation, troubleshooting, cost, and limitation reference.
2. `README.md` — product purpose, capabilities, architecture, and boundaries.
3. `RUN_INSTRUCTIONS.md` — exact Windows/macOS/Linux operating procedure.
4. `RELEASE_NOTES.md` — download contents, verification, and hosting boundary.
5. `MILESTONES.md` — ordered build stages.
6. `PROGRESS.md` — implementation and verification ledger.
7. `docs/BUILD_AND_LEARN.md` — what, why, when, and how for every technology.
8. `SECURITY.md` and `CONTRIBUTING.md` — safe operation and extension rules.

## Deep design

1. `docs/ARCHITECTURE.md`
2. `docs/adr/0001-durable-and-ephemeral-state.md`
3. `docs/adr/0002-at-least-once-and-fencing.md`
4. `docs/adr/0003-docker-socket-trust-boundary.md`
5. `docs/THREAT_MODEL.md`
6. `docs/API.md`

## Operate and troubleshoot

1. `docs/WINDOWS.md`
2. `docs/VSCODE.md`
3. `docs/OPERATIONS.md`
4. `docs/DEPENDENCIES.md`
5. `docs/ONLINE_DEPLOYMENT.md`
6. `docs/CLEAN_ROOM.md`

## Follow the implementation

Read `docs/milestones/M0_...` through `M6_...` in numeric order. Each file
states the outcome, why it matters, implementation files, verification, and a
hands-on extension.

No private employer/customer information belongs in this pack.
Generated archives also exclude credentials, databases, logs, build output,
dependency caches, and Git metadata.

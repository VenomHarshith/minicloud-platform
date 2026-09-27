# M7 — safe public observer

## Outcome required

Let another person inspect live state from the real MiniCloud control plane
without publishing the administrative dashboard, controller, workload gateway,
metrics, databases, gRPC channel, Docker socket, logs, secrets, or write routes.

## Delivery gates

- [x] A dedicated React build removes administrative/log controls.
- [x] Public Nginx allowlists only the exact snapshot read route.
- [x] A separate facade constructs a sanitized response from explicit fields.
- [x] Facade and tunnel containers run unprivileged without Docker access.
- [x] Focused tests prove representative private fields are removed.
- [x] The local edge serves live state and blocks mutation paths.
- [x] Linux, macOS, Windows, cost, OCI, verification, and shutdown guidance is
  recorded in `docs/PUBLIC_OBSERVER.md`.

Starting a Quick Tunnel is a deliberate per-session operator action after the
local checks pass. Its random URL is temporary and is never treated as a stable
release artifact. OCI is documented as an optional operator-managed VM path,
not as an automatically provisioned or guaranteed-free deployment.

## Architecture decision

The public browser never calls the controller. It reaches public Nginx, which
can call only the facade's snapshot endpoint. The facade authenticates to the
controller on the Compose network and produces a smaller public read model.
This is a backend-for-frontend boundary, not a frontend-only permission check.

The facade currently holds the full controller token internally because v0.1
does not issue scoped credentials. Server routing, response construction, and
container confinement reduce exposure, but this residual risk prevents a
production-security claim.

## Files to study

- `deploy/compose.public.yaml`
- `deploy/public_api.py`
- `dashboard/nginx.public.conf`
- `dashboard/Dockerfile`
- `dashboard/src/App.tsx`
- `tests/test_public_api.py`
- `docs/PUBLIC_OBSERVER.md`
- `docs/THREAT_MODEL.md`

## Verification

Run the sanitization proof:

```bash
python3 -m unittest tests/test_public_api.py
```

Then follow the local page, snapshot, and blocked-write checks in
`docs/PUBLIC_OBSERVER.md`. Open the tunnel only after those pass, and repeat the
same checks through the generated HTTPS origin before sharing it.

## Practical lesson

A public status view should not be made by hiding admin buttons or forwarding a
large internal response. The safer pattern uses an explicit data
classification, allowlisted route, smaller public contract, private upstream
credential, overlapping enforcement layers, and an operational rule that
closes ingress before any rebuild that could change the edge configuration.

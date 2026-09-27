# M5 — observability and operations dashboard

## Outcome

Built Prometheus metrics and a React/TypeScript operator console that exposes
desired state, observed state, placement, capacity, events, logs, and mutations.

## What was built

- Fixed-cardinality controller, worker, gateway, command, restart, and request
  metrics.
- A bounded snapshot API for coherent operator views.
- Service convergence, node capacity, allocation, event, and log panels.
- Deploy, scale, and restart controls through an authenticated same-origin
  Nginx proxy.
- Responsive layout usable on laptop and smaller screens.

## Why

Metrics answer aggregate questions, events explain control-plane decisions, and
logs provide workload detail. The UI connects those signals to desired versus
observed state instead of exposing disconnected raw tables.

## Files to study

- `cpp/common/metrics.cpp`
- `cpp/runtime/metrics.cpp`
- `deploy/prometheus/prometheus.yml`
- `dashboard/src/`
- `dashboard/nginx.conf`

## Verification

Deploy, scale, crash, and restart the echo service. Correlate one operation from
the dashboard to its event, allocation, worker logs, gateway result, and
Prometheus counters.

## Practical lesson

High-cardinality identity belongs in logs and events, not metric labels. An
operator UI should reveal causality and convergence, not only current values.

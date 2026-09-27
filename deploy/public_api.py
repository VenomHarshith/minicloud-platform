"""Read-only, sanitized facade for the public MiniCloud observer dashboard."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


CONTROLLER_URL = os.environ.get("MINICLOUD_CONTROLLER_URL", "http://controller:8090")
API_TOKEN = os.environ["MINICLOUD_API_TOKEN"]
MAX_UPSTREAM_BYTES = 1_000_000


def _select(source: dict[str, Any], fields: tuple[str, ...]) -> dict[str, Any]:
    return {field: source.get(field) for field in fields}


def sanitize_snapshot(source: dict[str, Any]) -> dict[str, Any]:
    """Keep dashboard-safe operational fields and replace internal identities."""
    overview = _select(
        source.get("overview", {}),
        (
            "healthy",
            "services",
            "desiredReplicas",
            "readyReplicas",
            "readyNodes",
            "totalNodes",
            "pendingAllocations",
            "restartCount",
        ),
    )
    overview["schedulerLeader"] = "controller"

    services = []
    for service in source.get("services", []):
        if not isinstance(service, dict):
            continue
        public = _select(
            service,
            (
                "name",
                "image",
                "desiredReplicas",
                "readyReplicas",
                "cpuMillis",
                "memoryMb",
                "containerPort",
                "healthPath",
                "generation",
                "createdAt",
            ),
        )
        public["id"] = f"service:{service.get('name', 'unknown')}"
        services.append(public)

    nodes = []
    for node in source.get("nodes", []):
        if not isinstance(node, dict):
            continue
        name = str(node.get("name", "worker"))
        public = _select(
            node,
            (
                "name",
                "status",
                "cpuCapacityMillis",
                "cpuAllocatedMillis",
                "memoryCapacityMb",
                "memoryAllocatedMb",
                "lastHeartbeat",
            ),
        )
        labels = node.get("labels", {})
        public["labels"] = {
            key: labels[key]
            for key in ("zone", "arch", "os")
            if isinstance(labels, dict) and isinstance(labels.get(key), str)
        }
        public["id"] = f"node:{name}"
        nodes.append(public)

    allocations = []
    for allocation in source.get("allocations", []):
        if not isinstance(allocation, dict):
            continue
        service_name = str(allocation.get("serviceName", "service"))
        replica = allocation.get("replica", 0)
        public = _select(
            allocation,
            ("serviceName", "replica", "nodeName", "state", "restartCount", "updatedAt"),
        )
        public.update(
            {
                "id": f"allocation:{service_name}:{replica}",
                "containerId": None,
                "endpoint": None,
            }
        )
        allocations.append(public)

    events = []
    for event in source.get("events", [])[:50]:
        if not isinstance(event, dict):
            continue
        event_type = str(event.get("type", "platform.event"))
        events.append(
            {
                "id": event.get("id", 0),
                "type": event_type,
                "aggregateId": "public",
                "message": event_type.replace(".", " "),
                "createdAt": event.get("createdAt", ""),
            }
        )

    return {
        "overview": overview,
        "services": services,
        "nodes": nodes,
        "allocations": allocations,
        "events": events,
    }


def fetch_snapshot() -> dict[str, Any]:
    request = urllib.request.Request(
        f"{CONTROLLER_URL}/api/v1/snapshot",
        headers={"Authorization": f"Bearer {API_TOKEN}", "Accept": "application/json"},
        method="GET",
    )
    with urllib.request.urlopen(request, timeout=3) as response:
        payload = response.read(MAX_UPSTREAM_BYTES + 1)
    if len(payload) > MAX_UPSTREAM_BYTES:
        raise ValueError("controller response exceeded the public facade limit")
    decoded = json.loads(payload)
    if not isinstance(decoded, dict):
        raise ValueError("controller returned a non-object snapshot")
    return sanitize_snapshot(decoded)


class Handler(BaseHTTPRequestHandler):
    server_version = "MiniCloudPublicObserver"
    sys_version = ""

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = self.path.split("?", 1)[0]
        if path == "/healthz":
            self._write(200, b"ok\n", "text/plain; charset=utf-8")
            return
        if path != "/api/v1/snapshot":
            self._json_error(404, "not found")
            return
        try:
            body = json.dumps(fetch_snapshot(), separators=(",", ":")).encode()
            self._write(200, body, "application/json")
        except (OSError, ValueError, json.JSONDecodeError, urllib.error.URLError):
            self._json_error(503, "live snapshot temporarily unavailable")

    def do_HEAD(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path.split("?", 1)[0] == "/healthz":
            self._write(200, b"", "text/plain; charset=utf-8")
        else:
            self._json_error(404, "not found")

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._json_error(405, "public observer is read-only")

    do_PUT = do_POST
    do_PATCH = do_POST
    do_DELETE = do_POST

    def _json_error(self, status: int, message: str) -> None:
        self._write(status, json.dumps({"error": message}).encode(), "application/json")

    def _write(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        return


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", 8081), Handler).serve_forever()

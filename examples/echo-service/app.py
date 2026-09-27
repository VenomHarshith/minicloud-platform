from __future__ import annotations

import json
import os
import signal
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


STARTED_AT = time.time()
INSTANCE = os.environ.get("MINICLOUD_ALLOCATION_ID", "standalone")
PORT = int(os.environ.get("PORT", "8080"))


class Handler(BaseHTTPRequestHandler):
    server_version = "MiniCloudEcho/1.0"

    def do_GET(self) -> None:
        if self.path == "/health":
            self._json(200, {"status": "ok"})
            return
        if self.path == "/ready":
            self._json(200, {"ready": True})
            return
        if self.path == "/crash":
            self._json(202, {"message": "intentional crash requested"})
            os.kill(os.getpid(), signal.SIGTERM)
            return
        if self.path == "/" or self.path.startswith("/echo"):
            self._json(
                200,
                {
                    "message": "Hello from a real MiniCloud-managed container",
                    "allocation": INSTANCE,
                    "hostname": os.environ.get("HOSTNAME", "unknown"),
                    "uptime_seconds": round(time.time() - STARTED_AT, 3),
                    "path": self.path,
                },
            )
            return
        self._json(404, {"error": "not found"})

    def log_message(self, format: str, *args: object) -> None:
        print(
            json.dumps(
                {
                    "level": "info",
                    "event": "http_request",
                    "client": self.client_address[0],
                    "message": format % args,
                }
            ),
            flush=True,
        )

    def _json(self, status: int, body: dict[str, object]) -> None:
        payload = json.dumps(body, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main() -> None:
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.daemon_threads = True
    print(json.dumps({"level": "info", "event": "started", "port": PORT}), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()

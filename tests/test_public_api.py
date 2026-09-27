import importlib.util
import os
import unittest
from pathlib import Path


os.environ.setdefault("MINICLOUD_API_TOKEN", "t" * 32)
MODULE_PATH = Path(__file__).parents[1] / "deploy" / "public_api.py"
SPEC = importlib.util.spec_from_file_location("minicloud_public_api", MODULE_PATH)
assert SPEC and SPEC.loader
PUBLIC_API = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PUBLIC_API)


class SanitizeSnapshotTests(unittest.TestCase):
    def test_removes_secrets_and_internal_identifiers(self) -> None:
        source = {
            "overview": {"healthy": True, "services": 1, "schedulerLeader": "secret-host"},
            "services": [{
                "id": "internal-service-id",
                "name": "echo-api",
                "image": "minicloud/echo-service:local",
                "desiredReplicas": 1,
                "readyReplicas": 1,
                "cpuMillis": 100,
                "memoryMb": 64,
                "containerPort": 8080,
                "healthPath": "/health",
                "generation": 1,
                "createdAt": "2026-09-27T00:00:00Z",
                "environment": {"PASSWORD": "must-not-leak"},
            }],
            "nodes": [{
                "id": "internal-node-id",
                "name": "worker-a",
                "status": "ready",
                "labels": {"zone": "demo-a", "private-host": "must-not-leak"},
                "dockerVersion": "private-version",
            }],
            "allocations": [{
                "id": "internal-allocation-id",
                "serviceName": "echo-api",
                "replica": 0,
                "nodeName": "worker-a",
                "state": "running",
                "containerId": "must-not-leak",
                "endpoint": "must-not-leak",
                "lastError": "must-not-leak",
                "restartCount": 0,
                "updatedAt": "2026-09-27T00:00:00Z",
            }],
            "events": [{
                "id": 7,
                "type": "allocation.ready",
                "aggregateId": "must-not-leak",
                "message": "must-not-leak",
                "payload": {"secret": "must-not-leak"},
                "createdAt": "2026-09-27T00:00:00Z",
            }],
        }

        result = PUBLIC_API.sanitize_snapshot(source)
        encoded = str(result)

        self.assertNotIn("must-not-leak", encoded)
        self.assertNotIn("secret-host", encoded)
        self.assertNotIn("internal-", encoded)
        self.assertEqual(result["overview"]["schedulerLeader"], "controller")
        self.assertEqual(result["services"][0]["id"], "service:echo-api")
        self.assertEqual(result["nodes"][0]["labels"], {"zone": "demo-a"})
        self.assertIsNone(result["allocations"][0]["containerId"])
        self.assertIsNone(result["allocations"][0]["endpoint"])
        self.assertEqual(result["events"][0]["aggregateId"], "public")


if __name__ == "__main__":
    unittest.main()

# Echo service

This is a deliberately small **real container workload** used to verify the
complete platform path: image build, API submission, scheduling, gRPC command,
Docker execution, health, discovery, gateway load balancing, logs, metrics,
restart, and scale-down.

After the cluster is running, build and submit it from the repository root.

macOS/Linux:

```bash
docker build -t minicloud/echo-service:dev examples/echo-service
curl -fsS -X POST http://127.0.0.1:8090/api/v1/services \
  -H "Authorization: Bearer $(awk -F= '/^MINICLOUD_API_TOKEN=/{print $2}' deploy/.env)" \
  -H 'content-type: application/json' \
  --data-binary @examples/echo-service/service.json
curl -fsS http://127.0.0.1:8080/services/echo-api/
```

PowerShell:

```powershell
docker build -t minicloud/echo-service:dev examples/echo-service
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8090/api/v1/services `
  -Headers @{ Authorization = "Bearer $((Get-Content deploy/.env | Where-Object { $_ -like 'MINICLOUD_API_TOKEN=*' }).Split('=', 2)[1])" } `
  -ContentType application/json `
  -InFile examples/echo-service/service.json
Invoke-RestMethod http://127.0.0.1:8080/services/echo-api/
```

Open `http://127.0.0.1:3000` to scale, restart, and inspect logs. Request
`/crash` through the gateway to exercise automatic recovery.

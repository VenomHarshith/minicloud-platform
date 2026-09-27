[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$EnvironmentFile = Join-Path $Root 'deploy\.env'
if (-not (Test-Path $EnvironmentFile)) { throw 'Run Start-MiniCloud.ps1 first.' }

$TokenLine = Get-Content $EnvironmentFile | Where-Object { $_ -like 'MINICLOUD_API_TOKEN=*' } | Select-Object -First 1
if (-not $TokenLine) { throw 'MINICLOUD_API_TOKEN is missing from deploy/.env.' }
$Token = $TokenLine.Split('=', 2)[1]
$Headers = @{ Authorization = "Bearer $Token" }

docker build -t minicloud/echo-service:dev (Join-Path $Root 'examples\echo-service')
if ($LASTEXITCODE -ne 0) { throw 'Echo image build failed.' }

$Deadline = (Get-Date).AddMinutes(3)
while ((Get-Date) -lt $Deadline) {
    try {
        Invoke-RestMethod http://127.0.0.1:8090/health | Out-Null
        break
    } catch {
        Start-Sleep -Seconds 2
    }
}

try {
    Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8090/api/v1/services `
        -Headers $Headers -ContentType application/json `
        -InFile (Join-Path $Root 'examples\echo-service\service.json') | Out-Host
} catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 409) { throw }
    Write-Host 'echo-api already exists; continuing with readiness check.'
}

$Deadline = (Get-Date).AddMinutes(4)
while ((Get-Date) -lt $Deadline) {
    try {
        $Response = Invoke-RestMethod http://127.0.0.1:8080/services/echo-api/
        $Response | ConvertTo-Json -Depth 8
        Write-Host 'MiniCloud end-to-end verification passed.'
        exit 0
    } catch {
        Start-Sleep -Seconds 2
    }
}
throw 'echo-api did not become routable before the deadline.'

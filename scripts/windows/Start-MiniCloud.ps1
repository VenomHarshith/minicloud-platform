[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$EnvironmentFile = Join-Path $Root 'deploy\.env'
$ComposeFile = Join-Path $Root 'deploy\compose.yaml'
$DesktopOverride = Join-Path $Root 'deploy\compose.desktop.yaml'

docker version | Out-Null
docker info --format '{{.OSType}}' | ForEach-Object {
    if ($_ -ne 'linux') { throw 'Switch Docker Desktop to Linux containers before starting MiniCloud.' }
}

if (-not (Test-Path $EnvironmentFile)) {
    & (Join-Path $Root 'deploy\powershell\Initialize-MiniCloud.ps1')
}

$Arguments = @('compose', '--env-file', $EnvironmentFile, '-f', $ComposeFile, '-f', $DesktopOverride, 'up', '-d')
if (-not $SkipBuild) { $Arguments += '--build' }
& docker @Arguments
if ($LASTEXITCODE -ne 0) { throw 'Docker Compose failed to start MiniCloud.' }

Write-Host ''
Write-Host 'MiniCloud is starting:'
Write-Host '  Dashboard   http://127.0.0.1:3000'
Write-Host '  Gateway     http://127.0.0.1:8080'
Write-Host '  API         http://127.0.0.1:8090'
Write-Host '  Prometheus  http://127.0.0.1:9090'
Write-Host ''
Write-Host 'Run .\scripts\windows\Deploy-Echo.ps1 for the full workload proof.'

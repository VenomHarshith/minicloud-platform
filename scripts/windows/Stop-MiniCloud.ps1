[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$EnvironmentFile = Join-Path $Root 'deploy\.env'
if (-not (Test-Path $EnvironmentFile)) {
    throw 'deploy/.env is missing; refusing to guess credentials or Compose state.'
}

$ComposeArguments = @(
    'compose', '--env-file', $EnvironmentFile,
    '-f', (Join-Path $Root 'deploy\compose.yaml'),
    '-f', (Join-Path $Root 'deploy\compose.desktop.yaml')
)

& docker @ComposeArguments stop --timeout 10
if ($LASTEXITCODE -ne 0) { throw 'Docker Compose failed to stop MiniCloud services.' }

$ManagedContainers = @(& docker ps --all --quiet `
    --filter 'label=io.minicloud.managed=true' `
    --filter 'network=minicloud-workloads')
if ($LASTEXITCODE -ne 0) { throw 'Unable to enumerate MiniCloud workload containers.' }

foreach ($ContainerIdValue in $ManagedContainers) {
    $ContainerId = $ContainerIdValue.Trim()
    if (-not $ContainerId) { continue }
    if ($ContainerId -notmatch '^[0-9a-f]{12,64}$') {
        throw "Refusing unexpected Docker container ID: $ContainerId"
    }
    & docker container stop --time 10 $ContainerId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to stop managed workload container $ContainerId." }
    & docker container rm $ContainerId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to remove managed workload container $ContainerId." }
}

& docker @ComposeArguments down --remove-orphans --timeout 10
if ($LASTEXITCODE -ne 0) { throw 'Docker Compose failed to remove MiniCloud services.' }
Write-Host 'MiniCloud stopped; PostgreSQL and Prometheus volumes were preserved.'

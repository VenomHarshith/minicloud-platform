[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$DeployDirectory = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$EnvironmentFile = Join-Path $DeployDirectory '.env'

if (Test-Path $EnvironmentFile) {
    throw 'deploy/.env already exists; refusing to overwrite credentials for a potentially initialized database.'
}

function New-HexSecret {
    $bytes = New-Object byte[] 32
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $generator.GetBytes($bytes)
    }
    finally {
        $generator.Dispose()
    }
    return ([BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
}

$Architecture = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'amd64' }
$Content = @"
MINICLOUD_DB_PASSWORD=$(New-HexSecret)
MINICLOUD_CLUSTER_TOKEN=$(New-HexSecret)
MINICLOUD_API_TOKEN=$(New-HexSecret)
MINICLOUD_DOCKER_SOCKET=/var/run/docker.sock
MINICLOUD_DOCKER_ENDPOINT=unix:///var/run/docker.sock
MINICLOUD_ARCH=$Architecture
"@

[IO.File]::WriteAllText($EnvironmentFile, $Content, [Text.UTF8Encoding]::new($false))
Write-Host "Created deploy/.env with local random credentials."
Write-Host "Next: docker compose --env-file deploy/.env -f deploy/compose.yaml up --build"

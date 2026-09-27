[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Version = '0.1.0'
$Dist = Join-Path $Root 'dist'
$TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("minicloud-package-" + [Guid]::NewGuid().ToString('N'))
$SourceStage = Join-Path $TemporaryRoot 'minicloud-platform'
$InfoStage = Join-Path $TemporaryRoot 'minicloud-information-pack'

New-Item -ItemType Directory -Force -Path $Dist, $SourceStage, $InfoStage | Out-Null

try {
    $ExcludedDirectories = @('.git', 'build', 'dist', 'node_modules', '.cache')
    $ExcludedFiles = @('.env', '.DS_Store')

    Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
        $Relative = $_.FullName.Substring($Root.Length).TrimStart('\', '/')
        $Parts = $Relative -split '[\\/]'
        -not ($Parts | Where-Object { $_ -in $ExcludedDirectories }) -and
        $_.Name -notin $ExcludedFiles -and $_.Extension -ne '.log'
    } | ForEach-Object {
        $Relative = $_.FullName.Substring($Root.Length).TrimStart('\', '/')
        $Destination = Join-Path $SourceStage $Relative
        New-Item -ItemType Directory -Force -Path (Split-Path $Destination) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $Destination
    }

    $TopLevelDocuments = @(
        'README.md', 'RUN_INSTRUCTIONS.md', 'MILESTONES.md', 'PROGRESS.md',
        'CHANGELOG.md', 'SECURITY.md', 'CONTRIBUTING.md', 'LICENSE'
    )
    foreach ($Name in $TopLevelDocuments) {
        Copy-Item -LiteralPath (Join-Path $Root $Name) -Destination $InfoStage
    }
    Copy-Item -Recurse -LiteralPath (Join-Path $Root 'docs') -Destination $InfoStage
    Copy-Item -Recurse -LiteralPath (Join-Path $Root 'information-pack') -Destination $InfoStage

    $SourceArchive = Join-Path $Dist "minicloud-platform-$Version-source.zip"
    $InfoArchive = Join-Path $Dist "minicloud-information-pack-$Version.zip"
    Compress-Archive -Path $SourceStage -DestinationPath $SourceArchive -Force
    Compress-Archive -Path (Join-Path $InfoStage '*') -DestinationPath $InfoArchive -Force
    Write-Host "Created:`n  $SourceArchive`n  $InfoArchive"
}
finally {
    if (Test-Path $TemporaryRoot) { Remove-Item -Recurse -Force -LiteralPath $TemporaryRoot }
}

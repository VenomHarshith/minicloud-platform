[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Dist = Join-Path $Root 'dist'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git is required to create release archives'
}

$CMakeLists = (& git -C $Root show 'HEAD:CMakeLists.txt' 2>$null) -join "`n"
if ($LASTEXITCODE -ne 0 -or
    $CMakeLists -notmatch '(?m)^project\(MiniCloud VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$') {
    throw 'could not read the MiniCloud version from committed CMakeLists.txt'
}
$Version = $Matches[1]
$SourceArchive = Join-Path $Dist "minicloud-platform-$Version-source.zip"
$InfoArchive = Join-Path $Dist "minicloud-information-pack-$Version.zip"

foreach ($RequiredPath in @('RELEASE_NOTES.md', 'docs/COMPLETE_PROJECT_GUIDE.md')) {
    & git -C $Root cat-file -e "HEAD`:$RequiredPath" 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "$RequiredPath is not committed at HEAD; commit release inputs before packaging"
    }
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $SourceArchive, $InfoArchive

$SourceArguments = @(
    '-C', $Root, 'archive', '--format=zip', '--prefix=minicloud-platform/',
    "--output=$SourceArchive", 'HEAD'
)
& git @SourceArguments
if ($LASTEXITCODE -ne 0) {
    throw 'git archive failed while creating the source archive'
}

$InformationPaths = @(
    'README.md', 'RUN_INSTRUCTIONS.md', 'RELEASE_NOTES.md', 'MILESTONES.md',
    'PROGRESS.md', 'CHANGELOG.md', 'SECURITY.md', 'CONTRIBUTING.md', 'LICENSE',
    'information-pack', 'docs'
)
$InformationArguments = @(
    '-C', $Root, 'archive', '--format=zip', "--output=$InfoArchive", 'HEAD', '--'
) + $InformationPaths
& git @InformationArguments
if ($LASTEXITCODE -ne 0) {
    throw 'git archive failed while creating the information archive'
}

Write-Host "Created:`n  $SourceArchive`n  $InfoArchive"

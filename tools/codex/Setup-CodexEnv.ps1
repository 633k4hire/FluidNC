param(
    [switch] $SkipToolchain,
    [switch] $SkipNodeInstall,
    [switch] $SkipPlatformIOProbe
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir '..\..')
$toolRoot = Join-Path $repoRoot '.codex-tools'
$cacheRoot = Join-Path $toolRoot 'cache'
$w64Root = Join-Path $toolRoot 'w64devkit'

New-Item -ItemType Directory -Force -Path $toolRoot, $cacheRoot | Out-Null

if (-not $SkipToolchain -and -not (Test-Path -LiteralPath (Join-Path $w64Root 'bin\g++.exe'))) {
    Write-Host "Installing repo-local w64devkit toolchain..."
    $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/skeeto/w64devkit/releases/latest'
    $asset = $release.assets | Where-Object { $_.name -like 'w64devkit-x64-*.7z.exe' } | Select-Object -First 1
    if (-not $asset) {
        throw 'Could not find a w64devkit x64 release asset.'
    }

    $archive = Join-Path $cacheRoot $asset.name
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive
    }

    $extractRoot = Join-Path $toolRoot 'extract'
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null

    & $archive -y "-o$extractRoot" | Out-Host
    $extracted = Get-ChildItem -LiteralPath $extractRoot -Directory | Where-Object { $_.Name -eq 'w64devkit' } | Select-Object -First 1
    if (-not $extracted) {
        throw 'w64devkit archive did not contain the expected w64devkit directory.'
    }
    Move-Item -LiteralPath $extracted.FullName -Destination $w64Root
    Remove-Item -LiteralPath $extractRoot -Recurse -Force
}

. (Join-Path $scriptDir 'Enter-CodexEnv.ps1') -Quiet

if (-not $SkipNodeInstall) {
    Write-Host "Installing embedded WebUI npm dependencies..."
    $npm = 'C:\Program Files\nodejs\npm.cmd'
    if (-not (Test-Path -LiteralPath $npm)) {
        $npm = 'npm.cmd'
    }
    Push-Location (Join-Path $repoRoot 'embedded')
    try {
        & $npm install
    } finally {
        Pop-Location
    }
}

if (-not $SkipPlatformIOProbe) {
    Write-Host "Checking PlatformIO with repo-local core directory..."
    pio system info
}

Write-Host "Codex FluidNC build environment is ready."
Write-Host "Use: . .\tools\codex\Enter-CodexEnv.ps1"

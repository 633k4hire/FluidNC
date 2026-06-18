param(
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir '..\..')
$toolRoot = Join-Path $repoRoot '.codex-tools'
$pioCore = Join-Path $repoRoot '.pio-core'
$w64Root = Join-Path $toolRoot 'w64devkit'
$embeddedBin = Join-Path $repoRoot 'embedded\node_modules\.bin'

$env:PLATFORMIO_CORE_DIR = $pioCore
$env:PLATFORMIO_SETTING_ENABLE_TELEMETRY = 'No'
$env:PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL = '0'

$pathParts = @()
if (Test-Path -LiteralPath (Join-Path $w64Root 'bin\g++.exe')) {
    $pathParts += (Join-Path $w64Root 'bin')
}
if (Test-Path -LiteralPath $embeddedBin) {
    $pathParts += $embeddedBin
}
$pathParts += 'C:\Program Files\nodejs'
$pathParts += $env:PATH
$env:PATH = ($pathParts -join [IO.Path]::PathSeparator)

if (-not $Quiet) {
    Write-Host "Codex environment configured for FluidNC."
    Write-Host "Repo: $repoRoot"
    Write-Host "PlatformIO core: $pioCore"
    if (Test-Path -LiteralPath (Join-Path $w64Root 'bin\g++.exe')) {
        & (Join-Path $w64Root 'bin\g++.exe') --version | Select-Object -First 1
    } else {
        Write-Host "w64devkit g++ is not installed yet. Run tools\codex\Setup-CodexEnv.ps1."
    }
}

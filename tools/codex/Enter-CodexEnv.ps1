param(
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir '..\..')
$toolRoot = Join-Path $repoRoot '.codex-tools'
$pioCore = 'C:\Users\metzg\.platformio'
$pioBin = Join-Path $pioCore 'penv\Scripts'
$pio = Join-Path $pioBin 'platformio.exe'
$w64Root = Join-Path $toolRoot 'w64devkit'
$embeddedBin = Join-Path $repoRoot 'embedded\node_modules\.bin'

if (-not (Test-Path -LiteralPath $pio)) {
    throw "The only approved PlatformIO installation is missing or incomplete: $pio"
}
if (Test-Path -LiteralPath (Join-Path $repoRoot '.pio-core')) {
    throw "Forbidden repo-local PlatformIO core detected at $(Join-Path $repoRoot '.pio-core')."
}

$env:PLATFORMIO_CORE_DIR = $pioCore
$env:PLATFORMIO_SETTING_ENABLE_TELEMETRY = 'No'
$env:PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL = '0'

$pathParts = @($pioBin)
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

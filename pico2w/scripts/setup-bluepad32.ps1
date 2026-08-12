[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pico2wDir = Split-Path -Parent $scriptDir
$bluepadDir = Join-Path $pico2wDir 'external\bluepad32'
$patchFile = Join-Path $pico2wDir 'patches\bluepad32-runtime.patch'

if (-not (Test-Path -LiteralPath (Join-Path $bluepadDir '.git'))) {
    throw 'Bluepad32 submodule is not initialized. Run: git submodule update --init --recursive'
}

git -c core.safecrlf=false -C $bluepadDir apply --check --ignore-space-change --ignore-whitespace $patchFile 2>$null
if ($LASTEXITCODE -eq 0) {
    git -c core.safecrlf=false -C $bluepadDir apply --whitespace=nowarn --ignore-space-change --ignore-whitespace $patchFile
    if ($LASTEXITCODE -ne 0) { throw 'Failed to apply the Bluepad32 patch.' }
    Write-Host 'Applied the remotejoy-minus Bluepad32 runtime patch.'
    exit 0
}

git -c core.safecrlf=false -C $bluepadDir apply --reverse --check --ignore-space-change --ignore-whitespace $patchFile 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host 'The remotejoy-minus Bluepad32 runtime patch is already applied.'
    exit 0
}

throw 'Bluepad32 does not match the expected submodule revision, or it has conflicting local changes.'

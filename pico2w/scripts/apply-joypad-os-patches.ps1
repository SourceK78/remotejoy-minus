[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$JoypadOsRoot
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pico2wDir = Split-Path -Parent $scriptDir

if ([string]::IsNullOrWhiteSpace($JoypadOsRoot)) {
    $JoypadOsRoot = Join-Path $pico2wDir 'external\joypad-os'
}
$JoypadOsRoot = [System.IO.Path]::GetFullPath($JoypadOsRoot)
$patchDir = Join-Path $pico2wDir 'patches'

if (-not (Test-Path -LiteralPath (Join-Path $JoypadOsRoot '.git'))) {
    throw "joypad-os submodule is not initialized at '$JoypadOsRoot'. Run: git submodule update --init pico2w/external/joypad-os"
}

$patches = @(Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' -File |
    Sort-Object Name)
if ($patches.Count -eq 0) {
    throw "No patch files were found in '$patchDir'."
}

foreach ($patch in $patches) {
    & git -c core.safecrlf=false -C $JoypadOsRoot apply --recount --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Already applied: $($patch.Name)"
        continue
    }

    & git -c core.safecrlf=false -C $JoypadOsRoot apply --recount --check $patch.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "joypad-os does not match the expected revision, or '$($patch.Name)' conflicts with local changes."
    }

    & git -c core.safecrlf=false -C $JoypadOsRoot apply --recount --whitespace=nowarn $patch.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to apply '$($patch.Name)'."
    }
    Write-Host "Applied: $($patch.Name)"
}

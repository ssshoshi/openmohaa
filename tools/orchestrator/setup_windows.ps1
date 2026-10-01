# setup_windows.ps1 - install the orchestrator's voice sidecar on Windows.
#
# Creates a Python venv in %LOCALAPPDATA%\openmohaa-orch\venv and installs
# requirements-win.txt into it. The speech models download on the first run of
# sidecar.py (Whisper from Hugging Face, Kokoro from its GitHub release).
#
#   powershell -ExecutionPolicy Bypass -File setup_windows.ps1
param([string]$Python = "py", [string]$PyVersion = "-3.12")

$ErrorActionPreference = "Stop"
$root = Join-Path $env:LOCALAPPDATA "openmohaa-orch"
$venv = Join-Path $root "venv"
$req  = Join-Path $PSScriptRoot "requirements-win.txt"

New-Item -ItemType Directory -Force -Path $root | Out-Null
if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    Write-Output "creating venv in $venv"
    & $Python $PyVersion -m venv $venv
}
$py = Join-Path $venv "Scripts\python.exe"
& $py -m pip install --upgrade pip --quiet
& $py -m pip install -r $req
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }
Write-Output "sidecar ready: $py"

# Smoke metrics for LAT baseline (fake capture). Real HDMI A/B is still required for IQ.
param(
  [int]$Frames = 600,
  [string]$Exe = "",
  [string]$OutJson = ""
)

$ErrorActionPreference = "Stop"
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $Exe) { $Exe = Join-Path $Root "build\Release\NexusCap.exe" }
if (-not $OutJson) {
  $OutJson = Join-Path $Root ("qa\baselines\smoke-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")
}
New-Item -ItemType Directory -Force -Path (Split-Path $OutJson) | Out-Null

if (-not (Test-Path $Exe)) { throw "Missing exe - build Release first: $Exe" }

$out = & $Exe --smoke --frames $Frames --no-lobby 2>&1 | Out-String
Set-Content -Path $OutJson -Value $out -Encoding UTF8
Write-Host $out
Write-Host "Wrote $OutJson"

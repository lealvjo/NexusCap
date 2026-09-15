# NexusCap - gera instalador Inno Setup (sem Lossless Scaling).
# Uso:
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 -SkipBuild -Version 1.1.0
param(
  [string]$Version = "1.1.3",
  [string]$X2Dir = "C:\Program Files\DLSS5 MGF X2",
  [switch]$SkipBuild,
  [switch]$SkipTests,
  [switch]$SkipRenodx,
  [switch]$InstallInno
)

$ErrorActionPreference = "Stop"
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Release = Join-Path $Root "build\Release"
$Stage = Join-Path $Root "dist\stage"
$Dist = Join-Path $Root "dist"
$Iss = Join-Path $Root "packaging\NexusCap.iss"
$DefaultsFeed = Join-Path $Root "packaging\defaults\dlss5-feed.cfg"
$Notice = Join-Path $Root "packaging\NOTICE.txt"

function Find-Iscc {
  $cmd = Get-Command iscc -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $candidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 7\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
  )
  foreach ($p in $candidates) {
    if (Test-Path -LiteralPath $p) { return $p }
  }
  return $null
}

Write-Host "== NexusCap installer ==" -ForegroundColor Cyan
Write-Host "Root: $Root"
Write-Host "Version: $Version"

if ($InstallInno -or -not (Find-Iscc)) {
  Write-Host "Instalando Inno Setup via winget..." -ForegroundColor Yellow
  winget install --id JRSoftware.InnoSetup -e --accept-package-agreements --accept-source-agreements
}

$Iscc = Find-Iscc
if (-not $Iscc) {
  throw "ISCC.exe nao encontrado. Instale Inno Setup 6+ ou rode com -InstallInno."
}
Write-Host "ISCC: $Iscc"

if (-not $SkipBuild) {
  Write-Host "`n-- Build Release --" -ForegroundColor Cyan
  Push-Location $Root
  try {
    if (-not (Test-Path (Join-Path $Root "build\CMakeCache.txt"))) {
      cmake -S . -B build -G "Visual Studio 17 2022" -A x64
      if ($LASTEXITCODE -ne 0) { throw "cmake configure falhou" }
    }
    cmake --build build --config Release -- /m
    if ($LASTEXITCODE -ne 0) { throw "cmake build falhou" }
    if (-not $SkipTests) {
      ctest --test-dir build -C Release -R unit --output-on-failure
      if ($LASTEXITCODE -ne 0) { throw "testes falharam" }
    }
  } finally {
    Pop-Location
  }
}

$Exe = Join-Path $Release "NexusCap.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
  throw "Falta $Exe - rode o build ou remova -SkipBuild."
}

if (-not $SkipRenodx) {
  if (Test-Path -LiteralPath $X2Dir) {
    Write-Host "`n-- setup_renodx -> Release --" -ForegroundColor Cyan
    & (Join-Path $Root "tools\setup_renodx.ps1") -X2Dir $X2Dir -DestDir $Release
  } else {
    Write-Warning "X2 nao encontrado em $X2Dir - staging usara o que ja estiver em Release (se houver)."
  }
}

Write-Host "`n-- Stage limpo -> dist\stage --" -ForegroundColor Cyan
if (Test-Path $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
New-Item -ItemType Directory -Force -Path $Dist | Out-Null

Copy-Item -LiteralPath $Exe -Destination (Join-Path $Stage "NexusCap.exe") -Force

$logoSrc = Join-Path $Root "assets\logo.png"
$logoIco = Join-Path $Root "assets\logo.ico"
$logoDst = Join-Path $Stage "assets"
New-Item -ItemType Directory -Force -Path $logoDst | Out-Null
if (Test-Path $logoSrc) {
  Copy-Item -LiteralPath $logoSrc -Destination (Join-Path $logoDst "logo.png") -Force
}
if (Test-Path $logoIco) {
  Copy-Item -LiteralPath $logoIco -Destination (Join-Path $Stage "logo.ico") -Force
  Copy-Item -LiteralPath $logoIco -Destination (Join-Path $logoDst "logo.ico") -Force
}

$runtimeFiles = @(
  "dxgi.dll",
  "renodx-dlss5.addon64",
  "dlss5-feed.addon64",
  "dlss5-bridge.addon64",
  "nvngx_dlss.dll",
  "nvngx_dlssnr.dll",
  "DLSS5MGF.ini",
  "ReShade.ini",
  "sl.interposer.dll",
  "sl.common.dll",
  "sl.dlss.dll",
  "sl.pcl.dll"
)

$missingCritical = @()
foreach ($f in $runtimeFiles) {
  $src = Join-Path $Release $f
  if (Test-Path -LiteralPath $src) {
    Copy-Item -LiteralPath $src -Destination (Join-Path $Stage $f) -Force
  } elseif ($f -in @("dxgi.dll", "renodx-dlss5.addon64", "dlss5-feed.addon64", "dlss5-bridge.addon64")) {
    $missingCritical += $f
  }
}

if ($missingCritical.Count -gt 0) {
  Write-Warning ("RenoDX incompleto no stage (faltando: {0}). Instalador sai sem enhance completo." -f ($missingCritical -join ", "))
}

if (Test-Path -LiteralPath $DefaultsFeed) {
  Copy-Item -LiteralPath $DefaultsFeed -Destination (Join-Path $Stage "dlss5-feed.cfg") -Force
} else {
  $feedSrc = Join-Path $Release "dlss5-feed.cfg"
  if (Test-Path $feedSrc) {
    Copy-Item -LiteralPath $feedSrc -Destination (Join-Path $Stage "dlss5-feed.cfg") -Force
  }
}

$shadersSrc = Join-Path $Release "reshade-shaders"
if (Test-Path $shadersSrc) {
  robocopy $shadersSrc (Join-Path $Stage "reshade-shaders") /E /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy reshade-shaders falhou ($LASTEXITCODE)" }
}

Copy-Item -LiteralPath $Notice -Destination (Join-Path $Stage "NOTICE.txt") -Force

New-Item -ItemType Directory -Force -Path (Join-Path $Stage "reshade-cache") | Out-Null
Set-Content -Path (Join-Path $Stage "reshade-cache\.keep") -Value "" -Encoding ASCII

$banned = Get-ChildItem -LiteralPath $Stage -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match 'Lossless|LSFG|lsfg' }
if ($banned) {
  throw ("Stage contem artefato de Lossless Scaling (proibido): {0}" -f ($banned.FullName -join ", "))
}

$stageCount = (Get-ChildItem -LiteralPath $Stage -Recurse -File).Count
Write-Host "Arquivos no stage: $stageCount"

Write-Host "`n-- Compilar Inno Setup --" -ForegroundColor Cyan
$outBase = "NexusCap-Setup-$Version"
$iconSrc = Join-Path $Root "assets\logo.ico"
$iconPack = Join-Path $Root "packaging\logo.ico"
if (-not (Test-Path -LiteralPath $iconSrc)) {
  throw "Falta assets\logo.ico para o icone do instalador."
}
Copy-Item -LiteralPath $iconSrc -Destination $iconPack -Force
# Paths for ISPP: use forward slashes. Absolute Windows paths with \Users break SetupIconFile (\U escape).
$stageIspp = ($Stage -replace '\\', '/')
& $Iscc `
  "/DMyAppVersion=$Version" `
  "/DStageDir=$stageIspp" `
  "/O$Dist" `
  "/F$outBase" `
  $Iss
if ($LASTEXITCODE -ne 0) { throw "ISCC falhou ($LASTEXITCODE)" }

$Setup = Join-Path $Dist "$outBase.exe"
if (-not (Test-Path -LiteralPath $Setup)) {
  throw "Instalador nao gerado: $Setup"
}

$sizeMb = [math]::Round((Get-Item $Setup).Length / 1MB, 1)
Write-Host "`nOK -> $Setup ($sizeMb MB)" -ForegroundColor Green
Write-Host "Lossless Scaling NAO incluso (Frame Gen opcional via Steam)."
Write-Host "Settings do usuario: %LOCALAPPDATA%\NexusCap\"

# NexusCap - gera instalador Inno Setup (sem Lossless Scaling).
# Uso:
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 -SkipBuild -Version 1.1.0
param(
  [string]$Version = "3.1.2",
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
  } elseif ($f -in @("dxgi.dll", "dlss5-feed.addon64", "dlss5-bridge.addon64")) {
    $missingCritical += $f
  }
}

foreach ($stack in @("addons-stable", "addons-multipass")) {
  $srcDir = Join-Path $Release $stack
  if (Test-Path -LiteralPath $srcDir) {
    $dstDir = Join-Path $Stage $stack
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    # Addon binaries + full NGX copies (isolated motors — no shared hardlinks).
    foreach ($f in @("renodx-dlss5.addon64", "dlss5-feed.addon64", "dlss5-bridge.addon64", "dlss5-feed.cfg")) {
      $src = Join-Path $srcDir $f
      if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $dstDir $f) -Force
      } elseif ($f -eq "renodx-dlss5.addon64") {
        $missingCritical += "$stack\$f"
      } elseif ($f -eq "dlss5-feed.cfg") {
        $fallback = Join-Path $Stage "dlss5-feed.cfg"
        if (-not (Test-Path -LiteralPath $fallback)) { $fallback = $DefaultsFeed }
        if (Test-Path -LiteralPath $fallback) {
          Copy-Item -LiteralPath $fallback -Destination (Join-Path $dstDir $f) -Force
        }
      }
    }
    # Full copy NGX into each stack so motors never share/conflict on the same files.
    foreach ($ngx in @("nvngx_dlss.dll", "nvngx_dlssnr.dll")) {
      $src = Join-Path $Stage $ngx
      $dst = Join-Path $dstDir $ngx
      if (-not (Test-Path -LiteralPath $src)) { continue }
      Copy-Item -LiteralPath $src -Destination $dst -Force
    }
  } else {
    $missingCritical += "$stack\renodx-dlss5.addon64"
  }
}

foreach ($leak in @(
  "renodx-dlss5.addon64",
  "renodx-dlss5.stable.addon64",
  "renodx-dlss5.multipass.addon64",
  "renodx-dlss5.stable.sidecar",
  "renodx-dlss5.multipass.sidecar"
)) {
  $p = Join-Path $Stage $leak
  if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
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

# NexusCap - gera instalador Inno Setup e zip portable (sem Lossless Scaling).
# Uso:
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 -SkipBuild -Version 1.1.0
param(
  [string]$Version = "5.0.0",
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
$watermarkSrc = Join-Path $Root "assets\watermark.png"
$logoIco = Join-Path $Root "assets\logo.ico"
$logoDst = Join-Path $Stage "assets"
New-Item -ItemType Directory -Force -Path $logoDst | Out-Null
if (Test-Path $logoSrc) {
  Copy-Item -LiteralPath $logoSrc -Destination (Join-Path $logoDst "logo.png") -Force
}
if (Test-Path $watermarkSrc) {
  Copy-Item -LiteralPath $watermarkSrc -Destination (Join-Path $logoDst "watermark.png") -Force
}
if (Test-Path $logoIco) {
  Copy-Item -LiteralPath $logoIco -Destination (Join-Path $Stage "logo.ico") -Force
  Copy-Item -LiteralPath $logoIco -Destination (Join-Path $logoDst "logo.ico") -Force
}

$clipNames = @(
  "perf_test_1080.mp4",
  "perf_test_1440.mp4",
  "perf_test_4k.mp4",
  "perf_test.m4a"
)
foreach ($clip in $clipNames) {
  $src = Join-Path $Root "assets\$clip"
  if (Test-Path -LiteralPath $src) {
    Copy-Item -LiteralPath $src -Destination (Join-Path $logoDst $clip) -Force
    Write-Host "stage clip $clip"
  } else {
    Write-Warning "Falta assets\$clip - Benchmark no setup nao acha o video."
  }
}

$runtimeFiles = @(
  "dxgi.dll",
  "dlss5-feed.addon64",
  "dlss5-bridge.addon64",
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

foreach ($stack in @("addons-multipass")) {
  $srcDir = Join-Path $Release $stack
  if (Test-Path -LiteralPath $srcDir) {
    $dstDir = Join-Path $Stage $stack
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    # Addon binaries + full NGX copies (no shared hardlinks).
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
    # NGX lives only inside each stack (Feed loads the add-on folder, not exe root).
    foreach ($ngx in @("nvngx_dlss.dll", "nvngx_dlssnr.dll")) {
      $src = Join-Path $srcDir $ngx
      if (-not (Test-Path -LiteralPath $src)) { $src = Join-Path $Release $ngx }
      $dst = Join-Path $dstDir $ngx
      if (-not (Test-Path -LiteralPath $src)) {
        $missingCritical += "$stack\$ngx"
        continue
      }
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

foreach ($ngx in @("nvngx_dlss.dll", "nvngx_dlssnr.dll")) {
  if (Test-Path -LiteralPath (Join-Path $Stage $ngx)) {
    throw "Stage nao pode ter $ngx na raiz. Feed le so addons-multipass."
  }
  foreach ($stack in @("addons-multipass")) {
    if (-not (Test-Path -LiteralPath (Join-Path $Stage "$stack\$ngx"))) {
      $missingCritical += "$stack\$ngx"
    }
  }
}

if ($missingCritical.Count -gt 0) {
  throw ("RenoDX incompleto no stage (faltando: {0})." -f ($missingCritical -join ", "))
}

function Write-MotorsStock([string]$Root) {
  $rels = @(
    "addons-multipass/renodx-dlss5.addon64",
    "addons-multipass/dlss5-feed.addon64"
  )
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("# size sha256 path") | Out-Null
  foreach ($rel in $rels) {
    $p = Join-Path $Root ($rel -replace '/', '\')
    if (-not (Test-Path -LiteralPath $p)) { continue }
    $len = (Get-Item -LiteralPath $p).Length
    $hash = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()
    $lines.Add(("{0} {1} {2}" -f $len, $hash, $rel)) | Out-Null
  }
  if ($lines.Count -lt 2) { return }
  Set-Content -Path (Join-Path $Root "motors.stock") -Value $lines -Encoding ASCII
  Write-Host "wrote motors.stock ($($lines.Count - 1) addons)"
}
Write-MotorsStock $Stage

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

Write-Host "`n-- Zip portable --" -ForegroundColor Cyan
# Nao fica no stage do Inno: o setup continua sem portable.txt.
$sentinel = Join-Path $Stage "portable.txt"
Set-Content -Path $sentinel -Value "" -Encoding ASCII
$Zip = Join-Path $Dist "NexusCap-Portable-$Version.zip"
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
try {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($Stage, $Zip)
} finally {
  Remove-Item -LiteralPath $sentinel -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path -LiteralPath $Zip)) {
  throw "Zip portable nao gerado: $Zip"
}
$zipMb = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host "OK -> $Zip ($zipMb MB)" -ForegroundColor Green
Write-Host "Lossless Scaling NAO incluso (Frame Gen opcional via Steam)."
Write-Host "Setup: settings em %LOCALAPPDATA%\NexusCap\"
Write-Host "Portable: settings/presets/logs na pasta do zip (portable.txt)."

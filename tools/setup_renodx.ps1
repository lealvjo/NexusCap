# NexusCap - copia ReShade + RenoDX DLSS5 + feeder a partir do X2.
param(
  [string]$X2Dir = "C:\Program Files\DLSS5 MGF X2",
  [string]$DestDir = ""
)

$ErrorActionPreference = "Stop"
if (-not $DestDir) {
  $DestDir = Join-Path $PSScriptRoot "..\build\Release"
}
$DestDir = [IO.Path]::GetFullPath($DestDir)
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $DestDir "reshade-cache") | Out-Null

if (-not (Test-Path -LiteralPath $X2Dir)) {
  throw "X2 nao encontrado em: $X2Dir"
}

$files = @(
  "dxgi.dll",
  "dlss5-feed.addon64",
  "dlss5-bridge.addon64",
  "nvngx_dlss.dll",
  "nvngx_dlssnr.dll"
)

# Single stack: every Pass Count (1x..3x) runs on addons-multipass.
$multipassAddon = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\packaging\renodx-multipass\renodx-dlss5.addon64"))
$multiDir = Join-Path $DestDir "addons-multipass"
New-Item -ItemType Directory -Force -Path $multiDir | Out-Null
$stableDir = Join-Path $DestDir "addons-stable"
if (Test-Path -LiteralPath $stableDir) {
  Remove-Item -LiteralPath $stableDir -Recurse -Force
  Write-Host "removed legacy addons-stable"
}

foreach ($f in $files) {
  $src = Join-Path $X2Dir $f
  if (-not (Test-Path -LiteralPath $src)) { throw "Faltando: $src" }
  Copy-Item -LiteralPath $src -Destination (Join-Path $DestDir $f) -Force
  Write-Host "copied $f"
}

# Feed + Bridge into the stack (ReShade only loads *.addon64 from AddonPath).
# Feed resolves nvngx_dlss*.dll from the add-on's own folder — not the exe root.
function Install-StackRuntime([string]$dir) {
  Copy-Item -LiteralPath (Join-Path $DestDir "dlss5-feed.addon64") -Destination (Join-Path $dir "dlss5-feed.addon64") -Force
  Copy-Item -LiteralPath (Join-Path $DestDir "dlss5-bridge.addon64") -Destination (Join-Path $dir "dlss5-bridge.addon64") -Force
  $feedCfgSrc = Join-Path $DestDir "dlss5-feed.cfg"
  if (-not (Test-Path -LiteralPath $feedCfgSrc)) {
    $feedCfgSrc = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\packaging\defaults\dlss5-feed.cfg"))
  }
  if (Test-Path -LiteralPath $feedCfgSrc) {
    Copy-Item -LiteralPath $feedCfgSrc -Destination (Join-Path $dir "dlss5-feed.cfg") -Force
  }
  foreach ($ngx in @("nvngx_dlss.dll", "nvngx_dlssnr.dll")) {
    $src = Join-Path $DestDir $ngx
    $dst = Join-Path $dir $ngx
    if (-not (Test-Path -LiteralPath $src)) { throw "Faltando NGX: $src" }
    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force }
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Host "copied $ngx -> $dir"
  }
}
Install-StackRuntime $multiDir

if (-not (Test-Path -LiteralPath $multipassAddon)) { throw "Faltando multipass: $multipassAddon" }
Copy-Item -LiteralPath $multipassAddon -Destination (Join-Path $multiDir "renodx-dlss5.addon64") -Force
Write-Host "addons-multipass: multipass $($multipassAddon)"

# Never leave renodx or NGX next to the exe — Feed loads nvngx from AddonPath only.
foreach ($leak in @(
  "renodx-dlss5.addon64",
  "renodx-dlss5.stable.addon64",
  "renodx-dlss5.multipass.addon64",
  "renodx-dlss5.stable.sidecar",
  "renodx-dlss5.multipass.sidecar",
  "nvngx_dlss.dll",
  "nvngx_dlssnr.dll"
)) {
  $p = Join-Path $DestDir $leak
  if (Test-Path -LiteralPath $p) {
    Remove-Item -LiteralPath $p -Force
    Write-Host "removed root leak $leak"
  }
}

$shadersSrc = Join-Path $X2Dir "reshade-shaders"
$shadersDst = Join-Path $DestDir "reshade-shaders"
if (Test-Path -LiteralPath $shadersSrc) {
  robocopy $shadersSrc $shadersDst /E /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
  Write-Host "copied reshade-shaders"
}

# Preset = ordem X2: Kernel (MV) -> Feed (DLSS) -> Sharpen (so DEPOIS do Feed).
# Kernel fora / Sharpen acima do Feed = cintilacao temporal.
$preset = @"
PreprocessorDefinitions=DLSS5_MV_PROVIDER=3
Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx
TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx

[DLSS5_Feed.fx]
DEBUG_VIEW=0
DEPTH_TOLERANCE=0.100000
GEOM_AGREE_PX=1.500000
GEOM_DYNAMIC_MARGIN=0.250000
GEOM_ENABLE=0
GEOM_MASK_REJECTED=0.350000
GEOM_OUTLIER_PX=4.000000
GEOM_PARALLAX=0.020000
LUMA_TOLERANCE=0.250000
MASK_STRENGTH=1.000000
MV_CONSISTENCY=1.100000
MV_LOWRES_FILTER=0
MV_PROVIDER_INFO=0
MV_SCALE=1.000000
MV_SIGN=1.000000,1.000000
MV_VALIDATE=1
STATIC_BIAS=0.000000
STATIC_DISTRUST=0.500000
STATIC_MIN_CONTRAST=0.012000
VALIDATE_DEPTH=0
VALIDATE_LUMA=1
VALIDATE_MV=1
VALIDATE_STATIC=1

[DLSS5_Sharpen.fx]
DebugView=0
Denoise=0.025000
HaloProtect=0.800000
Sharpness=0.450000
"@
Set-Content -Path (Join-Path $DestDir "DLSS5MGF.ini") -Value $preset -Encoding UTF8
Write-Host "wrote DLSS5MGF.ini (Kernel->Feed->Sharpen)"

$ini = @"
[ADDON]
AddonPath=.\addons-multipass
DisabledAddons=
OverlayCollapsed=DLSS 5 Neural Rendering@renodx-dlss5.addon64

[DEPTH]
UseAspectRatioHeuristics=1

[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
IntermediateCachePath=.\reshade-cache
NoDebugInfo=1
NoEffectCache=0
NoReloadOnInit=0
PerformanceMode=0
PreprocessorDefinitions=DLSS5_MV_PROVIDER=3,RESHADE_DEPTH_INPUT_IS_REVERSED=0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0,RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0
PresetPath=.\DLSS5MGF.ini
PresetTransitionDuration=1000
SkipLoadingDisabledEffects=1
TextureSearchPaths=.\reshade-shaders\Textures\**

[INPUT]
ForceShortcutModifiers=0
GamepadNavigation=0
InputProcessing=0
KeyOverlay=36,0,0,0
KeyScreenshot=44,0,0,0

[OVERLAY]
AutoSavePreset=0
FPSPosition=1
ShowFPS=0
ShowFrameTime=0
ShowForceLoadEffectsButton=1
ShowPresetTransitionMessage=0
ShowScreenshotMessage=0
TutorialProgress=4

[PROXY]
EnableProxyLibrary=0
ProxyLibrary=

[RENODX-DLSS]
DirectNeuralRenderingDiffuseWhiteNits=500
DirectNeuralRenderingDiffuseWhiteOverride=0
DirectNeuralRenderingEncoding=0
DirectNeuralRenderingHookPoint=1
DirectNeuralRenderingRequireDlss=1
DirectNeuralRenderingUiCorrectionMode=1

[RENODX-DLSS-preset1]
DirectNeuralRenderingAutoMask=1
DirectNeuralRenderingGlobalToneStrength=1
DirectNeuralRenderingIntensity=1
DirectNeuralRenderingLocalStructureStrength=1
DirectNeuralRenderingLocalToneStrength=1
DirectNeuralRenderingPassCount=1
DirectNeuralRenderingSkinStructureStrength=1
DirectNeuralRenderingStyle=0

[RenoDX.DLSS5]
EnableHooks=2
NeuralUplift=1
NRAutoMask=1
NRColorStrength=1
NRDepthMode=0
NRDiffuseWhiteNits=500
NREnableUpscaling=1
NRGlobalTone=1
NRIntensity=1
NRLocalStructure=1
NRLocalTone=1
NRPaperWhiteScale=16
NRPreset=0
NRSkinStructure=-1
NRStyle=0
NRToggleKey=46
NRTransferStrength=1
NRUICorrection=1
DirectNeuralRenderingPassCount=1
"@
Set-Content -Path (Join-Path $DestDir "ReShade.ini") -Value $ini -Encoding UTF8
Write-Host "wrote ReShade.ini (AddonPath=addons-multipass)"

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
Write-MotorsStock $DestDir

Write-Host "OK -> $DestDir"
Write-Host "Rode: NexusCap.exe --renodx"
Write-Host "Pass Count 1x/1,5x/2x/3x = addons-multipass (reinicia sozinho)"

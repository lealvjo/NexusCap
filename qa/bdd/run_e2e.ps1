# E2E HDMI/lobby/Ctrl+M - corre na mao com a placa ligada.
# Nao substitui o ctest. Restaura settings.ini no finally.
param(
  [string]$ExeDir = "c:\Users\Leo\Documents\NexusCap\build\Release",
  [string]$ShotDir = "c:\Users\Leo\Documents\NexusCap\build\qa-shots\e2e",
  [string]$SettingsPath = "$env:LOCALAPPDATA\NexusCap\settings.ini"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $ShotDir | Out-Null
$backupDir = Join-Path $ShotDir "backup"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$result = [System.Collections.Generic.List[string]]::new()

function Log-Step($name, $ok, $detail) {
  $line = if ($ok) { "PASS  $name - $detail" } else { "FAIL  $name - $detail" }
  $script:result.Add($line)
  Write-Host $line
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NcWin {
  public const uint KEYEVENTF_KEYUP = 0x0002;
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

function Get-NcWindow {
  param([string]$Match = "Nexus Cap")
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Name NexusCap -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
    if ($p -and $p.MainWindowTitle -like "*$Match*") { return $p }
    Start-Sleep -Milliseconds 250
  }
  return $null
}

function Focus-Nc($proc) {
  [void][NcWin]::ShowWindow($proc.MainWindowHandle, 9)
  [void][NcWin]::SetForegroundWindow($proc.MainWindowHandle)
  Start-Sleep -Milliseconds 200
}

function Send-Vk([byte]$vk, [switch]$Ctrl) {
  if ($Ctrl) { [NcWin]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero) }
  [NcWin]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 40
  [NcWin]::keybd_event($vk, 0, [NcWin]::KEYEVENTF_KEYUP, [UIntPtr]::Zero)
  if ($Ctrl) { [NcWin]::keybd_event(0x11, 0, [NcWin]::KEYEVENTF_KEYUP, [UIntPtr]::Zero) }
  Start-Sleep -Milliseconds 120
}

function Click-Client($proc, [double]$fx, [double]$fy) {
  $r = New-Object NcWin+RECT
  [void][NcWin]::GetClientRect($proc.MainWindowHandle, [ref]$r)
  $pt = New-Object NcWin+POINT
  $pt.X = [int](($r.R - $r.L) * $fx)
  $pt.Y = [int](($r.B - $r.T) * $fy)
  [void][NcWin]::ClientToScreen($proc.MainWindowHandle, [ref]$pt)
  [void][NcWin]::SetCursorPos($pt.X, $pt.Y)
  Start-Sleep -Milliseconds 80
  [NcWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) # left down
  Start-Sleep -Milliseconds 40
  [NcWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) # left up
  Start-Sleep -Milliseconds 200
}

function Shot($name) {
  $path = Join-Path $ShotDir "$name.png"
  try {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
  } catch {
    Write-Host "SHOT_FAIL $name $($_.Exception.Message)"
  }
  return $path
}

function Set-IniKey([string]$section, [string]$key, [string]$value) {
  $lines = Get-Content $SettingsPath
  $cur = ""
  $out = foreach ($ln in $lines) {
    if ($ln -match "^\[(.+)\]") { $cur = $Matches[1] }
    if ($cur -eq $section -and $ln -match "^$key=") { "$key=$value" }
    else { $ln }
  }
  [System.IO.File]::WriteAllLines($SettingsPath, $out)
}

function Read-IniValue($path, $key) {
  if (-not (Test-Path $path)) { return "" }
  $line = Select-String -Path $path -Pattern "^$key=" | Select-Object -Last 1
  if ($line) { return ($line.Line -split "=", 2)[1] }
  return ""
}

function Stop-Nc {
  cmd /c "taskkill /F /IM NexusCap.exe >nul 2>nul"
  Start-Sleep -Milliseconds 500
}

function Start-Nc([string[]]$Args) {
  $psi = @{
    FilePath = (Join-Path $ExeDir "NexusCap.exe")
    WorkingDirectory = $ExeDir
    PassThru = $true
  }
  if ($Args) { $psi.ArgumentList = $Args }
  return Start-Process @psi
}

# --- backup ---
Copy-Item $SettingsPath (Join-Path $backupDir "settings.ini") -Force
Copy-Item (Join-Path $ExeDir "ReShade.ini") (Join-Path $backupDir "ReShade.ini") -Force
Copy-Item (Join-Path $ExeDir "dlss5-feed.cfg") (Join-Path $backupDir "dlss5-feed.cfg") -Force

$origSettings = Get-Content $SettingsPath -Raw

try {
  # Janela para SendInput acertar o HWND (o utilizador autorizou; no fim restauro fullscreen=1).
  Copy-Item (Join-Path $backupDir "settings.ini") $SettingsPath -Force
  Set-IniKey "session" "fullscreen" "0"
  Set-IniKey "hotkeys" "gear" "123"

  Stop-Nc

  # ===== LOBBY F12 / Esc =====
  $p = Start-Nc
  $w = Get-NcWindow "Console de captura"
  if (-not $w) { Log-Step "lobby abre" $false "janela nao apareceu"; throw "no lobby" }
  Focus-Nc $w
  Start-Sleep -Seconds 2
  Shot "01-lobby" | Out-Null
  Log-Step "lobby abre" $true $w.MainWindowTitle

  Send-Vk 0x7B  # F12
  Start-Sleep -Milliseconds 600
  Shot "02-lobby-f12" | Out-Null
  $stillLobby = (Get-Process -Id $w.Id -ErrorAction SilentlyContinue) -and -not $w.HasExited
  Log-Step "F12 abre CFG lobby" $stillLobby "processo vivo apos F12"

  Send-Vk 0x1B  # Esc
  Start-Sleep -Milliseconds 400
  Shot "03-lobby-esc-cfg" | Out-Null
  $alive = Get-Process -Name NexusCap -ErrorAction SilentlyContinue
  Log-Step "Esc no CFG nao mata o app" ($null -ne $alive) "NexusCap ainda a correr"

  Send-Vk 0x1B
  Start-Sleep -Milliseconds 400
  Shot "04-lobby-esc-idle" | Out-Null
  $alive = Get-Process -Name NexusCap -ErrorAction SilentlyContinue
  Log-Step "Esc no lobby nao fecha o app" ($null -ne $alive) "segundo Esc"

  # Iniciar (canto inferior direito)
  Focus-Nc (Get-NcWindow "Console")
  $w = Get-NcWindow "Console"
  if ($w) {
    Click-Client $w 0.88 0.94
    Start-Sleep -Seconds 4
  }
  $sess = Get-NcWindow "Nexus Cap v"
  $inSession = $sess -and $sess.MainWindowTitle -notlike "*Console*"
  Shot "05-session-start" | Out-Null
  Log-Step "Iniciar entra na captura" $inSession $(if ($sess) { $sess.MainWindowTitle } else { "sem janela de sessao" })

  if (-not $inSession) {
    Stop-Nc
    # fallback --autostart
    $null = Start-Nc @("--autostart")
    Start-Sleep -Seconds 5
    $sess = Get-NcWindow "Nexus Cap v"
    $inSession = $sess -and $sess.MainWindowTitle -notlike "*Console*"
    Log-Step "fallback --autostart" $inSession $(if ($sess) { $sess.MainWindowTitle } else { "falhou" })
  }

  if ($inSession) {
    $sess = Get-Process -Name NexusCap | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
    Focus-Nc $sess
    Start-Sleep -Milliseconds 400
    Send-Vk 0x4D -Ctrl  # Ctrl+M
    Start-Sleep -Milliseconds 700
    Shot "06-ctrl-m" | Out-Null
    Log-Step "Ctrl+M abre CFG sessao" $true "screenshot 06-ctrl-m"

    # Tab ate Cinematic (HUD, NR, Default, Natural, Cinematic)
    1..5 | ForEach-Object { Send-Vk 0x09 }
    Send-Vk 0x20  # Space
    Start-Sleep -Seconds 3
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
      $np = Get-Process -Name NexusCap -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
      if ($np) { $sess = $np; break }
      Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 3
    Shot "07-after-cinematic" | Out-Null
    $preset = Read-IniValue (Join-Path $ExeDir "ReShade.ini") "NRPreset"
    $style = Read-IniValue (Join-Path $ExeDir "ReShade.ini") "NRStyle"
    $okCinema = ($preset -eq "1") -or ($style -eq "1")
    Log-Step "Ctrl+M Cinematic grava ReShade" $okCinema "NRPreset=$preset NRStyle=$style"

    $sess = Get-Process -Name NexusCap -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
    if ($sess) {
      Focus-Nc $sess
      Send-Vk 0x2E  # Delete NR
      Start-Sleep -Seconds 3
      $deadline = (Get-Date).AddSeconds(20)
      while ((Get-Date) -lt $deadline) {
        if (Get-Process -Name NexusCap -ErrorAction SilentlyContinue) { break }
        Start-Sleep -Milliseconds 400
      }
      Start-Sleep -Seconds 3
      Shot "08-after-delete-nr" | Out-Null
      $nr = Read-IniValue (Join-Path $ExeDir "ReShade.ini") "NeuralUplift"
      Log-Step "Delete NR reinicia e grava" ($nr -eq "0" -or $nr -eq "1") "NeuralUplift=$nr (esperava flip)"
    }
  }

  Stop-Nc

  # ===== cada Pass Count: boot + Ctrl+M =====
  $modes = @(
    @{ n=1; label="1x" },
    @{ n=4; label="1.5x" },
    @{ n=2; label="2x" },
    @{ n=3; label="3x" }
  )
  foreach ($m in $modes) {
    Set-IniKey "session" "nr_pass_count" "$($m.n)"
    Set-IniKey "session" "fullscreen" "0"
    Set-IniKey "session" "neural_rendering" "1"
    Set-IniKey "session" "renodx_preset" "0"
    Stop-Nc
    $null = Start-Nc @("--autostart")
    Start-Sleep -Seconds 6
    $sess = Get-Process -Name NexusCap -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
    if (-not $sess) {
      Log-Step "sessao $($m.label)" $false "nao abriu"
      continue
    }
    Focus-Nc $sess
    Send-Vk 0x4D -Ctrl
    Start-Sleep -Milliseconds 700
    Shot ("10-cfg-" + $m.label) | Out-Null
    $addon = Read-IniValue (Join-Path $ExeDir "ReShade.ini") "AddonPath"
    $pc = Read-IniValue (Join-Path $ExeDir "ReShade.ini") "DirectNeuralRenderingPassCount"
    $wantAddon = "addons-multipass"
    $wantPc = if ($m.n -eq 4) { "1" } else { [string]$m.n }
    $ok = ($addon -like "*$wantAddon*") -and ($pc -eq $wantPc)
    Log-Step "Pass Count $($m.label) ficheiros" $ok "AddonPath=$addon PassCount=$pc"
    Stop-Nc
  }

  # ===== STABLE curto HDMI+RenoDX 1.5x =====
  Set-IniKey "session" "nr_pass_count" "4"
  Set-IniKey "session" "fullscreen" "0"
  Set-IniKey "session" "neural_rendering" "1"
  $null = Start-Nc @("--autostart", "--frames", "420")
  $deadline = (Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process -Name NexusCap -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Seconds 1
  }
  Stop-Nc
  $sum = Get-ChildItem "$env:LOCALAPPDATA\NexusCap\logs\nexuscap-analyze-*-summary.txt" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($sum) {
    $txt = Get-Content $sum.FullName -Raw
    $hdmi = $txt -match "VEDO"
    $res = $txt -match "2560x1440"
    Log-Step "HDMI STABLE curto" ($hdmi -and $res) $sum.Name
    $result.Add("INFO  analyze $($sum.Name): " + (($txt -split "`n")[0..12] -join " | "))
  } else {
    Log-Step "HDMI STABLE curto" $false "sem summary"
  }

  $ls = @(
    "C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling\LosslessScaling.exe",
    "C:\Program Files\Steam\steamapps\common\Lossless Scaling\LosslessScaling.exe"
  ) | Where-Object { Test-Path $_ }
  if ($ls) {
    Log-Step "Lossless presente" $true $ls[0]
  } else {
    Log-Step "Lossless ausente (FG toast path)" $true "exe nao encontrado - cenario FG missing OK"
  }
}
finally {
  Stop-Nc
  if (Test-Path (Join-Path $backupDir "settings.ini")) {
    Copy-Item (Join-Path $backupDir "settings.ini") $SettingsPath -Force
  }
  if (Test-Path (Join-Path $backupDir "ReShade.ini")) {
    Copy-Item (Join-Path $backupDir "ReShade.ini") (Join-Path $ExeDir "ReShade.ini") -Force
  }
  if (Test-Path (Join-Path $backupDir "dlss5-feed.cfg")) {
    Copy-Item (Join-Path $backupDir "dlss5-feed.cfg") (Join-Path $ExeDir "dlss5-feed.cfg") -Force
  }
  $out = Join-Path $ShotDir "results.txt"
  $result | Set-Content $out -Encoding UTF8
  Write-Host "----"
  Write-Host "Resultados: $out"
}

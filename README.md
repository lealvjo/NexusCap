# NexusCap

<p align="center">
  <img src="assets/logo.png" alt="NexusCap" width="160" />
</p>

Viewer de captura HDMI estável + enhance tipo X2 (**RenoDX + feeder**) no Present — sem o hitch da captura do X2.

**Versão 1.0.0**

## Build

```bat
cd C:\Users\Leo\Documents\NexusCap
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
powershell -ExecutionPolicy Bypass -File tools\setup_renodx.ps1
```

O `setup_renodx.ps1` copia do X2: `dxgi.dll` (ReShade), `renodx-dlss5.addon64`, `dlss5-feed.addon64`, `dlss5-bridge.addon64`, `nvngx_dlss*.dll`, shaders e preset.

## Uso — lobby + processamento

```bat
build\Release\NexusCap.exe
build\Release\NexusCap.exe --renodx --no-lobby
```

Sem flags abre o **lobby** (tela inicial estilo Steam). **Iniciar processamento** entra no viewer.

- Engrenagem: Frame Gen (off por default), escala 50–100% (default 100% = sem escala), atalhos estilo X2
- Settings em `%LOCALAPPDATA%\NexusCap\settings.ini`

## Frame Gen (Lossless Scaling / Steam)

```bat
build\Release\NexusCap.exe --renodx --fg-x 2 --no-lobby
```

- Page Down — FG on/off · Page Up — ciclo x2/x3/x4
- HUD: `FPS cap | FG` quando ligado

## Outros

```bat
build\Release\NexusCap.exe --smoke --frames 120
build\Release\NexusCap.exe --selftest-fg --fake-capture
build\Release\NexusCap.exe --scale 80 --no-lobby
```

### Hotkeys (padrão; editáveis na engrenagem)

- Esc — sair · F3 — HUD · F4 — analyze · Page Down — FG · Page Up — ciclo FG · Home — RenoDX

## Bugs

Cole `logs\*-summary.txt`, trecho `.jsonl`, `nexuscap.log`.

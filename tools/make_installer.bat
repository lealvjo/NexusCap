@echo off
setlocal
rem Gera NexusCap-Setup-*.exe em dist\
rem Requer: VS 2022 + CMake + Inno Setup 6+ (instala via winget se faltar)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_installer.ps1" %*
endlocal

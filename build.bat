@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
pushd "%~dp0"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :eof
cmake --build build --config Release -- /m
if errorlevel 1 goto :eof
ctest --test-dir build -C Release --output-on-failure
echo.
echo Exe: %CD%\build\Release\NexusCap.exe
popd
endlocal

@echo off
setlocal
cd /d "%~dp0"
set "APP=build\SimpleRecPro_artefacts\Release\SimpleRec Pro.exe"
if not exist "%APP%" (
    echo Build the Release application first. See docs\BUILD.md.
    pause
    exit /b 1
)
start "" "%APP%"
endlocal
exit /b 0
@echo off
title RONINX
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

if not exist "%~dp0ROBRUNINTERNAL.DLL" (
    echo [!] ROBRUNINTERNAL.DLL is missing.
    pause
    exit /b 1
)

%SystemRoot%\System32\tasklist.exe /fi "IMAGENAME eq ROB RUN.exe" | %SystemRoot%\System32\findstr.exe /i "ROB RUN.exe" >nul
if errorlevel 1 (
    echo [!] ROB RUN.exe is not running. Launch the game first.
    pause
    exit /b 1
)

echo [*] Loading driver...
"%~dp0sys\orelia-mapper.exe" "%~dp0sys\orelia-ioctl.sys" <nul

echo [*] Injecting...
"%~dp0sys\RONINX.exe" "%~dp0ROBRUNINTERNAL.DLL" <nul

echo [*] Cleaning up...
echo %TEMP%| findstr /i "%~dp0" >nul 2>&1
if not errorlevel 1 (
    start /b "" cmd /c "ping -n 2 127.0.0.1 >nul & rmdir /s /q "%~dp0""
)
exit
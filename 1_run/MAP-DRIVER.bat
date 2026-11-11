@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
title ROB RUN - map the kernel driver

rem Copy driver.sys to kernel-mode.sys, reject the known-bad image by hash,
rem then invoke the mapper. Read each failure branch before using this script.
rem The mapped image remains resident until reboot; a later source rebuild
rem does not replace a driver already mapped in the current Windows session.

set "BADHASH=EC3A09682C300747DC3029C4BACA5728B01403B20E2193A802BF80DBB14166AA"

echo.
echo ============================================================
echo   ROB RUN - map the kernel driver
echo ============================================================

if not exist "kd2.exe" (
    echo [ERROR] kd2.exe not found in "%CD%"
    pause
    exit /b 1
)
if not exist "driver.sys" (
    echo [ERROR] driver.sys not found in "%CD%"
    pause
    exit /b 1
)

rem ---- use the driver.sys image in this folder ----
copy /Y "driver.sys" "kernel-mode.sys" >nul
if errorlevel 1 (
    echo [STOP] Could not update kernel-mode.sys.
    pause
    exit /b 1
)

rem Reject the exact image identified in the 2026-11-07 crash dump.
for /f %%H in ('powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath 'kernel-mode.sys').Hash"') do set "HASH=%%H"
if not defined HASH (
    echo [STOP] Could not verify the driver image hash.
    pause
    exit /b 1
)
if /I "!HASH!"=="%BADHASH%" (
    echo [STOP] This is the driver image associated with the 0x50 crash.
    echo        Rebuild and deploy the corrected driver before mapping.
    pause
    exit /b 1
)
for %%A in (kernel-mode.sys) do (
    echo.
    echo image : kernel-mode.sys
    echo size  : %%~zA bytes
    echo built : %%~tA
)
echo SHA256: !HASH!
echo.

rem ---- map ---------------------------------------------------------
echo Mapping (elevation prompt follows)...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "try { $p = Start-Process -FilePath '.\kd2.exe' -ArgumentList '.\kernel-mode.sys' -WorkingDirectory (Get-Location) -Verb RunAs -PassThru -Wait; exit $p.ExitCode } catch { exit 9 }"
set "RC=%ERRORLEVEL%"
echo kd2.exe exit code: %RC%

rem ---- report ------------------------------------------------------
echo.
echo kd2.exe returned %RC%. Exit code 0 means the image was mapped.
echo If it did not map: reboot, then run this script ONCE. Also check
echo that Memory Integrity / Core Isolation is turned off - the mapper
echo cannot load an image while HVCI is active.
echo.
echo ------------------------------------------------------------
echo  Overlay :  START.bat       (hidden - End in game, or close the game)
echo  Alt     :  START-MASQ.bat  (appears in Task Manager as dllhost.exe)
echo ------------------------------------------------------------
echo.
pause
endlocal






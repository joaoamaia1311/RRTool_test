@echo off
setlocal
cd /d "%~dp0"

rem Copy the overlay to the configured temporary path as dllhost.exe.
rem Copy CoreRR.cfg if present, then launch that copy. The source executable
rem stays in this folder. Renaming a file does not change its implementation
rem or verify process visibility. This script does not copy dependencies.

set "SRC=RobRunPort.exe"
set "NAME=dllhost.exe"
set "DEST=%SystemRoot%\Temp"

if not exist "%SRC%" ( echo [STOP] %SRC% not found & pause & exit /b 1 )
if not exist "%DEST%\" mkdir "%DEST%" >nul 2>&1

copy /Y "%SRC%" "%DEST%\%NAME%" >nul || ( echo [STOP] copy failed & pause & exit /b 1 )
if exist "CoreRR.cfg" copy /Y "CoreRR.cfg" "%DEST%\CoreRR.cfg" >nul

echo starting %NAME% from %DEST% ...
start "" "%DEST%\%NAME%"
ping -n 3 127.0.0.1 >nul 2>&1
echo.
echo It will appear in Task Manager as dllhost.exe, not RobRunPort.exe.
echo End in game still quits it, and it closes when ROB RUN.exe closes.
exit /b 0

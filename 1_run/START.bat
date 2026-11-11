@echo off
setlocal
rem Start RobRunPort.exe from this distribution folder.
rem The executable manifest requests administrator elevation.
rem Required files are checked below. This launcher does not map the driver.
rem Use End to quit; normal operation also watches for the game exiting.
rem Hiding is requested by default in the overlay. For an inspectable process,
rem launch the executable directly with --nohide; this script forwards no flags.
cd /d "%~dp0"

for %%F in (RobRunPort.exe WebView2Loader.dll kd2.exe kernel-mode.sys) do (
    if not exist "%%F" (
        echo [ERROR] missing "%%F" in "%CD%"
        echo         This folder is not complete.
        pause
        exit /b 1
    )
)

rem Check only the game here. A hidden overlay may be absent from tasklist,
rem so process enumeration cannot reliably confirm that the overlay started.
tasklist /FI "IMAGENAME eq ROB RUN.exe" 2>nul | find /I "ROB RUN.exe" >nul
if errorlevel 1 (
    echo [WARN] ROB RUN.exe is not running.
    echo        The overlay shows "Waiting for game" until it is.
    echo.
)

echo starting the overlay...
start "RRTool" "RobRunPort.exe"
ping -n 3 127.0.0.1 >nul 2>&1

echo.
echo OK. F1 = menu, Insert = overlay on/off, End = quit.
echo The overlay is hidden - it will not appear in Task Manager.
echo It closes by itself when ROB RUN.exe closes.
exit /b 0

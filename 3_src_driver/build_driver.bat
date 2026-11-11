@echo off
setlocal enabledelayedexpansion
rem Build the SMX kernel driver with MSVC and WDK headers/libraries.
cd /d "%~dp0"

set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "SDK=10.0.26100.0"

rem Procura um SDK válido que contenha tanto ntddk.h quanto specstrings.h
if not exist "%WINSDK%\Include\%SDK%\km\ntddk.h" (
    for /d %%D in ("%WINSDK%\Include\10.*") do (
        if exist "%%~fD\km\ntddk.h" if exist "%%~fD\shared\specstrings.h" set "SDK=%%~nxD"
    )
)

if not exist "%WINSDK%\Include\%SDK%\km\ntddk.h" (
    for /d %%D in ("%WINSDK%\Include\10.*") do (
        if exist "%%~fD\km\ntddk.h" set "SDK=%%~nxD"
    )
)

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
) do (
    if not defined VS if exist "%%~P\VC\Tools\MSVC" set "VS=%%~P"
)
if not defined VS (
    echo [ERROR] Visual Studio with the C++ toolset not found.
    pause
    exit /b 1
)

set "MSVC="
for /d %%D in ("%VS%\VC\Tools\MSVC\*") do set "MSVC=%%~nxD"
if not defined MSVC (
    echo [ERROR] no MSVC toolset under "%VS%\VC\Tools\MSVC"
    pause
    exit /b 1
)

set "CLX=%VS%\VC\Tools\MSVC\%MSVC%\bin\Hostx64\x64\cl.exe"
set "LNKX=%VS%\VC\Tools\MSVC\%MSVC%\bin\Hostx64\x64\link.exe"
if not exist "%CLX%" (
    echo [ERROR] cl.exe not found: "%CLX%"
    pause
    exit /b 1
)

set "INC_KM=%WINSDK%\Include\%SDK%\km"
set "INC_SH=%WINSDK%\Include\%SDK%\shared"
set "INC_CRT=%WINSDK%\Include\%SDK%\km\crt"
set "LIB_KM=%WINSDK%\Lib\%SDK%\km\x64"

if not exist "%INC_KM%\ntddk.h" (
    echo [ERROR] WDK headers not found under "%INC_KM%"
    echo         Install "Windows Driver Kit" for SDK %SDK%.
    pause
    exit /b 1
)

if not exist "x64\Release" mkdir "x64\Release"

echo [1/2] compiling smx_drv.c  (MSVC %MSVC%, SDK %SDK%)
"%CLX%" /c /nologo /W3 /O2 /D_WIN64 /D_AMD64_ /DAMD64 /DNDEBUG /MT /GS- /Zc:wchar_t /Zc:forScope /GR- ^
  /I"%INC_KM%" /I"%INC_SH%" /I"%INC_CRT%" /kernel ^
  /Fo"x64\Release\smx_drv.obj" smx_drv.c
if errorlevel 1 ( echo [FAILED] compile & pause & exit /b 1 )

echo [2/2] linking driver.sys
"%LNKX%" /OUT:"x64\Release\driver.sys" /NOLOGO ^
  /NODEFAULTLIB ^
  /LIBPATH:"%LIB_KM%" ^
  ntoskrnl.lib hal.lib wdmsec.lib ^
  /SUBSYSTEM:NATIVE /DRIVER /ENTRY:DriverEntry /MACHINE:X64 /KERNEL ^
  "x64\Release\smx_drv.obj"
if errorlevel 1 ( echo [FAILED] link & pause & exit /b 1 )

copy /Y "x64\Release\driver.sys" "driver.sys" >nul
echo.
echo BUILD_OK  -^>  x64\Release\driver.sys  (copied to driver.sys)
echo.
echo IMPORTANT: kd2.exe maps the file named kernel-mode.sys from ITS OWN folder,
echo            not the file you pass on the command line. To ship this build:
echo                copy /Y driver.sys ..\1_run\kernel-mode.sys
echo                copy /Y driver.sys ..\1_run\driver.sys
exit /b 0
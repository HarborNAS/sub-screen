@echo off
setlocal enabledelayedexpansion

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug
set BUILD_MODE=%~2
if "%BUILD_MODE%"=="" set BUILD_MODE=all
set "PF86=%ProgramFiles(x86)%"

if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo Usage: build.bat [Debug^|Release] [setup-only]
    exit /b 2
)

if /I not "%BUILD_MODE%"=="all" if /I not "%BUILD_MODE%"=="setup-only" (
    echo Usage: build.bat [Debug^|Release] [setup-only]
    exit /b 2
)

if /I "%BUILD_MODE%"=="setup-only" if /I not "%CONFIG%"=="Release" (
    echo setup-only is only valid for Release builds.
    exit /b 2
)

set ROOT=%~dp0
pushd "%ROOT%"

where cl.exe >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
        if defined VSINSTALL if exist "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" (
            call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
        )
    )
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo cl.exe was not found. Install Visual Studio Build Tools with the C++ workload.
    popd
    exit /b 1
)

set OUTDIR=build\%CONFIG%
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set COMMON_CFLAGS=/nologo /std:c17 /W4 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 /Iinclude
if /I "%CONFIG%"=="Debug" (
    set CFLAGS=%COMMON_CFLAGS% /Od /Zi /Fd"%OUTDIR%\subscreen.pdb" /MTd /D_DEBUG
) else (
    set CFLAGS=%COMMON_CFLAGS% /O2 /MT /DNDEBUG
)

set SOURCES=src\main.c src\system_monitor.c src\usb_comm.c src\protocol.c src\service.c src\firmware.c
set LIBS=winusb.lib setupapi.lib cfgmgr32.lib pdh.lib psapi.lib iphlpapi.lib ws2_32.lib advapi32.lib powrprof.lib

if /I not "%BUILD_MODE%"=="setup-only" (
    cl.exe %CFLAGS% %SOURCES% /Fo"%OUTDIR%\\" /Fe"%OUTDIR%\subscreen.exe" /link %LIBS%
    set RESULT=%ERRORLEVEL%
    if not "!RESULT!"=="0" (
        popd
        exit /b !RESULT!
    )
) else (
    if not exist "%OUTDIR%\subscreen.exe" (
        echo %OUTDIR%\subscreen.exe was not found. Run build.bat Release before setup-only.
        popd
        exit /b 1
    )
)

if /I "%CONFIG%"=="Release" (
    where rc.exe >nul 2>nul
    if errorlevel 1 (
        echo rc.exe was not found. Install the Windows SDK.
        popd
        exit /b 1
    )

    rc.exe /nologo /i . /fo"%OUTDIR%\setup_resources.res" src\setup_resources.rc
    if errorlevel 1 (
        popd
        exit /b 1
    )

    cl.exe %CFLAGS% src\setup.c "%OUTDIR%\setup_resources.res" /Fo"%OUTDIR%\\" /Fe"%OUTDIR%\HarborSubscreenSetup.exe" /link setupapi.lib cfgmgr32.lib newdev.lib shell32.lib advapi32.lib
    set RESULT=%ERRORLEVEL%
)

popd
exit /b %RESULT%

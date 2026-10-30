@echo off
setlocal enabledelayedexpansion

rem =========================================================================
rem run-toon1-sim.bat - Launch FreeToon Toon 1 (800x480) Simulator
rem =========================================================================
rem Usage:
rem   run-toon1-sim.bat             Interactive GUI window (800x480)
rem   run-toon1-sim.bat dim         Open directly at the dim screen
rem   run-toon1-sim.bat home_stock  Open directly at the classic stock home screen
rem   run-toon1-sim.bat --build     Force rebuild before running
rem   run-toon1-sim.bat all [dir]   Export screenshots of all screens
rem =========================================================================

set SCRIPT_DIR=%~dp0
set FREETOON_DIR=%SCRIPT_DIR%
if "%FREETOON_DIR:~-1%"=="\" set "FREETOON_DIR=%FREETOON_DIR:~0,-1%"

rem Check for WSL
where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] WSL is required to run the FreeToon simulator.
    echo Please ensure WSL Ubuntu and WSLg are installed.
    pause
    exit /b 1
)

set DO_BUILD=0
set ARGS=

:parse_args
if "%~1"=="" goto done_args
if /i "%~1"=="--build" (
    set DO_BUILD=1
    shift
    goto parse_args
)
if /i "%~1"=="-b" (
    set DO_BUILD=1
    shift
    goto parse_args
)
if /i "%~1"=="--help" goto show_help
if /i "%~1"=="-h" goto show_help

set "ARGS=!ARGS! %~1"
shift
goto parse_args

:done_args

rem Convert Windows path to WSL path
for /f "usebackq delims=" %%i in (`wsl -d Ubuntu wslpath -a "%FREETOON_DIR%" 2^>nul`) do set "WSL_DIR=%%i"
if not defined WSL_DIR (
    for /f "usebackq delims=" %%i in (`wsl wslpath -a "%FREETOON_DIR%" 2^>nul`) do set "WSL_DIR=%%i"
)
if not defined WSL_DIR (
    set "DRIVE=!FREETOON_DIR:~0,1!"
    for /f "delims=" %%D in ('powershell -NoProfile -Command "\"!DRIVE!\".ToLower()"') do set "DRIVE_LOWER=%%D"
    set "REST=!FREETOON_DIR:~2!"
    set "REST=!REST:\=/!"
    set "WSL_DIR=/mnt/!DRIVE_LOWER!!REST!"
)

set "SIM_BIN=%FREETOON_DIR%\lvgl_ui_recovered\build-sim1\toonui-sim1"

if not exist "%SIM_BIN%" set DO_BUILD=1

if "!DO_BUILD!"=="1" (
    echo [sim1] Building FreeToon Toon 1 simulator [800x480]...
    wsl -d Ubuntu bash -c "cd '%WSL_DIR%/lvgl_ui_recovered/src' && make TARGET=sim1 -j4"
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

rem Cleanup any stale sim process, socket locks, and ensure /mnt/data exists
wsl -d Ubuntu bash -c "killall -9 toonui-sim1 toonui-sim 2>/dev/null || true; rm -f /tmp/.X0-lock 2>/dev/null || true; [ -L /tmp/.X11-unix ] || (rm -rf /tmp/.X11-unix && ln -s /mnt/wslg/.X11-unix /tmp/.X11-unix 2>/dev/null || true); mkdir -p /mnt/data 2>/dev/null || true" >nul 2>nul

:sim_loop
echo [sim1] Launching FreeToon Toon 1 simulator [800x480]...
wsl -d Ubuntu bash -c "DISPLAY=${DISPLAY:-:0} cd '%WSL_DIR%/lvgl_ui_recovered/build-sim1' && ./toonui-sim1!ARGS!"
set EXIT_CODE=%ERRORLEVEL%

if "%EXIT_CODE%"=="42" (
    echo.
    echo [sim1] UI restart requested — restarting simulator...
    goto sim_loop
)

if not "%EXIT_CODE%"=="0" (
    echo [sim1] Process exited with code %EXIT_CODE%
    pause
)
exit /b %EXIT_CODE%

:show_help
echo =========================================================================
echo FreeToon Toon 1 Simulator Launcher (800x480)
echo =========================================================================
echo Usage:
echo   run-toon1-sim.bat [options] [screen_name]
echo.
echo Examples:
echo   run-toon1-sim.bat             Interactive Toon 1 UI in window
echo   run-toon1-sim.bat dim         Open dim screen
echo   run-toon1-sim.bat home_stock  Open classic Toon look (tiles carousel)
echo   run-toon1-sim.bat thermostat  Open thermostat detail screen
echo   run-toon1-sim.bat --build     Force rebuild before running
echo   run-toon1-sim.bat all C:\tmp  Dump all screen screenshots as PPM
echo =========================================================================
pause
exit /b 0

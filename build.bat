@echo off
rem ============================================================================
rem  VoiceKit build script   (Qt 5.14.2 + MinGW 7.3 64-bit + qmake)
rem  Output: lib\libVoiceKit.a   bin\vk.exe
rem
rem  IMPORTANT: keep this file PURE ASCII.
rem  cmd.exe parses .bat files with the ANSI codepage (936 on a Chinese system),
rem  so UTF-8 Chinese comments turn into garbage instructions such as
rem  'st' is not recognized as an internal or external command.
rem  ============================================================================
setlocal

set QT_DIR=F:\Qt\5.14.2\mingw73_64
set MINGW_DIR=F:\Qt\Tools\mingw730_64

if not exist "%QT_DIR%\bin\qmake.exe" (
    echo [ERROR] qmake not found: %QT_DIR%\bin\qmake.exe
    echo         Edit QT_DIR / MINGW_DIR at the top of this script.
    pause
    exit /b 1
)

rem The project path contains non-ASCII characters. qmake then writes broken
rem paths (????) into the Makefile and the build always fails, so map the
rem project to a drive letter first - that makes every path pure ASCII.
subst Y: /d >nul 2>&1
subst Y: "%~dp0.."
if not exist "Y:\VoiceKit\src\vk\vk.pro" (
    echo [ERROR] subst failed - drive Y: may already be in use.
    echo         Change Y: to another free letter in this script.
    pause
    exit /b 1
)

set PATH=%MINGW_DIR%\bin;%QT_DIR%\bin;%PATH%
cd /d "Y:\VoiceKit"

if not exist build mkdir build
cd build

echo [1/2] qmake ...
qmake.exe ..\VoiceKit.pro
if errorlevel 1 (
    echo qmake FAILED
    pause
    exit /b 1
)

echo [2/2] mingw32-make ...
mingw32-make -j4
if errorlevel 1 (
    echo BUILD FAILED
    pause
    exit /b 1
)

cd /d "%~dp0"
echo.
echo Build OK:
echo   %~dp0lib\libVoiceKit.a
echo   %~dp0bin\vk.exe
echo.
echo To use it, run:  bin\console.bat
echo (The project stays mapped as drive Y: while building; it disappears on reboot.)
pause

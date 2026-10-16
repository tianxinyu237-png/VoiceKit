@echo off
rem Pure ASCII only - see the note in build.bat.
rem Double-click = show overview. For a usable shell run console.bat instead.
setlocal
set QT_DIR=F:\Qt\5.14.2\mingw73_64
set PATH=%QT_DIR%\bin;%PATH%
"%~dp0vk.exe" %*
if "%~1"=="" pause

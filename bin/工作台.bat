@echo off
rem Pure ASCII inside - see the note in build.bat.
rem Double-click this to open the VoiceKit GUI workbench.
setlocal
set QT_DIR=F:\Qt\5.14.2\mingw73_64
set PATH=%QT_DIR%\bin;%~dp0;%PATH%
cd /d "%~dp0"
start "" "%~dp0vk_gui.exe"

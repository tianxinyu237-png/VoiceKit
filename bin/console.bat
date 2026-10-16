@echo off
rem Pure ASCII only - see the note in build.bat.
rem Opens a cmd window with Qt DLLs and vk.exe already on PATH.
setlocal
set QT_DIR=F:\Qt\5.14.2\mingw73_64
set PATH=%QT_DIR%\bin;%~dp0;%PATH%
cd /d "%~dp0"

echo.
echo   VoiceKit console ready. Commands:
echo.
echo     vk info                      config + index overview (354 voices)
echo     vk list zhongli              search a voice by name
echo     vk ping                      is the engine up?
echo.
echo     vk engine start              start engine (first time: 2-5 min)
echo     vk engine status             engine status
echo     vk engine stop               stop engine, free memory
echo.
echo     vk say -c 钟离 -t "契约，必须履行。"
echo     vk script script.txt -o table.json
echo     vk pack --table table.json -o out_pack --spec embedded
echo.
echo   If the engine is not running, vk say will start it (slow first time).
echo.
cmd /k

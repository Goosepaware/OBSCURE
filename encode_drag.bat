@echo off
setlocal
if "%~1"=="" (
  echo Drag a source video onto this file.
  pause
  exit /b 1
)
set "OUT=%~dpn1_obscured.mp4"
echo Encoding: %~1
echo Output:   %OUT%
"%~dp0build\obscure-cli.exe" encode --input "%~1" --output "%OUT%" --config c32_g2_s120 --bitrate 8
echo.
echo Done. Drag the output onto decode_drag.bat to watch the restored preview.
pause

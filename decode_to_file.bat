@echo off
setlocal
if "%~1"=="" (
  echo Drag an obfuscated transport video onto this file.
  pause
  exit /b 1
)
set "OUT=%~dpn1_restored.mp4"
"%~dp0build\obscure-cli.exe" decode --input "%~1" --output "%OUT%" --config c32_g2_s120
echo.
echo Restored video: %OUT%
pause

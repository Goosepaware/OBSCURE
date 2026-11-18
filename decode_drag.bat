@echo off
setlocal
if "%~1"=="" (
  echo Drag an obfuscated transport video onto this file.
  pause
  exit /b 1
)
"%~dp0build\obscure-decode.exe" --file "%~1" --config c32_g2_s120

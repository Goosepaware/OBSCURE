@echo off
setlocal
if "%~1"=="" (
  echo Drag a recording of a player window onto this file.
  pause
  exit /b 1
)
"%~dp0build\obscure-decode.exe" --file "%~1" --config c32_g2_s120 --detect

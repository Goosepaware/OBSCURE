@echo off
call "D:\program\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set CMAKE="D:\program\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% --build "D:\program\OBScure\build" --target transport-player -j 8 || exit /b 1
echo PLAYER_BUILD_OK

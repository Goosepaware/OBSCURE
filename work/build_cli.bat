@echo off
setlocal
call "D:\program\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set CMAKE="D:\program\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NINJA="D:\program\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
%CMAKE% -S "D:\program\OBScure" -B "D:\program\OBScure\build" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM=%NINJA% ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DOpenCV_DIR="D:/program/OBScure/third_party/opencv-install/x64/vc17/staticlib" ^
  -DOpenCV_STATIC=ON || exit /b 1
%CMAKE% --build "D:\program\OBScure\build" -j 8 || exit /b 1
echo CLI_BUILD_OK

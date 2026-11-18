@echo off
setlocal
call "D:\program\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set CMAKE="D:\program\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NINJA="D:\program\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
%CMAKE% -S "D:\program\OBScure\third_party\opencv" -B "D:\program\OBScure\third_party\opencv-build" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM=%NINJA% ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="D:/program/OBScure/third_party/opencv-install" ^
  -DBUILD_LIST=core,imgproc,imgcodecs,calib3d,features2d,objdetect,aruco,videoio ^
  -DOPENCV_EXTRA_MODULES_PATH="D:/program/OBScure/third_party/opencv_contrib/modules" ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF ^
  -DBUILD_opencv_python3=OFF -DBUILD_JAVA=OFF ^
  -DWITH_OPENCL=OFF -DWITH_IPP=OFF -DWITH_TBB=OFF -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF ^
  -DWITH_FFMPEG=OFF -DWITH_MSMF=ON -DWITH_DSHOW=OFF -DWITH_VFW=OFF ^
  -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_OPENEXR=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL || exit /b 1
%CMAKE% --build "D:\program\OBScure\third_party\opencv-build" --target install -j 8 || exit /b 1
echo OPENCV_BUILD_OK

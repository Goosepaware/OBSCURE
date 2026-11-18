cmake_minimum_required(VERSION 3.24)
project(OBScure LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if (MSVC)
    add_compile_options(/utf-8 /W3 /permissive-)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(OPENCV_ROOT "${CMAKE_SOURCE_DIR}/third_party/opencv-install" CACHE PATH
    "OpenCV install prefix")
set(OPENCV_LIB_DIR "${OPENCV_ROOT}/x64/vc17/staticlib")
set(OPENCV_INCLUDE_DIR "${OPENCV_ROOT}/include")
if (NOT EXISTS "${OPENCV_INCLUDE_DIR}/opencv2/core.hpp")
    message(FATAL_ERROR "OpenCV headers not found under ${OPENCV_INCLUDE_DIR}")
endif()

set(OPENCV_LIBS
    "${OPENCV_LIB_DIR}/opencv_aruco4100.lib"
    "${OPENCV_LIB_DIR}/opencv_objdetect4100.lib"
    "${OPENCV_LIB_DIR}/opencv_calib3d4100.lib"
    "${OPENCV_LIB_DIR}/opencv_features2d4100.lib"
    "${OPENCV_LIB_DIR}/opencv_flann4100.lib"
    "${OPENCV_LIB_DIR}/opencv_videoio4100.lib"
    "${OPENCV_LIB_DIR}/opencv_imgcodecs4100.lib"
    "${OPENCV_LIB_DIR}/opencv_imgproc4100.lib"
    "${OPENCV_LIB_DIR}/opencv_core4100.lib"
    "${OPENCV_LIB_DIR}/ittnotify.lib"
    "${OPENCV_LIB_DIR}/libjpeg-turbo.lib"
    "${OPENCV_LIB_DIR}/libopenjp2.lib"
    "${OPENCV_LIB_DIR}/libpng.lib"
    "${OPENCV_LIB_DIR}/zlib.lib"
)

add_executable(obscure-cli
    src/core/layout.cpp
    src/core/key.cpp
    src/core/metadata.cpp
    src/core/border.cpp
    src/core/transform.cpp
    src/core/detect.cpp
    src/io/ffmpeg_writer.cpp
    src/cli/main.cpp
)

target_include_directories(obscure-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENCV_INCLUDE_DIR}
)

target_link_libraries(obscure-cli PRIVATE
    ${OPENCV_LIBS}
    bcrypt
    mfplat
    mfreadwrite
    mfuuid
    ole32
    uuid
    shlwapi
    gdi32
    comctl32
    ws2_32
    advapi32
    user32
    shell32
)

if (MSVC)
    target_compile_options(obscure-cli PRIVATE /Zi)
    target_link_options(obscure-cli PRIVATE /DEBUG)
endif()

set(SPOUT_ROOT "${CMAKE_SOURCE_DIR}/third_party/Spout2/SPOUTSDK")
set(SPOUT_SOURCES
    "${SPOUT_ROOT}/SpoutDirectX/SpoutDX/SpoutDX.cpp"
    "${SPOUT_ROOT}/SpoutGL/Spout.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutCopy.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutDirectX.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutFrameCount.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutGL.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutGLextensions.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutReceiver.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutSender.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutSenderNames.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutSharedMemory.cpp"
    "${SPOUT_ROOT}/SpoutGL/SpoutUtils.cpp"
)

add_executable(obscure-live
    src/live/live_main.cpp
    src/live/wgc_capture.cpp
    src/live/d3d_helpers.cpp
    src/core/layout.cpp
    src/core/key.cpp
    src/core/metadata.cpp
    src/core/border.cpp
    src/core/transform.cpp
    ${SPOUT_SOURCES}
)
target_compile_definitions(obscure-live PRIVATE SPOUT_BUILD_STATIC)
target_include_directories(obscure-live PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENCV_INCLUDE_DIR}
    "${SPOUT_ROOT}/SpoutDirectX/SpoutDX"
    "${SPOUT_ROOT}/SpoutGL"
)
target_link_libraries(obscure-live PRIVATE
    ${OPENCV_LIBS}
    bcrypt
    d3d11 dxgi d3dcompiler dwmapi shcore windowsapp runtimeobject
    opengl32 kernel32 user32 gdi32 winspool comdlg32 comctl32 advapi32 shell32
    ole32 oleaut32 uuid odbc32 odbccp32 d3d9 version winmm psapi
)

add_executable(transport-player
    src/live/transport_player.cpp
    src/live/d3d_helpers.cpp
)
target_include_directories(transport-player PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENCV_INCLUDE_DIR}
)
target_link_libraries(transport-player PRIVATE
    ${OPENCV_LIBS}
    d3d11 dxgi d3dcompiler
    kernel32 user32 gdi32 shell32 ole32 oleaut32 uuid
)

add_executable(obscure-decode
    src/live/decode_main.cpp
    src/live/wgc_capture.cpp
    src/live/d3d_helpers.cpp
    src/core/layout.cpp
    src/core/key.cpp
    src/core/metadata.cpp
    src/core/detect.cpp
    src/core/transform.cpp
)
target_include_directories(obscure-decode PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENCV_INCLUDE_DIR}
)
target_link_libraries(obscure-decode PRIVATE
    ${OPENCV_LIBS}
    bcrypt
    d3d11 dxgi d3dcompiler dwmapi shcore windowsapp runtimeobject
    kernel32 user32 gdi32 shell32 ole32 oleaut32 uuid comdlg32
)

add_library(obscure-vcam SHARED
    src/vcam/obscure_filter.cpp
    third_party/UnityCapture/Source/streams.cpp
)
target_compile_definitions(obscure-vcam PRIVATE _WINDOWS _USRDLL)
target_compile_options(obscure-vcam PRIVATE /permissive /Zc:strictStrings- /UNOMINMAX)
target_include_directories(obscure-vcam PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/UnityCapture/Source"
)
target_link_libraries(obscure-vcam PRIVATE
    strmiids ole32 oleaut32 uuid user32 gdi32 advapi32 winmm version shlwapi
)
target_link_options(obscure-vcam PRIVATE
    "/DEF:${CMAKE_SOURCE_DIR}/third_party/UnityCapture/Source/UnityCaptureFilter.def"
)
set_target_properties(obscure-vcam PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obscure-vcam"
)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi imm32)

add_executable(obscure-gui
    src/gui/main.cpp
    src/gui/gui_app.cpp
    src/gui/gui_settings.cpp
    src/platform/win_util.cpp
    src/core/layout.cpp
    src/core/key.cpp
    src/core/metadata.cpp
    src/core/border.cpp
    src/core/transform.cpp
    src/core/detect.cpp
    src/live/wgc_capture.cpp
    src/live/d3d_helpers.cpp
    src/live/shader_path.cpp
    src/live/encode_pipeline.cpp
    src/live/decode_pipeline.cpp
    src/live/output_window.cpp
    src/live/preview_window.cpp
    src/live/spout_sink.cpp
    src/live/file_source.cpp
    src/live/detection_worker.cpp
    src/live/vcam_sink.cpp
    src/live/vcam_registry.cpp
    src/io/ffmpeg_sink.cpp
    ${SPOUT_SOURCES}
)
target_compile_definitions(obscure-gui PRIVATE SPOUT_BUILD_STATIC)
target_include_directories(obscure-gui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENCV_INCLUDE_DIR}
    "${SPOUT_ROOT}/SpoutDirectX/SpoutDX"
    "${SPOUT_ROOT}/SpoutGL"
)
target_link_libraries(obscure-gui PRIVATE
    imgui
    ${OPENCV_LIBS}
    bcrypt
    d3d11 dxgi d3dcompiler dwmapi shcore imm32
    windowsapp runtimeobject
    mfplat mfreadwrite mfuuid
    opengl32 kernel32 user32 gdi32 winspool comdlg32 comctl32 advapi32 shell32
    ole32 oleaut32 uuid odbc32 odbccp32 d3d9 version winmm psapi
)
add_dependencies(obscure-gui obscure-vcam)
add_custom_command(TARGET obscure-gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:obscure-gui>/shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:obscure-gui>/vcam"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:obscure-vcam>"
        "$<TARGET_FILE_DIR:obscure-gui>/vcam/obscure-vcam.dll"
)

add_executable(spout-smoke
    src/live/spout_smoke.cpp
    ${SPOUT_SOURCES}
)
target_compile_definitions(spout-smoke PRIVATE SPOUT_BUILD_STATIC)
target_include_directories(spout-smoke PRIVATE
    "${SPOUT_ROOT}/SpoutDirectX/SpoutDX"
    "${SPOUT_ROOT}/SpoutGL"
)
target_link_libraries(spout-smoke PRIVATE
    d3d11 dxgi d3dcompiler
    opengl32 kernel32 user32 gdi32 winspool comdlg32 comctl32 advapi32 shell32
    ole32 oleaut32 uuid odbc32 odbccp32 d3d9 version winmm psapi
)

install(TARGETS obscure-cli RUNTIME DESTINATION bin)

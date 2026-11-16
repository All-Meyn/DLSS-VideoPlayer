cmake_minimum_required(VERSION 3.21)
project(DLSSVideoPlayer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DLSS-VideoPlayer targets Windows (Direct3D 11) only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# FFmpeg: point FFMPEG_DIR at a shared ("-shared") Windows build that has
# include/, lib/ and bin/ folders, e.g. a BtbN or gyan.dev "ffmpeg-*-shared" zip.
set(FFMPEG_DIR "" CACHE PATH "Root of a Windows FFmpeg shared build (include/, lib/, bin/)")

# NVIDIA RTX Video SDK (optional). Download from developer.nvidia.com/rtx-video-sdk.
option(DLSSVP_WITH_RTX_VIDEO_SDK "Build the NVIDIA RTX Video SDK upscaler backend" OFF)
set(RTX_VIDEO_SDK_DIR "" CACHE PATH "Root of the extracted NVIDIA RTX Video SDK")

# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------
if(NOT FFMPEG_DIR)
    message(FATAL_ERROR "Set -DFFMPEG_DIR=<path to FFmpeg shared build>. See README.md.")
endif()

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_DIR}/include" REQUIRED NO_DEFAULT_PATH)
foreach(_lib avformat avcodec avutil)
    find_library(FFMPEG_${_lib}_LIB NAMES ${_lib} PATHS "${FFMPEG_DIR}/lib" REQUIRED NO_DEFAULT_PATH)
endforeach()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(DLSSVideoPlayer WIN32
    src/main.cpp
    src/App.cpp
    src/App.h
    src/GpuDevice.cpp
    src/GpuDevice.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/VideoProcessor.cpp
    src/VideoProcessor.h
    src/Renderer.cpp
    src/Renderer.h
    src/Log.h
    src/upscalers/IUpscaler.h
    src/upscalers/RtxVideoSdkUpscaler.cpp
)

target_include_directories(DLSSVideoPlayer PRIVATE src "${FFMPEG_INCLUDE_DIR}")
target_compile_definitions(DLSSVideoPlayer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(DLSSVideoPlayer PRIVATE
    d3d11 dxgi comdlg32 shell32
    ${FFMPEG_avformat_LIB} ${FFMPEG_avcodec_LIB} ${FFMPEG_avutil_LIB})

if(MSVC)
    target_compile_options(DLSSVideoPlayer PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(DLSSVideoPlayer PRIVATE -Wall -Wextra)
endif()

if(DLSSVP_WITH_RTX_VIDEO_SDK)
    if(NOT RTX_VIDEO_SDK_DIR)
        message(FATAL_ERROR "DLSSVP_WITH_RTX_VIDEO_SDK=ON requires -DRTX_VIDEO_SDK_DIR=<path>.")
    endif()
    target_compile_definitions(DLSSVideoPlayer PRIVATE DLSSVP_HAS_RTX_VIDEO_SDK=1)
    # The SDK's exact folder layout and library names should be checked against
    # the version you download; adjust these two lines if they differ.
    target_include_directories(DLSSVideoPlayer PRIVATE "${RTX_VIDEO_SDK_DIR}/include")
    file(GLOB _rtxvsdk_libs "${RTX_VIDEO_SDK_DIR}/lib/*.lib")
    target_link_libraries(DLSSVideoPlayer PRIVATE ${_rtxvsdk_libs})
endif()

# Copy FFmpeg runtime DLLs next to the executable so it runs from the build tree.
file(GLOB _ffmpeg_dlls "${FFMPEG_DIR}/bin/*.dll")
if(_ffmpeg_dlls)
    add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffmpeg_dlls} $<TARGET_FILE_DIR:DLSSVideoPlayer>)
endif()

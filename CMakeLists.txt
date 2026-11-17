cmake_minimum_required(VERSION 3.21)
project(VSRPlayer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VSRPlayer targets Windows (Direct3D 11) only.")
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
# Old name (before the project was renamed to VSRPlayer) still accepted.
if(DEFINED DLSSVP_WITH_RTX_VIDEO_SDK AND NOT DEFINED VSRPLAYER_WITH_RTX_VIDEO_SDK)
    message(STATUS "DLSSVP_WITH_RTX_VIDEO_SDK is now VSRPLAYER_WITH_RTX_VIDEO_SDK; using its value")
    set(VSRPLAYER_WITH_RTX_VIDEO_SDK ${DLSSVP_WITH_RTX_VIDEO_SDK} CACHE BOOL "Build the NVIDIA RTX Video SDK upscaler backend")
endif()
option(VSRPLAYER_WITH_RTX_VIDEO_SDK "Build the NVIDIA RTX Video SDK upscaler backend" OFF)
set(RTX_VIDEO_SDK_DIR "C:/libs/RTXVSR" CACHE PATH "Root of the extracted NVIDIA RTX Video SDK")

# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------
if(NOT FFMPEG_DIR)
    message(FATAL_ERROR "Set -DFFMPEG_DIR=<path to FFmpeg shared build>. See README.md.")
endif()

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_DIR}/include" REQUIRED NO_DEFAULT_PATH)
foreach(_lib avformat avcodec avutil swresample)
    find_library(FFMPEG_${_lib}_LIB NAMES ${_lib} PATHS "${FFMPEG_DIR}/lib" REQUIRED NO_DEFAULT_PATH)
endforeach()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(VSRPlayer WIN32
    src/main.cpp
    src/App.cpp
    src/App.h
    src/AudioDeviceWatcher.cpp
    src/AudioDeviceWatcher.h
    src/AudioPlayer.cpp
    src/AudioPlayer.h
    src/DisplayInfo.cpp
    src/DisplayInfo.h
    src/GpuDevice.cpp
    src/GpuDevice.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/VideoProcessor.cpp
    src/VideoProcessor.h
    src/Overlay.cpp
    src/Overlay.h
    src/OutputPass.cpp
    src/OutputPass.h
    src/PlaybackHistory.cpp
    src/PlaybackHistory.h
    src/Renderer.cpp
    src/Renderer.h
    src/Settings.cpp
    src/Settings.h
    src/Subtitles.cpp
    src/Subtitles.h
    src/Log.h
    src/upscalers/IUpscaler.h
    src/upscalers/RtxVideoSdkUpscaler.cpp
)

target_include_directories(VSRPlayer PRIVATE src)
# SYSTEM: third-party headers, so their warnings don't drown out ours.
target_include_directories(VSRPlayer SYSTEM PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_compile_definitions(VSRPlayer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(VSRPlayer PRIVATE
    d3d11 dxgi d2d1 dwrite d3dcompiler comdlg32 shell32 ole32 uuid
    ${FFMPEG_avformat_LIB} ${FFMPEG_avcodec_LIB} ${FFMPEG_avutil_LIB} ${FFMPEG_swresample_LIB})

if(MSVC)
    target_compile_options(VSRPlayer PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(VSRPlayer PRIVATE -Wall -Wextra)
endif()

if(VSRPLAYER_WITH_RTX_VIDEO_SDK)
    # The SDK ships x64 and ARM64 (and possibly x86) builds of the same files.
    # Keep only the x64 ones: prefer paths with an x64 folder, else drop ARM64/x86 paths.
    function(vsrplayer_keep_x64 list_var)
        set(_all ${${list_var}})
        set(_x64 ${_all})
        list(FILTER _x64 INCLUDE REGEX "[/\\\\_]([xX]64|x86_64|[aA][mM][dD]64)[/\\\\]")
        if(NOT _x64)
            set(_x64 ${_all})
            list(FILTER _x64 EXCLUDE REGEX "[aA][rR][mM]64|[/\\\\]([xX]86|[wW]in32)[/\\\\]")
        endif()
        set(${list_var} ${_x64} PARENT_SCOPE)
    endfunction()

    if(NOT RTX_VIDEO_SDK_DIR)
        message(FATAL_ERROR "VSRPLAYER_WITH_RTX_VIDEO_SDK=ON requires -DRTX_VIDEO_SDK_DIR=<path>.")
    endif()
    target_compile_definitions(VSRPlayer PRIVATE VSRPLAYER_HAS_RTX_VIDEO_SDK=1)

    # Headers: wherever nvsdk_ngx_helpers_vsr.h lives inside the SDK.
    file(GLOB_RECURSE _ngx_hdr "${RTX_VIDEO_SDK_DIR}/nvsdk_ngx_helpers_vsr.h")
    if(NOT _ngx_hdr)
        message(FATAL_ERROR "nvsdk_ngx_helpers_vsr.h not found under RTX_VIDEO_SDK_DIR=${RTX_VIDEO_SDK_DIR}")
    endif()
    list(GET _ngx_hdr 0 _ngx_hdr)
    get_filename_component(_ngx_inc "${_ngx_hdr}" DIRECTORY)
    target_include_directories(VSRPlayer SYSTEM PRIVATE "${_ngx_inc}")

    # Import library: nvsdk_ngx_d.lib (dynamic CRT, matches CMake's default /MD).
    # Override with -DRTX_VIDEO_SDK_LIB=<path> if your SDK names it differently.
    if(NOT RTX_VIDEO_SDK_LIB)
        file(GLOB_RECURSE _ngx_libs "${RTX_VIDEO_SDK_DIR}/nvsdk_ngx_d.lib")
        vsrplayer_keep_x64(_ngx_libs)
        if(NOT _ngx_libs)
            message(FATAL_ERROR "nvsdk_ngx_d.lib not found under ${RTX_VIDEO_SDK_DIR}; set -DRTX_VIDEO_SDK_LIB=<path to .lib>")
        endif()
        list(GET _ngx_libs 0 RTX_VIDEO_SDK_LIB)
    endif()
    message(STATUS "RTX Video SDK: include ${_ngx_inc}, lib ${RTX_VIDEO_SDK_LIB}")
    target_link_libraries(VSRPlayer PRIVATE "${RTX_VIDEO_SDK_LIB}")

    # Runtime: NGX loads its feature DLLs (nvngx_*.dll) from the executable's folder.
    # Copy the release DLLs (skip any "dev"/debug builds) next to the exe.
    file(GLOB_RECURSE _ngx_dlls "${RTX_VIDEO_SDK_DIR}/*.dll")
    list(FILTER _ngx_dlls INCLUDE REGEX "nvngx")
    list(FILTER _ngx_dlls EXCLUDE REGEX "[/\\\\]dev[/\\\\]")
    vsrplayer_keep_x64(_ngx_dlls)
    if(_ngx_dlls)
        message(STATUS "RTX Video SDK runtime DLLs: ${_ngx_dlls}")
        add_custom_command(TARGET VSRPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ngx_dlls} $<TARGET_FILE_DIR:VSRPlayer>)
    else()
        message(WARNING "No nvngx*.dll found in the RTX Video SDK; copy the VSR runtime DLL next to the exe manually.")
    endif()
endif()

# Copy FFmpeg runtime DLLs next to the executable so it runs from the build tree.
file(GLOB _ffmpeg_dlls "${FFMPEG_DIR}/bin/*.dll")
if(_ffmpeg_dlls)
    add_custom_command(TARGET VSRPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffmpeg_dlls} $<TARGET_FILE_DIR:VSRPlayer>)
endif()

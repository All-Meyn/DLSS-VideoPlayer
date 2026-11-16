# DLSS-VideoPlayer

A Windows video player that upscales video with NVIDIA's AI super resolution on RTX GPUs.

> **Note on "DLSS":** DLSS Super Resolution is designed for games. It needs per-pixel
> motion vectors, depth and camera jitter that decoded video does not have. NVIDIA's
> AI upscaler for video is **RTX Video Super Resolution (VSR)**, which is what this
> player uses. The upscaler sits behind an interface (`src/upscalers/IUpscaler.h`), so
> other backends (DLSS, FSR, custom shaders) can be added later.

## Status: milestone 1

| Feature | State |
|---|---|
| Win32 window, drag & drop, open dialog, borderless fullscreen | done |
| FFmpeg demux + D3D11VA GPU decode (H.264, HEVC, VP9, AV1 - whatever your GPU decodes) | done |
| YUV->RGB, scaling, aspect ratio / letterboxing via D3D11 Video Processor | done |
| **RTX VSR via NVIDIA driver** (D3D11 stream extension, the same path Chrome/VLC use) | done |
| **RTX Video SDK backend** | integration point in place; SDK calls still to be wired (`TODO(SDK)` in `src/upscalers/RtxVideoSdkUpscaler.cpp`) |
| Play / pause / seek, frame pacing to vsync | done |
| Audio, A/V sync | not yet (milestone 2) |
| HDR output, interlaced content, software-decode fallback | not yet |

## Requirements

- Windows 10/11 x64
- NVIDIA GeForce RTX 20-series or newer, recent Game Ready / Studio driver
- NVIDIA Control Panel -> *Adjust video image settings* -> **RTX video enhancement -> Super resolution** enabled
  (required for the driver VSR path)
- Visual Studio 2022 (Desktop development with C++) and CMake 3.21+
- FFmpeg **shared** build for Windows (headers + import libs + DLLs), FFmpeg 6.x or 7.x,
  e.g. from https://github.com/BtbN/FFmpeg-Builds/releases (`ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip`)
  or https://www.gyan.dev/ffmpeg/builds/ (`ffmpeg-release-full-shared`)
- Optional: NVIDIA RTX Video SDK (https://developer.nvidia.com/rtx-video-sdk)

## Build

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFFMPEG_DIR=C:/libs/ffmpeg-7.1-shared
cmake --build build --config Release
build\Release\DLSSVideoPlayer.exe path\to\video.mkv
```

With the RTX Video SDK (once the `TODO(SDK)` calls are filled in):

```bat
cmake -S . -B build -A x64 -DFFMPEG_DIR=C:/libs/ffmpeg-7.1-shared ^
      -DDLSSVP_WITH_RTX_VIDEO_SDK=ON -DRTX_VIDEO_SDK_DIR=C:/Users/<you>/Downloads/RTX_Video_SDK_v1.1.0
```

The FFmpeg DLLs are copied next to the executable after each build. The RTX Video SDK may also
ship runtime DLLs that need to sit next to the executable; check its documentation.

## Controls

| Key | Action |
|---|---|
| `O` / drag & drop | Open a file |
| `Space` | Play / pause (at end: replay) |
| `Left` / `Right` | Seek -5 s / +5 s (`Shift`: 30 s) |
| `U` | Cycle upscaler: Off -> RTX VSR (driver) -> RTX Video SDK (if built) |
| `1`-`4` | Quality level (RTX Video SDK backend) |
| `F`, `Alt+Enter`, double-click | Toggle fullscreen (`Esc` leaves) |

The title bar shows source -> output resolution, requested -> actually used upscaler, time and state.
Super resolution only runs when the video is being **enlarged**: e.g. a 1080p video in a 1080p window
falls back to "Off". Use fullscreen on a 1440p/4K display, or play 480p-1080p content, to see it.
Diagnostics go to the debugger output (Visual Studio Output window or Sysinternals DebugView).

## Architecture

```
 file --> VideoDecoder (libavformat + libavcodec, D3D11VA) --> NV12/P010 texture array slice
                                                                    |
                                     Renderer (per new frame / resize / setting change)
                     +----------------------------------+-------------------------------+
         mode Off / DriverVsr                                         mode RtxVideoSdk
  VideoProcessor: YUV->RGB + scale into                  VideoProcessor: YUV->RGB at source size
  window-sized "present" texture (+ NVIDIA                 -> IUpscaler (RTX Video SDK) -> output
  VSR stream extension when DriverVsr)                     -> copy into letterboxed "present" texture
                     +----------------------------------+-------------------------------+
                                                                    |
                          every vsync: CopyResource(present -> back buffer), Present(1)
```

- `src/App.*`: window, input, playback clock, main loop (single UI thread; decode on demand, one frame ahead)
- `src/GpuDevice.*`: D3D11 device (prefers NVIDIA adapter), video interfaces, flip-model swap chain
- `src/VideoDecoder.*`: FFmpeg demux/decode with zero-copy D3D11 surfaces on the shared device
- `src/VideoProcessor.*`: D3D11 Video Processor wrapper, color spaces, NVIDIA driver VSR extension
- `src/Renderer.*`: picks the upscaling path, letterboxing, presentation
- `src/upscalers/`: pluggable texture-to-texture upscalers (RTX Video SDK backend)

## Licensing

FFmpeg is used as shared DLLs. Use an LGPL build if you plan to distribute the player.
The NVIDIA RTX Video SDK has its own license and is not included in this repository.

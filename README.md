# VSRPlayer

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
| **RTX Video SDK backend** (NGX VSR feature, quality levels 1-4) | done, used by default when it initializes |
| Play / pause / seek, frame pacing to vsync | done |
| Audio (WASAPI, any codec/channel layout via libswresample), audio-master A/V sync, volume/mute | done |
| On-screen controls (Direct2D/DirectWrite): seek bar, play/pause, volume, upscaler/quality, fullscreen, auto-hide, status messages | done |
| HDR10 output on HDR displays (Windows HDR on): 10-bit PQ/BT.2020 end to end, VSR on HDR, HDR metadata, overlay at SDR white | done |
| 10-bit SDR precision: 10-bit sources stay 10-bit through VSR and output (10-bit SDR swap chain) | done |
| RTX Video HDR (SDK TrueHDR): SDR video converted to HDR10 on HDR displays after VSR; on by default, `H` / RTX HDR chip toggles | done |
| HDR10 -> SDR tone mapping (BT.2390 EETF on max RGB, content peak from MaxCLL / mastering metadata -> 203 nits, 10-bit output) when Windows HDR is off | done |
| RTX Video HDR tuning panel (`T`): contrast, saturation, middle gray; saved in `%APPDATA%\\VSRPlayer\\settings.ini` | done |
| Audio track selection (`A` / Audio menu) | done |
| Subtitles: embedded text (SRT/ASS/WebVTT/mov_text, shown as plain outlined text) and image (PGS/VobSub) tracks, external `.srt/.ass/.ssa/.vtt` next to the video; `S` / Subs menu | done |
| Robustness: audio follows default-device changes / device loss; GPU device reset recovery (rebuild + resume) | done |
| ASS styling (libass), HLG, interlaced content, software-decode fallback | not yet |

## Requirements

- Windows 10/11 x64
- NVIDIA GeForce RTX 20-series or newer, recent Game Ready / Studio driver
- NVIDIA Control Panel -> *Adjust video image settings* -> **RTX video enhancement -> Super resolution** enabled
  (required for the driver VSR path)
- Visual Studio 2022 or 2026, or just the free Build Tools, with the "Desktop development with C++" workload; CMake 3.21+
  (CMake 4.2+ for the Visual Studio 2026 generator)
- FFmpeg **shared** build for Windows (headers + import libs + DLLs), FFmpeg 6.x or 7.x,
  e.g. from https://github.com/BtbN/FFmpeg-Builds/releases (`ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip`)
  or https://www.gyan.dev/ffmpeg/builds/ (`ffmpeg-release-full-shared`)
- Optional: NVIDIA RTX Video SDK (https://developer.nvidia.com/rtx-video-sdk)

## Build

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DFFMPEG_DIR=C:/libs/ffmpeg
REM Visual Studio / Build Tools 2022: use -G "Visual Studio 17 2022" instead
cmake --build build --config Release
build\Release\VSRPlayer.exe path\to\video.mkv
```

With the RTX Video SDK (v1.1):

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DFFMPEG_DIR=C:/libs/ffmpeg -DVSRPLAYER_WITH_RTX_VIDEO_SDK=ON
REM The SDK is expected at C:/libs/RTXVSR (the extracted SDK folder); override with -DRTX_VIDEO_SDK_DIR=<path>
```

The FFmpeg DLLs are copied next to the executable after each build. With the SDK enabled, CMake also
finds the NGX headers (`nvsdk_ngx_helpers_vsr.h`), links `nvsdk_ngx_d.lib`, and copies the SDK's
`nvngx*.dll` runtime files next to the executable; the configure output prints what it found. If your
SDK names the library differently, pass `-DRTX_VIDEO_SDK_LIB=<path to .lib>`.
NGX writes its logs to `%TEMP%\VSRPlayer`.

## Controls

| Key | Action |
|---|---|
| `O` / drag & drop | Open a file |
| `Space` | Play / pause (at end: replay) |
| `Left` / `Right` | Seek -5 s / +5 s (`Shift`: 30 s) |
| `U` | Toggle upscaler: Off <-> RTX Video SDK when the SDK is active; otherwise Off <-> RTX VSR (driver) |
| `1`-`4` | VSR quality: Low / Medium / High / Ultra (RTX Video SDK backend) |
| `F`, `Alt+Enter`, double-click | Toggle fullscreen (`Esc` leaves) |
| `Up` / `Down` | Volume +/- 5% |
| `M` | Mute / unmute |
| `H` | RTX Video HDR on/off (SDR videos on a display with Windows HDR on) |
| `A` / `S` | Next audio track / next subtitle track (incl. off); also the Audio and Subs buttons on the control bar |
| `T` | RTX Video HDR tuning panel (arrows select/adjust, Shift = fine steps, Esc/T closes; or drag the sliders) |
| Mouse | Move to show the control bar (hides after 2.5 s; cursor hides too in fullscreen). Click/drag the seek bar (hover shows the time), drag the volume slider, click the buttons. Wheel = volume. Double-click the video = fullscreen. |

The title bar shows source -> output resolution, requested -> actually used upscaler, time and state.
Super resolution only runs when the video is being **enlarged**: e.g. a 1080p video in a 1080p window
falls back to "Off". Use fullscreen on a 1440p/4K display, or play 480p-1080p content, to see it.
Diagnostics go to `%TEMP%\VSRPlayer\VSRPlayer.log` (recreated on each start) and to the debugger
output (VS Code Debug Console, Visual Studio Output window or Sysinternals DebugView).
Run with `--no-sdk` to skip the RTX Video SDK backend (driver VSR / Off only).
Run with `--d3d-debug` to enable the Direct3D 11 debug layer (needs the Windows "Graphics Tools" optional
feature); its messages appear in the debugger output / DebugView. If the GPU driver resets the device, the
player logs the reason and stops rendering; restart it.

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

- `src/App.*`: window, input, playback clock, main loop (UI thread; video decode on demand, one frame ahead)
- `src/AudioPlayer.*`: audio thread with its own reader of the file, decoder, libswresample and WASAPI output;
  provides the master clock that video follows
- `src/GpuDevice.*`: D3D11 device (prefers NVIDIA adapter), video interfaces, flip-model swap chain
- `src/VideoDecoder.*`: FFmpeg demux/decode with zero-copy D3D11 surfaces on the shared device
- `src/VideoProcessor.*`: D3D11 Video Processor wrapper, color spaces, NVIDIA driver VSR extension
- `src/Renderer.*`: picks the upscaling path, letterboxing, presentation
- `src/Overlay.*`: control bar and on-screen messages drawn with Direct2D/DirectWrite (onto the back buffer in SDR,
  offscreen in HDR)
- `src/OutputPass.*`: HDR10 output shader (compiled at startup with d3dcompiler_47): writes the PQ video to the
  HDR10 swap chain and blends the overlay in at the Windows SDR-white level
- `src/DisplayInfo.*`: monitor HDR state / peak / SDR white level, HDR10 metadata from the file
- `src/upscalers/`: pluggable texture-to-texture upscalers (RTX Video SDK / NGX VSR backend)

## Licensing

FFmpeg is used as shared DLLs. Use an LGPL build if you plan to distribute the player.
The NVIDIA RTX Video SDK has its own license and is not included in this repository.

#pragma once

#include "GpuDevice.h"
#include "Renderer.h"
#include "VideoDecoder.h"

#include <string>

// Window, input, playback clock and the main loop.
//
// Threading: everything (Win32 messages, FFmpeg demux/decode, D3D11 rendering) runs on
// the UI thread. Decode happens on demand, at most one frame ahead of the one shown.
class App
{
public:
    struct Options
    {
        std::wstring initialFile;
        bool d3dDebug = false; // --d3d-debug: enable the D3D11 debug layer
        bool noSdk = false;    // --no-sdk: don't create the RTX Video SDK backend
    };
    int Run(HINSTANCE instance, int showCmd, const Options& options);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void OnKeyDown(WPARAM key);

    void OpenFile(const std::wstring& path);
    void ShowOpenDialog();
    void CloseVideo();

    void TogglePause();
    void SeekTo(double seconds);
    void SeekBy(double deltaSeconds);
    void CycleUpscaleMode();
    void ToggleFullscreen();

    bool AdvancePlayback(); // returns true if the displayed frame changed
    void DecodeNext();
    void RenderNow();
    void LogUpscalerStateIfChanged();
    void UpdateTitle(bool force);
    bool HandleDeviceLost();
    void UpdateKeepAwake();

    double Now() const;
    double MediaTime() const;
    void AnchorClock(double mediaTime);

    HWND m_hwnd = nullptr;
    GpuDevice m_gpu;
    Renderer m_renderer;
    VideoDecoder m_decoder;
    bool m_initialized = false;
    bool m_deviceLost = false; // GPU device removed: rendering stops until restart

    // Playback state
    VideoFrame m_current;
    VideoFrame m_next;
    bool m_hasVideo = false;
    bool m_paused = false;
    bool m_eof = false;
    bool m_dirty = true;
    bool m_minimized = false;
    bool m_keepAwake = false; // display/system sleep blocked while playing
    std::wstring m_fileName;

    // Clock: media time = anchorMedia + (now - anchorTime) while playing.
    double m_qpcToSeconds = 0.0;
    double m_anchorMedia = 0.0;
    double m_anchorTime = 0.0;

    // Upscaling
    UpscaleMode m_mode = UpscaleMode::DriverVsr;
    UpscaleMode m_effectiveMode = UpscaleMode::Off;
    int m_quality = 4;
    // Last upscaler state written to the log (-1 = nothing logged yet for this file).
    int m_loggedMode = -1;
    int m_loggedEffectiveMode = -1;
    int m_loggedQuality = -1;

    // Window state
    bool m_fullscreen = false;
    WINDOWPLACEMENT m_windowedPlacement{sizeof(WINDOWPLACEMENT)};
    double m_lastTitleUpdate = 0.0;
};

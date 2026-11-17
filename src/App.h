#pragma once

#include "AudioPlayer.h"
#include "DisplayInfo.h"
#include "GpuDevice.h"
#include "Overlay.h"
#include "OutputPass.h"
#include "Renderer.h"
#include "Settings.h"
#include "VideoDecoder.h"

#include <string>

// Window, input, playback clock and the main loop.
//
// Threading: Win32 messages, video demux/decode and D3D11 rendering run on the UI thread;
// video decode happens on demand, at most one frame ahead of the one shown. Audio runs on
// AudioPlayer's own thread and is the master clock whenever the file has audio.
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
    bool SeekVideo(double seconds); // video only; false if the video was lost
    void SeekBy(double deltaSeconds);
    void CycleUpscaleMode();
    void ToggleFullscreen();
    void ChangeVolume(float delta);
    void SetVolume(float volume);
    void ToggleMute();
    void ApplyVolume();
    void SetQuality(int quality);
    void ToggleTrueHdr();
    void ToggleTunePanel();
    void SetTuneValue(int row, unsigned value);
    void ApplyTrueHdrSettings();

    // On-screen controls
    void OnMouseMove(int x, int y);
    void OnMouseDown(int x, int y);
    void OnMouseUp();
    void UpdateOverlay();             // fade / auto-hide / cursor, once per loop iteration
    bool BarWanted(double now) const; // should the control bar be visible?
    bool OverlayAnimating() const;    // needs redraws even when paused
    float OsdOpacity(double now) const;
    OverlayState BuildOverlayState();
    void ShowOsd(const wchar_t* format, ...);
    void ShowSeekOsd();
    void PresentFrame(); // back buffer <- video frame, overlay on top, Present

    // SDR or HDR10 output, from the current file and the monitor the window is on.
    void UpdateOutputMode();

    bool AdvancePlayback(); // returns true if the displayed frame changed
    void DecodeNext();
    void RenderNow();
    void LogUpscalerStateIfChanged();
    void UpdateTitle(bool force);
    bool HandleDeviceLost();
    void UpdateKeepAwake();

    double Now() const;
    double MediaTime(); // audio clock when available, otherwise the QPC clock
    void AnchorClock(double mediaTime);

    HWND m_hwnd = nullptr;
    GpuDevice m_gpu;
    Renderer m_renderer;
    Overlay m_overlay;
    OutputPass m_outputPass;
    VideoDecoder m_decoder;
    AudioPlayer m_audio;
    bool m_initialized = false;
    bool m_deviceLost = false; // GPU device removed: rendering stops until restart
    DisplayInfo m_display;
    OutputMode m_outputMode = OutputMode::Sdr8;
    bool m_trueHdrEnabled = true; // RTX Video HDR for SDR videos on HDR displays (H toggles)
    bool m_trueHdrOutput = false; // current output is SDR content converted by RTX Video HDR
    bool m_contentIsHdr10 = false; // current file is HDR10 (RTX Video HDR doesn't apply)
    DXGI_FORMAT m_sdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool m_pqVideo = false;          // HDR10 source kept as PQ in the presentation texture
    bool m_toneMap = false;          // ...and tone-mapped to SDR by OutputPass (Windows HDR off)
    float m_contentPeakNits = 1000.0f;
    unsigned m_thdrMaxLuminance = 1000;
    Settings m_settings;             // persisted (RTX Video HDR tuning)
    bool m_tuneOpen = false;         // RTX Video HDR tuning panel (T)
    int m_tuneRow = 0;
    int m_draggingTune = -1;

    // Playback state
    VideoFrame m_current;
    VideoFrame m_next;
    bool m_hasVideo = false;
    bool m_hasAudio = false;
    float m_volume = 1.0f; // 0..1, kept across files
    bool m_muted = false;
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

    // On-screen controls state
    bool m_overlayDirty = false; // overlay changed: present even if the video frame didn't
    float m_barOpacity = 0.0f;
    double m_lastOverlayUpdate = 0.0;
    double m_lastMouseMove = -1e9;
    int m_mouseX = -1;
    int m_mouseY = -1;
    bool m_mouseInside = false;
    bool m_trackingMouse = false;
    bool m_cursorHidden = false;
    OverlayPart m_hover = OverlayPart::None;
    bool m_draggingSeek = false;
    bool m_draggingVolume = false;
    double m_seekPreview = -1.0;
    wchar_t m_osdText[96] = {};
    double m_osdUntil = 0.0;
};

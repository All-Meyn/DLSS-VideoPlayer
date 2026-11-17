#pragma once

#include "GpuDevice.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

enum class OverlayPart
{
    None,       // video area (not on the bar)
    Bar,        // empty part of the control bar
    Seek,
    PlayPause,
    Mute,
    Volume,
    Upscaler,
    Quality,
    Fullscreen,
};

// Everything the overlay needs to draw one frame. Filled by App; plain data, no ownership.
struct OverlayState
{
    float barOpacity = 0.0f; // 0 = hidden .. 1 = fully shown
    bool paused = false;
    bool hasAudio = false;
    bool muted = false;
    bool fullscreen = false;
    double position = 0.0; // seconds
    double duration = 0.0; // seconds (0 = unknown)
    double seekPreview = -1.0; // seconds under the mouse / being dragged, -1 = none
    bool draggingSeek = false;
    float volume = 1.0f;
    const wchar_t* upscalerLabel = L"";
    bool upscalerOn = false;
    bool showQuality = false;
    int quality = 4;
    OverlayPart hover = OverlayPart::None;
    const wchar_t* osdText = nullptr; // centered status message
    float osdOpacity = 0.0f;
};

// Player control bar + on-screen messages drawn with Direct2D / DirectWrite straight onto
// the swap chain's back buffer (after the video frame was copied in, before Present).
// Layout is in DIPs, so it scales with the monitor's DPI. UI thread only.
class Overlay
{
public:
    bool Init(GpuDevice* gpu, HWND hwnd);
    bool IsAvailable() const { return m_context != nullptr; }

    // Must be called before the swap chain is resized (the target holds the back buffer).
    void ReleaseTarget();
    void OnDpiChanged();

    void Draw(const OverlayState& state);

    // Hit testing in client-area pixels against the current layout.
    OverlayPart HitTest(int x, int y) const;
    double SeekFraction(int x) const;   // 0..1 along the seek bar
    float VolumeFraction(int x) const;  // 0..1 along the volume slider

private:
    struct Layout
    {
        float width = 0, height = 0; // DIPs
        D2D1_RECT_F gradient{}, bar{};
        D2D1_RECT_F seekTrack{}, seekHit{};
        D2D1_RECT_F play{}, mute{}, volumeTrack{}, volumeHit{}, time{};
        D2D1_RECT_F upscaler{}, quality{}, fullscreen{};
    };

    bool EnsureTarget();
    Layout ComputeLayout() const;
    float Scale() const { return m_dpi / 96.0f; }

    void Fill(const D2D1_RECT_F& r, UINT32 rgb, float alpha, float radius = 0.0f);
    void Text(const wchar_t* text, IDWriteTextFormat* format, const D2D1_RECT_F& r, UINT32 rgb, float alpha);

    GpuDevice* m_gpu = nullptr;
    HWND m_hwnd = nullptr;
    float m_dpi = 96.0f;

    ComPtr<ID2D1Factory1> m_factory;
    ComPtr<ID2D1Device> m_device;
    ComPtr<ID2D1DeviceContext> m_context;
    ComPtr<ID2D1Bitmap1> m_target;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<ID2D1LinearGradientBrush> m_gradient;

    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<IDWriteTextFormat> m_textLeft;
    ComPtr<IDWriteTextFormat> m_textCenter;
    ComPtr<IDWriteTextFormat> m_icons;
    ComPtr<IDWriteTextFormat> m_osd;

    float m_opacity = 1.0f; // current bar opacity, applied by Fill/Text
    bool m_lastHasAudio = false;    // from the last Draw, for hit testing
    bool m_lastShowQuality = false; // from the last Draw, for hit testing
};

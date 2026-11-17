#include "Overlay.h"

#include "Log.h"

#include <algorithm>
#include <cwchar>

namespace
{
constexpr UINT32 kWhite = 0xFFFFFF;
constexpr UINT32 kBlack = 0x000000;
constexpr UINT32 kAccent = 0x76B900; // NVIDIA green

// Segoe MDL2 Assets glyphs (ships with Windows 10 and 11).
constexpr wchar_t kIconPlay[] = L"";
constexpr wchar_t kIconPause[] = L"";
constexpr wchar_t kIconVolume[] = L"";
constexpr wchar_t kIconMute[] = L"";
constexpr wchar_t kIconFullscreen[] = L"";
constexpr wchar_t kIconWindowed[] = L"";

void FormatTime(double s, wchar_t* out, size_t n)
{
    if (s < 0)
        s = 0;
    const int total = static_cast<int>(s);
    swprintf(out, n, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

bool Contains(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float Fraction(const D2D1_RECT_F& track, float x)
{
    const float w = track.right - track.left;
    return w > 0.0f ? std::clamp((x - track.left) / w, 0.0f, 1.0f) : 0.0f;
}
} // namespace

bool Overlay::Init(GpuDevice* gpu, HWND hwnd)
{
    m_gpu = gpu;
    m_hwnd = hwnd;
    const UINT dpi = GetDpiForWindow(hwnd);
    m_dpi = dpi ? static_cast<float>(dpi) : 96.0f;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_factory.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = gpu->Device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr))
        hr = m_factory->CreateDevice(dxgiDevice.Get(), &m_device);
    if (SUCCEEDED(hr))
        hr = m_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_context);
    if (SUCCEEDED(hr))
        hr = m_context->CreateSolidColorBrush(D2D1::ColorF(kWhite), &m_brush);
    if (SUCCEEDED(hr))
    {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(kBlack, 0.0f)}, {1.0f, D2D1::ColorF(kBlack, 0.8f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        hr = m_context->CreateGradientStopCollection(stops, 2, &collection);
        if (SUCCEEDED(hr))
            hr = m_context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)), collection.Get(),
                &m_gradient);
    }
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));

    auto makeFormat = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, bool center,
                          ComPtr<IDWriteTextFormat>& out) {
        if (FAILED(hr))
            return;
        hr = m_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"en-us", &out);
        if (SUCCEEDED(hr))
        {
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 13.0f, false, m_textLeft);
    makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, true, m_textCenter);
    makeFormat(L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 16.0f, true, m_icons);
    makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f, true, m_osd);

    if (FAILED(hr))
    {
        LogF("Overlay: Direct2D/DirectWrite setup failed (0x%08lx); on-screen controls disabled",
             static_cast<unsigned long>(hr));
        m_context.Reset();
        return false;
    }
    m_context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // target has no usable alpha
    return true;
}

void Overlay::ReleaseTarget()
{
    if (m_context)
        m_context->SetTarget(nullptr);
    m_target.Reset();
    m_offscreenSrv.Reset(); // window-sized: recreated after a resize
    m_offscreenTex.Reset();
}

void Overlay::SetOffscreen(bool offscreen)
{
    ReleaseTarget();
    m_offscreen = offscreen;
}

void Overlay::OnDpiChanged()
{
    const UINT dpi = GetDpiForWindow(m_hwnd);
    m_dpi = dpi ? static_cast<float>(dpi) : 96.0f;
    ReleaseTarget(); // re-created with the new DPI on the next draw
}

bool Overlay::EnsureTarget()
{
    if (m_target)
        return true;
    ID3D11Texture2D* targetTexture = m_gpu->BackBuffer();
    if (m_offscreen)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_gpu->Width();
        td.Height = m_gpu->Height();
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_gpu->Device()->CreateTexture2D(&td, nullptr, &m_offscreenTex)) ||
            FAILED(m_gpu->Device()->CreateShaderResourceView(m_offscreenTex.Get(), nullptr, &m_offscreenSrv)))
        {
            LogF("Overlay: creating the offscreen texture failed");
            m_offscreenTex.Reset();
            return false;
        }
        targetTexture = m_offscreenTex.Get();
    }
    if (!targetTexture)
        return false;
    ComPtr<IDXGISurface> surface;
    if (FAILED(targetTexture->QueryInterface(IID_PPV_ARGS(&surface))))
        return false;

    // Back buffer: opaque SDR. Offscreen: premultiplied alpha for blending in OutputPass.
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
        m_offscreen ? D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                    : D2D1::PixelFormat(GpuDevice::kBackBufferFormat, D2D1_ALPHA_MODE_IGNORE),
        m_dpi, m_dpi);
    const HRESULT hr = m_context->CreateBitmapFromDxgiSurface(surface.Get(), &props, &m_target);
    if (FAILED(hr))
    {
        LogF("Overlay: CreateBitmapFromDxgiSurface failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    m_context->SetTarget(m_target.Get());
    m_context->SetDpi(m_dpi, m_dpi);
    return true;
}

Overlay::Layout Overlay::ComputeLayout() const
{
    Layout l;
    const float s = Scale();
    l.width = static_cast<float>(m_gpu->Width()) / s;
    l.height = static_cast<float>(m_gpu->Height()) / s;
    const float w = l.width, h = l.height;

    l.gradient = {0.0f, h - 140.0f, w, h};
    l.bar = {0.0f, h - 84.0f, w, h};

    const float seekY = h - 62.0f;
    l.seekTrack = {16.0f, seekY - 3.0f, w - 16.0f, seekY + 3.0f};
    l.seekHit = {8.0f, seekY - 12.0f, w - 8.0f, seekY + 12.0f};

    const float cy = h - 28.0f;
    const float b = 36.0f;
    auto button = [&](float x) { return D2D1_RECT_F{x, cy - b / 2, x + b, cy + b / 2}; };
    l.play = button(12.0f);
    l.mute = button(52.0f);
    l.volumeTrack = {94.0f, cy - 2.0f, 184.0f, cy + 2.0f};
    l.volumeHit = {88.0f, cy - 12.0f, 190.0f, cy + 12.0f};
    l.time = {204.0f, cy - 12.0f, 204.0f + 220.0f, cy + 12.0f};
    l.fullscreen = button(w - 12.0f - b);
    l.quality = {l.fullscreen.left - 8.0f - 44.0f, cy - 14.0f, l.fullscreen.left - 8.0f, cy + 14.0f};
    l.upscaler = {l.quality.left - 8.0f - 104.0f, cy - 14.0f, l.quality.left - 8.0f, cy + 14.0f};
    return l;
}

void Overlay::Fill(const D2D1_RECT_F& r, UINT32 rgb, float alpha, float radius)
{
    m_brush->SetColor(D2D1::ColorF(rgb, alpha * m_opacity));
    if (radius > 0.0f)
        m_context->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), m_brush.Get());
    else
        m_context->FillRectangle(r, m_brush.Get());
}

void Overlay::Text(const wchar_t* text, IDWriteTextFormat* format, const D2D1_RECT_F& r, UINT32 rgb, float alpha)
{
    m_brush->SetColor(D2D1::ColorF(rgb, alpha * m_opacity));
    m_context->DrawText(text, static_cast<UINT32>(wcslen(text)), format, r, m_brush.Get());
}

bool Overlay::Draw(const OverlayState& s)
{
    m_lastHasAudio = s.hasAudio;
    m_lastShowQuality = s.showQuality;
    if (!m_context || (s.barOpacity <= 0.001f && (!s.osdText || s.osdOpacity <= 0.001f)))
        return false;
    if (!EnsureTarget())
        return false;

    const Layout l = ComputeLayout();
    m_context->BeginDraw();
    m_context->SetTransform(D2D1::Matrix3x2F::Identity());
    if (m_offscreen)
        m_context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    if (s.barOpacity > 0.001f)
    {
        m_opacity = s.barOpacity;

        // Darkening gradient behind the controls.
        m_gradient->SetStartPoint(D2D1::Point2F(0.0f, l.gradient.top));
        m_gradient->SetEndPoint(D2D1::Point2F(0.0f, l.gradient.bottom));
        m_gradient->SetOpacity(s.barOpacity);
        m_context->FillRectangle(l.gradient, m_gradient.Get());

        // Seek bar.
        const bool seekActive = s.hover == OverlayPart::Seek || s.draggingSeek;
        const float trackW = l.seekTrack.right - l.seekTrack.left;
        const float cy = (l.seekTrack.top + l.seekTrack.bottom) * 0.5f;
        const float th = seekActive ? 6.0f : 4.0f;
        const double played = s.duration > 0.0 ? std::clamp(s.position / s.duration, 0.0, 1.0) : 0.0;
        Fill({l.seekTrack.left, cy - th / 2, l.seekTrack.right, cy + th / 2}, kWhite, 0.25f, th / 2);
        Fill({l.seekTrack.left, cy - th / 2, l.seekTrack.left + static_cast<float>(played) * trackW, cy + th / 2},
             kAccent, 1.0f, th / 2);
        if (s.duration > 0.0 && s.seekPreview >= 0.0)
        {
            const float px = l.seekTrack.left + static_cast<float>(std::clamp(s.seekPreview / s.duration, 0.0, 1.0)) * trackW;
            wchar_t t[32];
            FormatTime(s.seekPreview, t, 32);
            const float tw = 72.0f;
            const float left = std::clamp(px - tw / 2, 4.0f, l.width - tw - 4.0f);
            const D2D1_RECT_F tip{left, cy - 36.0f, left + tw, cy - 14.0f};
            Fill(tip, kBlack, 0.8f, 4.0f);
            Text(t, m_textCenter.Get(), tip, kWhite, 1.0f);
            if (!s.draggingSeek)
                Fill({px - 1.0f, cy - th / 2 - 2.0f, px + 1.0f, cy + th / 2 + 2.0f}, kWhite, 0.8f);
        }
        if (seekActive && s.duration > 0.0)
        {
            const double knobPos = s.draggingSeek && s.seekPreview >= 0.0 ? s.seekPreview / s.duration : played;
            const float kx = l.seekTrack.left + static_cast<float>(std::clamp(knobPos, 0.0, 1.0)) * trackW;
            m_brush->SetColor(D2D1::ColorF(kWhite, m_opacity));
            m_context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 7.0f, 7.0f), m_brush.Get());
        }

        auto highlight = [&](OverlayPart p, const D2D1_RECT_F& r) {
            if (s.hover == p)
                Fill(r, kWhite, 0.15f, 6.0f);
        };

        // Play / pause.
        highlight(OverlayPart::PlayPause, l.play);
        Text(s.paused ? kIconPlay : kIconPause, m_icons.Get(), l.play, kWhite, 1.0f);

        // Mute + volume slider.
        if (s.hasAudio)
        {
            highlight(OverlayPart::Mute, l.mute);
            Text(s.muted ? kIconMute : kIconVolume, m_icons.Get(), l.mute, kWhite, 1.0f);
            const float vw = l.volumeTrack.right - l.volumeTrack.left;
            const float vol = s.muted ? 0.0f : std::clamp(s.volume, 0.0f, 1.0f);
            Fill(l.volumeTrack, kWhite, 0.25f, 2.0f);
            Fill({l.volumeTrack.left, l.volumeTrack.top, l.volumeTrack.left + vol * vw, l.volumeTrack.bottom}, kWhite,
                 0.9f, 2.0f);
            const float vy = (l.volumeTrack.top + l.volumeTrack.bottom) * 0.5f;
            m_brush->SetColor(D2D1::ColorF(kWhite, m_opacity));
            m_context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(l.volumeTrack.left + vol * vw, vy), 6.0f, 6.0f),
                                   m_brush.Get());
        }

        // Time.
        wchar_t pos[32], dur[32], timeText[80];
        FormatTime(s.position, pos, 32);
        FormatTime(s.duration, dur, 32);
        swprintf(timeText, 80, L"%ls / %ls", pos, dur);
        Text(timeText, m_textLeft.Get(), l.time, kWhite, 0.9f);

        // Upscaler toggle (green when on) and quality level.
        Fill(l.upscaler, s.upscalerOn ? kAccent : kWhite, s.upscalerOn ? 0.9f : 0.15f, 6.0f);
        if (s.hover == OverlayPart::Upscaler)
            Fill(l.upscaler, kWhite, 0.15f, 6.0f);
        Text(s.upscalerLabel, m_textCenter.Get(), l.upscaler, s.upscalerOn ? kBlack : kWhite, 1.0f);
        if (s.showQuality)
        {
            Fill(l.quality, kWhite, s.hover == OverlayPart::Quality ? 0.3f : 0.15f, 6.0f);
            wchar_t q[8];
            swprintf(q, 8, L"Q%d", s.quality);
            Text(q, m_textCenter.Get(), l.quality, kWhite, 1.0f);
        }

        // Fullscreen.
        highlight(OverlayPart::Fullscreen, l.fullscreen);
        Text(s.fullscreen ? kIconWindowed : kIconFullscreen, m_icons.Get(), l.fullscreen, kWhite, 1.0f);
    }

    if (s.osdText && s.osdOpacity > 0.001f)
    {
        m_opacity = s.osdOpacity;
        const float boxW = std::min(420.0f, l.width - 32.0f);
        const D2D1_RECT_F box{(l.width - boxW) * 0.5f, 40.0f, (l.width + boxW) * 0.5f, 92.0f};
        Fill(box, kBlack, 0.65f, 10.0f);
        Text(s.osdText, m_osd.Get(), box, kWhite, 1.0f);
    }

    m_opacity = 1.0f;
    const HRESULT hr = m_context->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
    {
        ReleaseTarget(); // device loss is reported by GpuDevice::CheckDeviceRemoved
        return false;
    }
    return SUCCEEDED(hr);
}

OverlayPart Overlay::HitTest(int x, int y) const
{
    if (!m_context)
        return OverlayPart::None;
    const float s = Scale();
    const float dx = static_cast<float>(x) / s, dy = static_cast<float>(y) / s;
    const Layout l = ComputeLayout();
    if (Contains(l.seekHit, dx, dy))
        return OverlayPart::Seek;
    if (Contains(l.play, dx, dy))
        return OverlayPart::PlayPause;
    if (m_lastHasAudio && Contains(l.mute, dx, dy))
        return OverlayPart::Mute;
    if (m_lastHasAudio && Contains(l.volumeHit, dx, dy))
        return OverlayPart::Volume;
    if (Contains(l.upscaler, dx, dy))
        return OverlayPart::Upscaler;
    if (m_lastShowQuality && Contains(l.quality, dx, dy))
        return OverlayPart::Quality;
    if (Contains(l.fullscreen, dx, dy))
        return OverlayPart::Fullscreen;
    if (dy >= l.bar.top)
        return OverlayPart::Bar;
    return OverlayPart::None;
}

double Overlay::SeekFraction(int x) const
{
    return Fraction(ComputeLayout().seekTrack, static_cast<float>(x) / Scale());
}

float Overlay::VolumeFraction(int x) const
{
    return Fraction(ComputeLayout().volumeTrack, static_cast<float>(x) / Scale());
}

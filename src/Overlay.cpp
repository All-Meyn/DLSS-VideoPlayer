#include "Overlay.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
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

constexpr wchar_t kIconAudio[] = L"\uE8D6";
constexpr wchar_t kIconSubtitles[] = L"\uE7F0";
constexpr wchar_t kIconCheck[] = L"\uE73E";
constexpr float kMenuRowHeight = 30.0f;
constexpr float kMenuWidth = 340.0f;

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
    l.trueHdr = {l.upscaler.left - 8.0f - 84.0f, cy - 14.0f, l.upscaler.left - 8.0f, cy + 14.0f};
    l.subsButton = button(l.trueHdr.left - 8.0f - b);
    l.audioButton = button(l.subsButton.left - 4.0f - b);

    // Tuning panel: bottom-right, just above the control bar.
    const float pw = 320.0f, rowH = 34.0f, titleH = 40.0f, footH = 44.0f;
    const float ph = titleH + 3.0f * rowH + footH;
    l.tunePanel = {w - 16.0f - pw, l.bar.top - 8.0f - ph, w - 16.0f, l.bar.top - 8.0f};
    for (int i = 0; i < 3; ++i)
    {
        const float top = l.tunePanel.top + titleH + static_cast<float>(i) * rowH;
        l.tuneRows[i] = {l.tunePanel.left + 8.0f, top, l.tunePanel.right - 8.0f, top + rowH};
        const float mid = top + rowH * 0.5f;
        l.tuneTracks[i] = {l.tunePanel.left + 120.0f, mid - 2.0f, l.tunePanel.right - 64.0f, mid + 2.0f};
    }
    l.tuneReset = {l.tunePanel.right - 8.0f - 80.0f, l.tunePanel.bottom - 8.0f - 28.0f, l.tunePanel.right - 8.0f,
                   l.tunePanel.bottom - 8.0f};
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

D2D1_RECT_F Overlay::MenuRect(const Layout& l, OverlayPart anchor, int count) const
{
    const D2D1_RECT_F& b = anchor == OverlayPart::AudioButton ? l.audioButton : l.subsButton;
    const float h = static_cast<float>(count) * kMenuRowHeight + 12.0f;
    const float right = std::min(b.right + 8.0f, l.width - 8.0f);
    const float left = std::max(8.0f, right - kMenuWidth);
    const float bottom = l.bar.top - 8.0f;
    return {left, std::max(8.0f, bottom - h), right, bottom};
}

void Overlay::DrawSubtitles(const OverlayState& s)
{
    const float scale = Scale();
    const D2D1_RECT_F video{static_cast<float>(s.videoRect.left) / scale, static_cast<float>(s.videoRect.top) / scale,
                            static_cast<float>(s.videoRect.right) / scale, static_cast<float>(s.videoRect.bottom) / scale};
    const float vw = video.right - video.left, vh = video.bottom - video.top;
    if (vw <= 1.0f || vh <= 1.0f)
        return;
    ++m_frameCounter;

    // Image subtitles (PGS / VobSub): placed on their canvas, scaled to the video rect.
    for (int i = 0; i < s.subCount; ++i)
    {
        const SubEvent& ev = *s.subs[i];
        const float cw = static_cast<float>(ev.canvasW > 0 ? ev.canvasW : s.videoW);
        const float ch = static_cast<float>(ev.canvasH > 0 ? ev.canvasH : s.videoH);
        if (cw <= 0.0f || ch <= 0.0f)
            continue;
        for (size_t k = 0; k < ev.images.size() && k < 256; ++k)
        {
            const SubImage& img = ev.images[k];
            const uint64_t key = (ev.id << 8) | k;
            CachedBitmap& cached = m_subBitmaps[key];
            if (!cached.bitmap)
            {
                const D2D1_BITMAP_PROPERTIES props =
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                m_context->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(img.w), static_cast<UINT32>(img.h)),
                                        img.bgra.data(), static_cast<UINT32>(img.w) * 4, &props, &cached.bitmap);
            }
            cached.lastUsed = m_frameCounter;
            if (!cached.bitmap)
                continue;
            const float sx = vw / cw, sy = vh / ch;
            const D2D1_RECT_F dst{video.left + static_cast<float>(img.x) * sx, video.top + static_cast<float>(img.y) * sy,
                                  video.left + static_cast<float>(img.x + img.w) * sx,
                                  video.top + static_cast<float>(img.y + img.h) * sy};
            m_context->DrawBitmap(cached.bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    // Forget bitmaps of events that are no longer shown.
    for (auto it = m_subBitmaps.begin(); it != m_subBitmaps.end();)
        it = it->second.lastUsed == m_frameCounter ? std::next(it) : m_subBitmaps.erase(it);

    // Text subtitles: joined, bottom-centered in the video, white with a black outline.
    m_subText.clear();
    for (int i = 0; i < s.subCount; ++i)
    {
        if (s.subs[i]->text.empty())
            continue;
        if (!m_subText.empty())
            m_subText += L'\n';
        m_subText += s.subs[i]->text;
    }
    if (m_subText.empty())
        return;

    const float fontSize = std::clamp(vh * 0.045f, 16.0f, 72.0f);
    if (!m_subFormat || std::abs(fontSize - m_subFontSize) > 0.5f)
    {
        m_subFormat.Reset();
        m_subLayout.Reset();
        if (FAILED(m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"",
                                              &m_subFormat)))
            return;
        m_subFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_subFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        m_subFontSize = fontSize;
    }
    const float maxWidth = vw * 0.92f;
    if (!m_subLayout || m_subText != m_subLayoutText || std::abs(maxWidth - m_subLayoutWidth) > 0.5f)
    {
        m_subLayout.Reset();
        if (FAILED(m_dwrite->CreateTextLayout(m_subText.c_str(), static_cast<UINT32>(m_subText.size()),
                                              m_subFormat.Get(), maxWidth, vh, &m_subLayout)))
            return;
        m_subLayoutText = m_subText;
        m_subLayoutWidth = maxWidth;
    }
    DWRITE_TEXT_METRICS metrics{};
    m_subLayout->GetMetrics(&metrics);

    // Lift the subtitles above the control bar while it is visible.
    const float barTop = ComputeLayout().bar.top;
    const float raise = std::max(0.0f, video.bottom - barTop) * s.barOpacity;
    const float x = video.left + (vw - maxWidth) * 0.5f;
    const float y = video.bottom - vh * 0.05f - raise - metrics.height;

    const float o = std::max(1.5f, fontSize * 0.06f);
    static const float kDirs[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
    m_brush->SetColor(D2D1::ColorF(kBlack, 1.0f));
    for (const auto& d : kDirs)
        m_context->DrawTextLayout(D2D1::Point2F(x + d[0] * o, y + d[1] * o), m_subLayout.Get(), m_brush.Get());
    m_brush->SetColor(D2D1::ColorF(kWhite, 1.0f));
    m_context->DrawTextLayout(D2D1::Point2F(x, y), m_subLayout.Get(), m_brush.Get());
}

bool Overlay::Draw(const OverlayState& s)
{
    m_lastHasAudio = s.hasAudio;
    m_lastShowQuality = s.showQuality;
    m_lastShowTrueHdr = s.showTrueHdr;
    m_lastTuneOpen = s.tuneOpen;
    m_lastShowAudio = s.showAudioButton;
    m_lastShowSubs = s.showSubsButton;
    m_lastMenu = s.menuItems ? s.openMenu : OverlayPart::None;
    m_lastMenuCount = s.menuItems ? static_cast<int>(s.menuItems->size()) : 0;
    if (!m_context ||
        (s.barOpacity <= 0.001f && (!s.osdText || s.osdOpacity <= 0.001f) && s.subCount == 0))
    {
        m_subBitmaps.clear();
        return false;
    }
    if (!EnsureTarget())
        return false;

    const Layout l = ComputeLayout();
    m_context->BeginDraw();
    m_context->SetTransform(D2D1::Matrix3x2F::Identity());
    if (m_offscreen)
        m_context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    m_opacity = 1.0f;
    DrawSubtitles(s); // under the controls

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

        // RTX Video HDR toggle (green when converting SDR -> HDR).
        if (s.showTrueHdr)
        {
            Fill(l.trueHdr, s.trueHdrOn ? kAccent : kWhite, s.trueHdrOn ? 0.9f : 0.15f, 6.0f);
            if (s.hover == OverlayPart::TrueHdr)
                Fill(l.trueHdr, kWhite, 0.15f, 6.0f);
            Text(L"RTX HDR", m_textCenter.Get(), l.trueHdr, s.trueHdrOn ? kBlack : kWhite, 1.0f);
        }

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

        // Audio / subtitle track buttons.
        if (s.showAudioButton)
        {
            if (s.hover == OverlayPart::AudioButton || s.openMenu == OverlayPart::AudioButton)
                Fill(l.audioButton, kWhite, 0.15f, 6.0f);
            Text(kIconAudio, m_icons.Get(), l.audioButton, kWhite, 1.0f);
        }
        if (s.showSubsButton)
        {
            if (s.hover == OverlayPart::SubsButton || s.openMenu == OverlayPart::SubsButton)
                Fill(l.subsButton, kWhite, 0.15f, 6.0f);
            Text(kIconSubtitles, m_icons.Get(), l.subsButton, s.subsOn ? kAccent : kWhite, 1.0f);
        }

        // Track menu.
        if (s.menuItems && !s.menuItems->empty() &&
            (s.openMenu == OverlayPart::AudioButton || s.openMenu == OverlayPart::SubsButton))
        {
            const int count = static_cast<int>(s.menuItems->size());
            const D2D1_RECT_F menu = MenuRect(l, s.openMenu, count);
            Fill(menu, kBlack, 0.85f, 8.0f);
            for (int i = 0; i < count; ++i)
            {
                const float top = menu.top + 6.0f + static_cast<float>(i) * kMenuRowHeight;
                const D2D1_RECT_F row{menu.left + 6.0f, top, menu.right - 6.0f, top + kMenuRowHeight};
                if (i == s.menuHover)
                    Fill(row, kWhite, 0.15f, 5.0f);
                if (i == s.menuSelected)
                    Text(kIconCheck, m_icons.Get(), {row.left, row.top, row.left + 28.0f, row.bottom}, kAccent, 1.0f);
                Text((*s.menuItems)[static_cast<size_t>(i)].c_str(), m_textLeft.Get(),
                     {row.left + 32.0f, row.top, row.right - 6.0f, row.bottom}, kWhite, i == s.menuSelected ? 1.0f : 0.85f);
            }
        }

        if (s.tuneOpen)
        {
            static const wchar_t* const kLabels[3] = {L"Contrast", L"Saturation", L"Middle gray"};
            static const unsigned kMin[3] = {0, 0, 10};
            static const unsigned kMax[3] = {200, 200, 100};

            Fill(l.tunePanel, kBlack, 0.8f, 10.0f);
            const D2D1_RECT_F title{l.tunePanel.left + 16.0f, l.tunePanel.top, l.tunePanel.right - 16.0f,
                                    l.tunePanel.top + 40.0f};
            Text(L"RTX Video HDR", m_textCenter.Get(), {title.left, title.top, title.left + 110.0f, title.bottom},
                 kWhite, 1.0f);
            if (!s.trueHdrOn)
                Text(L"inactive", m_textLeft.Get(), {title.right - 56.0f, title.top, title.right, title.bottom}, kWhite,
                     0.5f);

            for (int i = 0; i < 3; ++i)
            {
                const D2D1_RECT_F& row = l.tuneRows[i];
                const D2D1_RECT_F& track = l.tuneTracks[i];
                if (s.tuneRow == i)
                    Fill(row, kWhite, 0.10f, 6.0f);
                else if (s.hover == static_cast<OverlayPart>(static_cast<int>(OverlayPart::TuneContrast) + i))
                    Fill(row, kWhite, 0.06f, 6.0f);
                Text(kLabels[i], m_textLeft.Get(), {row.left + 8.0f, row.top, track.left - 8.0f, row.bottom}, kWhite,
                     0.9f);

                const unsigned v = std::clamp(s.tuneValues[i], kMin[i], kMax[i]);
                const float f = static_cast<float>(v - kMin[i]) / static_cast<float>(kMax[i] - kMin[i]);
                const float tw = track.right - track.left;
                Fill(track, kWhite, 0.25f, 2.0f);
                Fill({track.left, track.top, track.left + f * tw, track.bottom}, kAccent, 1.0f, 2.0f);
                m_brush->SetColor(D2D1::ColorF(kWhite, m_opacity));
                m_context->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(track.left + f * tw, (track.top + track.bottom) * 0.5f), 6.0f, 6.0f),
                    m_brush.Get());

                wchar_t value[8];
                swprintf(value, 8, L"%u", v);
                Text(value, m_textCenter.Get(), {track.right + 8.0f, row.top, row.right, row.bottom}, kWhite, 1.0f);
            }

            Fill(l.tuneReset, kWhite, s.hover == OverlayPart::TuneReset ? 0.3f : 0.15f, 6.0f);
            Text(L"Reset", m_textCenter.Get(), l.tuneReset, kWhite, 1.0f);
        }
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

OverlayPart Overlay::HitTest(int x, int y, int* itemIndex) const
{
    if (!m_context)
        return OverlayPart::None;
    const float s = Scale();
    const float dx = static_cast<float>(x) / s, dy = static_cast<float>(y) / s;
    const Layout l = ComputeLayout();
    if (m_lastMenu != OverlayPart::None && m_lastMenuCount > 0)
    {
        const D2D1_RECT_F menu = MenuRect(l, m_lastMenu, m_lastMenuCount);
        if (Contains(menu, dx, dy))
        {
            const int i = static_cast<int>((dy - menu.top - 6.0f) / kMenuRowHeight);
            if (i >= 0 && i < m_lastMenuCount)
            {
                if (itemIndex)
                    *itemIndex = i;
                return OverlayPart::MenuItem;
            }
            return OverlayPart::Menu;
        }
    }
    if (m_lastTuneOpen && Contains(l.tunePanel, dx, dy))
    {
        for (int i = 0; i < 3; ++i)
            if (Contains(l.tuneRows[i], dx, dy))
                return static_cast<OverlayPart>(static_cast<int>(OverlayPart::TuneContrast) + i);
        if (Contains(l.tuneReset, dx, dy))
            return OverlayPart::TuneReset;
        return OverlayPart::TunePanel;
    }
    if (Contains(l.seekHit, dx, dy))
        return OverlayPart::Seek;
    if (Contains(l.play, dx, dy))
        return OverlayPart::PlayPause;
    if (m_lastHasAudio && Contains(l.mute, dx, dy))
        return OverlayPart::Mute;
    if (m_lastHasAudio && Contains(l.volumeHit, dx, dy))
        return OverlayPart::Volume;
    if (m_lastShowTrueHdr && Contains(l.trueHdr, dx, dy))
        return OverlayPart::TrueHdr;
    if (m_lastShowAudio && Contains(l.audioButton, dx, dy))
        return OverlayPart::AudioButton;
    if (m_lastShowSubs && Contains(l.subsButton, dx, dy))
        return OverlayPart::SubsButton;
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

float Overlay::TuneFraction(int row, int x) const
{
    return Fraction(ComputeLayout().tuneTracks[std::clamp(row, 0, 2)], static_cast<float>(x) / Scale());
}

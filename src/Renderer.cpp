#include "Renderer.h"

#include "Log.h"

#include <algorithm>
#include <cmath>

const char* UpscaleModeName(UpscaleMode mode)
{
    switch (mode)
    {
    case UpscaleMode::Off: return "Off (bilinear)";
    case UpscaleMode::DriverVsr: return "RTX VSR (driver)";
    case UpscaleMode::RtxVideoSdk: return "RTX Video SDK";
    }
    return "?";
}

bool Renderer::Init(GpuDevice* gpu)
{
    m_gpu = gpu;
    m_vp.Init(gpu->Device(), gpu->VideoDevice(), gpu->VideoContext());
    m_sdk = CreateRtxVideoSdkUpscaler(gpu->Device(), gpu->Context());
    LogF("RTX Video SDK backend: %s", m_sdk ? "available" : "not available");
    return EnsurePresentTexture();
}

void Renderer::OnResize()
{
    m_present.Reset();
    m_presentRtv.Reset();
    m_sdkOutput.Reset();
    EnsurePresentTexture();
}

bool Renderer::EnsureTexture(ComPtr<ID3D11Texture2D>& tex, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind)
{
    if (tex)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (d.Width == w && d.Height == h && d.Format == fmt && d.BindFlags == bind)
            return true;
        tex.Reset();
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    HRESULT hr = m_gpu->Device()->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr))
    {
        LogF("CreateTexture2D %ux%u fmt %d failed: 0x%08lx", w, h, fmt, hr);
        return false;
    }
    return true;
}

bool Renderer::EnsurePresentTexture()
{
    if (m_present)
        return true;
    if (!EnsureTexture(m_present, m_gpu->Width(), m_gpu->Height(), GpuDevice::kBackBufferFormat,
                       D3D11_BIND_RENDER_TARGET))
        return false;
    m_gpu->Device()->CreateRenderTargetView(m_present.Get(), nullptr, &m_presentRtv);
    RenderBlack();
    return true;
}

RECT Renderer::ComputeDestRect(int srcW, int srcH, AVRational sar) const
{
    const double winW = m_gpu->Width();
    const double winH = m_gpu->Height();
    const double dar = (static_cast<double>(srcW) * sar.num) / (static_cast<double>(srcH) * sar.den);

    double w = winW;
    double h = winW / dar;
    if (h > winH)
    {
        h = winH;
        w = winH * dar;
    }
    const LONG iw = std::max<LONG>(1, static_cast<LONG>(std::lround(w)));
    const LONG ih = std::max<LONG>(1, static_cast<LONG>(std::lround(h)));
    const LONG x = (static_cast<LONG>(winW) - iw) / 2;
    const LONG y = (static_cast<LONG>(winH) - ih) / 2;
    return RECT{x, y, x + iw, y + ih};
}

bool Renderer::RenderFrame(const VideoFrame& vf, AVRational sar, UpscaleMode mode, int quality,
                           UpscaleMode& effectiveMode)
{
    if (!EnsurePresentTexture())
        return false;

    const AVFrame* f = vf.frame.get();
    const RECT srcRect{0, 0, f->width, f->height};
    m_destRect = ComputeDestRect(f->width, f->height, sar);
    const LONG destW = m_destRect.right - m_destRect.left;
    const LONG destH = m_destRect.bottom - m_destRect.top;
    const bool upscaling = destW > f->width || destH > f->height;

    // Super resolution only makes sense when enlarging; downscaling uses the plain path.
    if (!upscaling)
        mode = UpscaleMode::Off;

    if (mode == UpscaleMode::RtxVideoSdk)
    {
        if (m_sdk && m_sdk->OutputFormat() == GpuDevice::kBackBufferFormat &&
            EnsureTexture(m_sdkInput, f->width, f->height, m_sdk->InputFormat(), m_sdk->InputBindFlags()) &&
            EnsureTexture(m_sdkOutput, destW, destH, m_sdk->OutputFormat(), m_sdk->OutputBindFlags()))
        {
            // 1) YUV -> RGB at source resolution, 2) SDK upscale, 3) place into letterboxed frame.
            const RECT outRect{0, 0, destW, destH};
            if (m_vp.Blit(vf.Texture(), vf.ArraySlice(), srcRect, f, m_sdkInput.Get(), srcRect, false) &&
                m_sdk->Upscale(m_sdkInput.Get(), srcRect, m_sdkOutput.Get(), outRect, quality))
            {
                RenderBlack();
                m_gpu->Context()->CopySubresourceRegion(m_present.Get(), 0, m_destRect.left, m_destRect.top, 0,
                                                        m_sdkOutput.Get(), 0, nullptr);
                effectiveMode = UpscaleMode::RtxVideoSdk;
                return true;
            }
        }
        mode = UpscaleMode::DriverVsr; // SDK unavailable or failed: fall back.
    }

    const bool vsr = mode == UpscaleMode::DriverVsr;
    if (!m_vp.Blit(vf.Texture(), vf.ArraySlice(), srcRect, f, m_present.Get(), m_destRect, vsr))
        return false;
    effectiveMode = (vsr && m_vp.NvidiaVsrAccepted()) ? UpscaleMode::DriverVsr : UpscaleMode::Off;
    return true;
}

void Renderer::RenderBlack()
{
    if (!m_presentRtv)
        return;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_gpu->Context()->ClearRenderTargetView(m_presentRtv.Get(), black);
}

void Renderer::Present()
{
    if (m_present && m_gpu->BackBuffer())
        m_gpu->Context()->CopyResource(m_gpu->BackBuffer(), m_present.Get());
    m_gpu->Present();
}

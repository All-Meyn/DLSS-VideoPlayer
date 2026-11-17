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

bool Renderer::Init(GpuDevice* gpu, bool enableRtxSdk)
{
    m_gpu = gpu;
    m_vp.Init(gpu->Device(), gpu->VideoDevice(), gpu->VideoContext());
    if (enableRtxSdk)
        m_sdk = CreateRtxVideoSdkUpscaler(gpu->Device(), gpu->Context());
    LogF("RTX Video SDK backend: %s", m_sdk ? "available" : (enableRtxSdk ? "not available" : "disabled (--no-sdk)"));
    return EnsurePresentTexture();
}

void Renderer::SetOutputMode(OutputMode mode, bool trueHdr, DXGI_FORMAT sdrFormat, bool pqVideo)
{
    m_mode = mode;
    m_pqVideo = pqVideo && mode != OutputMode::Sdr8; // PQ needs the 10-bit presentation texture
    m_trueHdr = trueHdr && mode == OutputMode::Hdr10;
    m_trueHdrActive = false;
    m_sdrFormat = sdrFormat;
    m_sdkInput.Reset();
    m_sdkOutput.Reset();
    m_sdrScaled.Reset();
    m_thdrOutput.Reset();
    OnResize(); // recreates the presentation texture in the new format
}

void Renderer::OnResize()
{
    m_present.Reset();
    m_presentRtv.Reset();
    m_presentSrv.Reset();
    m_sdrScaled.Reset();
    m_thdrOutput.Reset();
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
    if (!EnsureTexture(m_present, m_gpu->Width(), m_gpu->Height(), PresentFormat(),
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        return false;
    m_gpu->Device()->CreateRenderTargetView(m_present.Get(), nullptr, &m_presentRtv);
    m_gpu->Device()->CreateShaderResourceView(m_present.Get(), nullptr, &m_presentSrv);
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

    // RTX Video HDR: SDR source on an HDR10 output. VSR (if any) runs on the SDR image first,
    // TrueHDR last, as in NVIDIA's samples.
    const bool trueHdr = m_trueHdr && IsHdr() && m_sdk && m_sdk->SupportsSdrToHdr();
    const DXGI_FORMAT stageFormat = trueHdr ? m_sdrFormat : PresentFormat();
    const bool vpHdr = m_pqVideo && !trueHdr; // the video processor writes PQ only for HDR10 sources
    const RECT outRect{0, 0, destW, destH};
    ID3D11Texture2D* scaled = nullptr; // dest-sized result; null when the VP wrote straight into m_present
    effectiveMode = UpscaleMode::Off;

    if (mode == UpscaleMode::RtxVideoSdk)
    {
        // 1) YUV -> RGB at source resolution, 2) SDK upscale (3) placed into the letterboxed frame below).
        if (m_sdk && m_sdk->SupportsFormat(stageFormat) &&
            EnsureTexture(m_sdkInput, f->width, f->height, stageFormat, m_sdk->InputBindFlags()) &&
            EnsureTexture(m_sdkOutput, destW, destH, stageFormat, m_sdk->OutputBindFlags()) &&
            m_vp.Blit(vf.Texture(), vf.ArraySlice(), srcRect, f, m_sdkInput.Get(), srcRect, false, vpHdr) &&
            m_sdk->Upscale(m_sdkInput.Get(), srcRect, m_sdkOutput.Get(), outRect, quality))
        {
            scaled = m_sdkOutput.Get();
            effectiveMode = UpscaleMode::RtxVideoSdk;
        }
        else
        {
            // SDK unavailable or failed: fall back to plain scaling, NOT driver VSR. On the
            // RTX 4090 test machine driver VSR fails every frame ("NVWGF2UM: Failed to run
            // Algo 2") while the SDK is alive, ending in DXGI_ERROR_DRIVER_INTERNAL_ERROR.
            mode = UpscaleMode::Off;
        }
    }

    if (!scaled)
    {
        const bool vsr = mode == UpscaleMode::DriverVsr;
        if (trueHdr)
        {
            if (!EnsureTexture(m_sdrScaled, destW, destH, stageFormat, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) ||
                !m_vp.Blit(vf.Texture(), vf.ArraySlice(), srcRect, f, m_sdrScaled.Get(), outRect, vsr, false))
                return false;
            scaled = m_sdrScaled.Get();
        }
        else if (!m_vp.Blit(vf.Texture(), vf.ArraySlice(), srcRect, f, m_present.Get(), m_destRect, vsr, vpHdr))
        {
            return false;
        }
        effectiveMode = (vsr && m_vp.NvidiaVsrAccepted()) ? UpscaleMode::DriverVsr : UpscaleMode::Off;
    }

    m_trueHdrActive = false;
    if (trueHdr)
    {
        if (EnsureTexture(m_thdrOutput, destW, destH, PresentFormat(),
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS) &&
            m_sdk->SdrToHdr(scaled, outRect, m_thdrOutput.Get(), outRect, m_thdrSettings))
        {
            scaled = m_thdrOutput.Get();
            m_trueHdrActive = true;
        }
        else
        {
            // TrueHDR failed: let the video processor map the SDR frame into the HDR10 output
            // instead (no AI expansion, but correct colors).
            effectiveMode = UpscaleMode::Off;
            return m_vp.Blit(vf.Texture(), vf.ArraySlice(), srcRect, f, m_present.Get(), m_destRect, false, true);
        }
    }

    if (scaled)
    {
        // Place the dest-sized result into the letterboxed presentation texture.
        RenderBlack();
        m_gpu->Context()->CopySubresourceRegion(m_present.Get(), 0, m_destRect.left, m_destRect.top, 0, scaled, 0,
                                                nullptr);
    }
    return true;
}

void Renderer::RenderBlack()
{
    if (!m_presentRtv)
        return;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_gpu->Context()->ClearRenderTargetView(m_presentRtv.Get(), black);
}

void Renderer::CopyToBackBuffer()
{
    if (m_present && m_gpu->BackBuffer())
        m_gpu->Context()->CopyResource(m_gpu->BackBuffer(), m_present.Get());
}

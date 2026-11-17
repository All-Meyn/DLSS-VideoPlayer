#pragma once

#include "GpuDevice.h"
#include "VideoDecoder.h"
#include "VideoProcessor.h"
#include "upscalers/IUpscaler.h"

#include <memory>

enum class UpscaleMode
{
    Off,         // Plain video-processor scaling (driver default, usually bilinear/bicubic).
    DriverVsr,   // NVIDIA RTX Video Super Resolution via the driver's D3D11 stream extension.
    RtxVideoSdk, // NVIDIA RTX Video SDK (only when built with the SDK).
};

const char* UpscaleModeName(UpscaleMode mode);

// Turns decoded frames into a window-sized "presentation" texture, which is then
// copied to the swap chain every vsync (App draws the overlay on top and presents). Re-processing only happens when the frame,
// window size or settings change.
class Renderer
{
public:
    // enableRtxSdk=false skips creating the RTX Video SDK backend (--no-sdk).
    bool Init(GpuDevice* gpu, bool enableRtxSdk);
    void OnResize();

    bool HasRtxVideoSdk() const { return m_sdk != nullptr; }

    // 10-bit modes: the presentation texture becomes RGB10A2 (read by OutputPass); in HDR10
    // it holds PQ / BT.2020, otherwise SDR BT.709.
    // trueHdr (HDR10 only): the source is SDR and is converted with RTX Video HDR after
    // upscaling; sdrFormat is the format of that SDR stage (RGBA8, or RGB10A2 for 10-bit sources).
    // pqVideo: an HDR10 source kept as PQ / BT.2020 in the presentation texture (HDR10 output,
    // or SDR 10-bit output where OutputPass tone-maps it); otherwise the video processor writes SDR.
    void SetOutputMode(OutputMode mode, bool trueHdr = false, DXGI_FORMAT sdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
                       bool pqVideo = false);
    bool SupportsTrueHdr() const { return m_sdk && m_sdk->SupportsSdrToHdr(); }
    void SetSdrToHdrSettings(const IUpscaler::SdrToHdrSettings& settings) { m_thdrSettings = settings; }
    bool TrueHdrActive() const { return m_trueHdrActive; } // last frame went through RTX Video HDR
    ID3D11ShaderResourceView* PresentSrv() const { return m_presentSrv.Get(); }

    // Processes `frame` with the selected mode into the presentation texture.
    // `effectiveMode` receives the mode actually used (e.g. falls back to Off when downscaling).
    bool RenderFrame(const VideoFrame& frame, AVRational sar, UpscaleMode mode, int quality,
                     UpscaleMode& effectiveMode);
    void RenderBlack();
    // Copies the processed frame into the swap chain back buffer (overlay is drawn after,
    // then GpuDevice::Present).
    void CopyToBackBuffer();

    RECT LastDestRect() const { return m_destRect; }

private:
    bool EnsurePresentTexture();
    bool EnsureTexture(ComPtr<ID3D11Texture2D>& tex, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind);
    RECT ComputeDestRect(int srcW, int srcH, AVRational sar) const;

    GpuDevice* m_gpu = nullptr;
    VideoProcessor m_vp;
    std::unique_ptr<IUpscaler> m_sdk;

    ComPtr<ID3D11Texture2D> m_present;      // window-sized, back-buffer format
    ComPtr<ID3D11RenderTargetView> m_presentRtv;
    ComPtr<ID3D11ShaderResourceView> m_presentSrv;
    OutputMode m_mode = OutputMode::Sdr8;
    bool IsHdr() const { return m_mode == OutputMode::Hdr10; }
    DXGI_FORMAT PresentFormat() const
    {
        return m_mode == OutputMode::Sdr8 ? GpuDevice::kBackBufferFormat : GpuDevice::k10BitBackBufferFormat;
    }
    ComPtr<ID3D11Texture2D> m_sdkInput;     // source-sized RGB (SDK path)
    ComPtr<ID3D11Texture2D> m_sdkOutput;    // dest-rect-sized RGB (SDK path)
    ComPtr<ID3D11Texture2D> m_sdrScaled;    // dest-rect-sized SDR RGB when TrueHDR runs without VSR
    ComPtr<ID3D11Texture2D> m_thdrOutput;   // dest-rect-sized HDR10 output of TrueHDR (UAV)
    bool m_trueHdr = false;
    bool m_trueHdrActive = false;
    bool m_pqVideo = false;
    DXGI_FORMAT m_sdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    IUpscaler::SdrToHdrSettings m_thdrSettings;
    RECT m_destRect{};
};

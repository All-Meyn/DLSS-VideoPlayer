#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

struct AVFrame;

using Microsoft::WRL::ComPtr;

// Wraps ID3D11VideoProcessor: YUV -> RGB conversion, scaling, letterboxing, and
// the NVIDIA driver "RTX Video Super Resolution" stream extension.
class VideoProcessor
{
public:
    bool Init(ID3D11Device* device, ID3D11VideoDevice* videoDevice, ID3D11VideoContext* videoContext);

    // Blits `srcRect` of array slice `srcSlice` of `src` into `dstRect` of `dst`.
    // The area of `dst` outside `dstRect` is filled with black.
    // `colorInfo` supplies matrix / range / transfer metadata (may be null).
    // `nvidiaVsr` asks the NVIDIA driver to apply RTX Video Super Resolution while scaling.
    // `hdrOutput`: write PQ / BT.2020 RGB (HDR10, no tone mapping) instead of SDR BT.709.
    bool Blit(ID3D11Texture2D* src, UINT srcSlice, const RECT& srcRect, const AVFrame* colorInfo,
              ID3D11Texture2D* dst, const RECT& dstRect, bool nvidiaVsr, bool hdrOutput);

    // True if the last driver-VSR request was accepted by the driver (it can still be
    // disabled in the NVIDIA Control Panel, which the API cannot report).
    bool NvidiaVsrAccepted() const { return m_nvVsrAccepted; }

private:
    bool EnsureProcessor(const D3D11_TEXTURE2D_DESC& in, const D3D11_TEXTURE2D_DESC& out);
    void ApplyColorSpace(const AVFrame* colorInfo, bool hdrOutput);
    void ApplyNvidiaVsr(bool enable);

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoContext> m_videoContext;
    ComPtr<ID3D11VideoContext1> m_videoContext1;
    ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
    ComPtr<ID3D11VideoProcessor> m_processor;

    UINT m_inW = 0, m_inH = 0, m_outW = 0, m_outH = 0;
    DXGI_FORMAT m_inFmt = DXGI_FORMAT_UNKNOWN, m_outFmt = DXGI_FORMAT_UNKNOWN;
    bool m_nvVsrStateKnown = false;
    bool m_nvVsrEnabled = false;
    bool m_nvVsrAccepted = false;
};

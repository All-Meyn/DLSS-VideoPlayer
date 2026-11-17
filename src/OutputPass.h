#pragma once

#include "GpuDevice.h"

#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Final full-screen pass for the 10-bit output modes: writes the video frame to the 10-bit
// back buffer and blends the overlay on top. Modes:
//   Sdr     - SDR video to an SDR back buffer; plain alpha blend of the overlay.
//   Hdr10   - PQ / BT.2020 video to an HDR10 back buffer; the overlay is placed at the user's
//             SDR white level so the controls look like normal UI instead of 10,000-nit white.
//   ToneMap - PQ / BT.2020 video tone-mapped to SDR BT.709 (BT.2390 EETF on max(R,G,B), so
//             hues are preserved), for HDR10 files when Windows HDR is off.
// One draw call per presented frame.
// The shader is compiled at startup with d3dcompiler_47 (part of Windows 10/11).
class OutputPass
{
public:
    enum class Mode : UINT
    {
        Sdr = 0,
        Hdr10 = 1,
        ToneMap = 2,
    };
    struct Params
    {
        Mode mode = Mode::Sdr;
        float sdrWhiteNits = 200.0f; // Hdr10: overlay brightness
        float srcPeakNits = 1000.0f; // ToneMap: content peak (MaxCLL / mastering peak)
        float dstPeakNits = 203.0f;  // ToneMap: nits that map to SDR 100% (BT.2408 reference white)
    };

    bool Init(GpuDevice* gpu);
    bool IsAvailable() const { return m_ps != nullptr; }

    // Must be called before the swap chain is resized or its format changes.
    void ReleaseTarget();

    // video: window-sized texture (PQ/BT.2020 for Hdr10 / ToneMap, else SDR); overlay:
    // window-sized premultiplied sRGB texture, or nullptr when no overlay is visible.
    void Run(ID3D11ShaderResourceView* video, ID3D11ShaderResourceView* overlay, const Params& params);

private:
    GpuDevice* m_gpu = nullptr;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11RenderTargetView> m_rtv;
};

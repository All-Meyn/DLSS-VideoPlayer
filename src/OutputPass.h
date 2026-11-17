#pragma once

#include "GpuDevice.h"

#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Final full-screen pass for the 10-bit output modes: writes the video frame to the 10-bit
// back buffer and blends the overlay on top. In HDR10 (PQ / BT.2020) the overlay is placed at
// the user's SDR white level so the controls look like normal UI instead of 10,000-nit white;
// in 10-bit SDR it's a plain alpha blend. One draw call per presented frame.
// The shader is compiled at startup with d3dcompiler_47 (part of Windows 10/11).
class OutputPass
{
public:
    bool Init(GpuDevice* gpu);
    bool IsAvailable() const { return m_ps != nullptr; }

    // Must be called before the swap chain is resized or its format changes.
    void ReleaseTarget();

    // video: window-sized texture (PQ/BT.2020 if hdr, else SDR); overlay: window-sized
    // premultiplied sRGB texture, or nullptr when no overlay is visible.
    void Run(ID3D11ShaderResourceView* video, ID3D11ShaderResourceView* overlay, float sdrWhiteNits, bool hdr);

private:
    GpuDevice* m_gpu = nullptr;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11RenderTargetView> m_rtv;
};

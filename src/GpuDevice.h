#pragma once

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <string>

using Microsoft::WRL::ComPtr;

// Swap chain output: 8-bit SDR (default), 10-bit SDR (10-bit sources, less banding) or
// HDR10 (PQ / BT.2020 for HDR10 sources on a display with Windows HDR on).
enum class OutputMode
{
    Sdr8,
    Sdr10,
    Hdr10,
};
const char* OutputModeName(OutputMode mode);

// Owns the D3D11 device, the video device/context interfaces and the swap chain.
// Prefers an NVIDIA adapter so RTX Video Super Resolution is available.
class GpuDevice
{
public:
    // debugLayer: enable the D3D11 debug layer (needs the Windows "Graphics Tools" optional feature).
    bool Init(HWND hwnd, bool debugLayer);
    bool Resize(UINT width, UINT height);

    // Switches the swap chain format / color space. Callers must release every view of the
    // back buffer first. Returns the mode actually set (falls back HDR10 -> SDR 10-bit ->
    // SDR 8-bit if the display path refuses).
    OutputMode SetOutputMode(OutputMode mode, const DXGI_HDR_METADATA_HDR10* metadata);
    OutputMode GetOutputMode() const { return m_mode; }
    void Present();

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    ID3D11VideoDevice* VideoDevice() const { return m_videoDevice.Get(); }
    ID3D11VideoContext* VideoContext() const { return m_videoContext.Get(); }

    // In D3D11 flip-model swap chains, buffer 0 always refers to the current back buffer,
    // so this cached pointer stays valid until the next Resize().
    ID3D11Texture2D* BackBuffer() const { return m_backBuffer.Get(); }

    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    bool IsNvidia() const { return m_isNvidia; }

    // Returns true once the driver has removed/reset the device (TDR, driver fault).
    // Logs the reason the first time. Cheap: no GPU synchronization.
    bool CheckDeviceRemoved();
    const std::wstring& AdapterName() const { return m_adapterName; }

    // 8-bit SDR back buffer: RGBA (not BGRA) so the RTX Video SDK's RGBA8 output can be
    // copied straight in. 10-bit (SDR or HDR10) back buffer: written by OutputPass.
    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT k10BitBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoContext> m_videoContext;
    ComPtr<IDXGISwapChain1> m_swapChain;
    OutputMode m_mode = OutputMode::Sdr8;
    ComPtr<ID3D11Texture2D> m_backBuffer;

    UINT m_width = 0;
    UINT m_height = 0;
    bool m_isNvidia = false;
    bool m_deviceRemoved = false;
    std::wstring m_adapterName;
};

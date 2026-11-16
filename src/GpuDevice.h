#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>

using Microsoft::WRL::ComPtr;

// Owns the D3D11 device, the video device/context interfaces and the swap chain.
// Prefers an NVIDIA adapter so RTX Video Super Resolution is available.
class GpuDevice
{
public:
    bool Init(HWND hwnd);
    bool Resize(UINT width, UINT height);
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
    const std::wstring& AdapterName() const { return m_adapterName; }

    // RGBA (not BGRA) so the RTX Video SDK's RGBA8 output can be copied straight in.
    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoContext> m_videoContext;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11Texture2D> m_backBuffer;

    UINT m_width = 0;
    UINT m_height = 0;
    bool m_isNvidia = false;
    std::wstring m_adapterName;
};

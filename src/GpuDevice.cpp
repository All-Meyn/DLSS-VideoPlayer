#include "GpuDevice.h"

#include "Log.h"

#include <algorithm>

namespace
{
constexpr UINT kNvidiaVendorId = 0x10DE;

ComPtr<IDXGIAdapter1> PickAdapter(IDXGIFactory1* factory, bool& isNvidia)
{
    ComPtr<IDXGIAdapter1> fallback;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        if (desc.VendorId == kNvidiaVendorId)
        {
            isNvidia = true;
            return adapter;
        }
        if (!fallback)
            fallback = adapter;
    }
    isNvidia = false;
    return fallback;
}
} // namespace

bool GpuDevice::Init(HWND hwnd)
{
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        LogF("CreateDXGIFactory1 failed");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter = PickAdapter(factory.Get(), m_isNvidia);
    if (adapter)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        m_adapterName = desc.Description;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device,
                                   nullptr, &m_context);
#ifdef _DEBUG
    if (FAILED(hr))
    {
        // Debug layer not installed (Graphics Tools optional feature) - retry without it.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                               nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr,
                               &m_context);
    }
#endif
    if (FAILED(hr))
    {
        LogF("D3D11CreateDevice failed: 0x%08lx", hr);
        return false;
    }

    // The decoder and renderer share this device. Everything currently runs on the UI
    // thread, but enabling multithread protection makes a future decode thread safe.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    if (FAILED(m_device.As(&m_videoDevice)) || FAILED(m_context.As(&m_videoContext)))
    {
        LogF("GPU/driver does not expose D3D11 video interfaces");
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    m_width = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
    m_height = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = m_width;
    scd.Height = m_height;
    scd.Format = kBackBufferFormat;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling = DXGI_SCALING_NONE;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    // Swap chains must be created from the factory that owns the device's adapter.
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> deviceAdapter;
    ComPtr<IDXGIFactory2> deviceFactory;
    m_device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&deviceAdapter);
    deviceAdapter->GetParent(IID_PPV_ARGS(&deviceFactory));

    hr = deviceFactory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &scd, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr))
    {
        LogF("CreateSwapChainForHwnd failed: 0x%08lx", hr);
        return false;
    }
    // We implement our own borderless fullscreen; stop DXGI from hijacking Alt+Enter.
    deviceFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&m_backBuffer))))
        return false;

    LogF("GPU: %ls (NVIDIA: %s)", m_adapterName.c_str(), m_isNvidia ? "yes" : "no");
    return true;
}

bool GpuDevice::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0 || (width == m_width && height == m_height))
        return false;

    m_backBuffer.Reset();
    m_context->ClearState();
    m_context->Flush();

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        LogF("ResizeBuffers failed: 0x%08lx", hr);
        return false;
    }
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&m_backBuffer));
    m_width = width;
    m_height = height;
    return true;
}

void GpuDevice::Present()
{
    // VSync on: paces the render loop to the display refresh rate.
    m_swapChain->Present(1, 0);
}

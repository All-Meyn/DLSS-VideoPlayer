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

bool GpuDevice::Init(HWND hwnd, bool debugLayer)
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
    debugLayer = true;
#endif
    if (debugLayer)
        flags |= D3D11_CREATE_DEVICE_DEBUG;

    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device,
                                   nullptr, &m_context);
    if (FAILED(hr) && debugLayer)
    {
        // Debug layer not installed (Graphics Tools optional feature) - retry without it.
        LogF("D3D11 debug layer unavailable (install the 'Graphics Tools' optional feature); continuing without it");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                               nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr,
                               &m_context);
    }
    else if (debugLayer)
    {
        LogF("D3D11 debug layer enabled (messages go to the debugger output / DebugView)");
    }
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

const char* OutputModeName(OutputMode mode)
{
    switch (mode)
    {
    case OutputMode::Sdr8: return "SDR 8-bit";
    case OutputMode::Sdr10: return "SDR 10-bit";
    case OutputMode::Hdr10: return "HDR10";
    }
    return "?";
}

OutputMode GpuDevice::SetOutputMode(OutputMode mode, const DXGI_HDR_METADATA_HDR10* metadata)
{
    ComPtr<IDXGISwapChain4> sc4;
    m_swapChain.As(&sc4); // null before Windows 10 1709: no color-space control
    if (!sc4 && mode == OutputMode::Hdr10)
        mode = OutputMode::Sdr10;

    const DXGI_FORMAT format = mode == OutputMode::Sdr8 ? kBackBufferFormat : k10BitBackBufferFormat;
    const DXGI_COLOR_SPACE_TYPE colorSpace =
        mode == OutputMode::Hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    m_backBuffer.Reset();
    m_context->ClearState();
    m_context->Flush();
    const HRESULT hr = m_swapChain->ResizeBuffers(0, m_width, m_height, format, 0);
    if (FAILED(hr))
    {
        LogF("SetOutputMode(%s): ResizeBuffers failed: 0x%08lx", OutputModeName(mode), static_cast<unsigned long>(hr));
        if (mode != OutputMode::Sdr8)
            return SetOutputMode(OutputMode::Sdr8, nullptr);
        m_mode = OutputMode::Sdr8;
        return m_mode;
    }
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&m_backBuffer));

    if (sc4)
    {
        UINT support = 0;
        if (SUCCEEDED(sc4->CheckColorSpaceSupport(colorSpace, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        {
            sc4->SetColorSpace1(colorSpace);
        }
        else if (mode == OutputMode::Hdr10)
        {
            LogF("SetOutputMode: display path does not accept HDR10; using SDR 10-bit");
            return SetOutputMode(OutputMode::Sdr10, nullptr);
        }
        if (mode == OutputMode::Hdr10 && metadata)
            sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(*metadata),
                                const_cast<DXGI_HDR_METADATA_HDR10*>(metadata));
        else
            sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
    }
    m_mode = mode;
    return mode;
}

void GpuDevice::Present()
{
    // VSync on: paces the render loop to the display refresh rate.
    m_swapChain->Present(1, 0);
}

bool GpuDevice::CheckDeviceRemoved()
{
    if (m_deviceRemoved)
        return true;
    const HRESULT reason = m_device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return false;

    m_deviceRemoved = true;
    const char* name = "unknown";
    switch (reason)
    {
    case DXGI_ERROR_DEVICE_HUNG: name = "DEVICE_HUNG (GPU took too long / hung)"; break;
    case DXGI_ERROR_DEVICE_REMOVED: name = "DEVICE_REMOVED"; break;
    case DXGI_ERROR_DEVICE_RESET: name = "DEVICE_RESET (bad commands sent to the GPU)"; break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: name = "DRIVER_INTERNAL_ERROR"; break;
    case DXGI_ERROR_INVALID_CALL: name = "INVALID_CALL"; break;
    default: break;
    }
    LogF("GPU device removed! Reason: 0x%08lx %s", static_cast<unsigned long>(reason), name);
    return true;
}

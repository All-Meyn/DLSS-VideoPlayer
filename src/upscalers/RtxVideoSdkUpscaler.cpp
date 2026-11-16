// NVIDIA RTX Video SDK upscaler backend.
//
// STATUS: integration point only. The SDK's headers are distributed behind an
// NVIDIA developer login and were not available when this file was written, so the
// three SDK calls below are left as clearly marked TODO(SDK) blocks rather than
// guessed. Everything around them (texture formats, rects, quality, lifetime) is
// in place. To finish:
//   1. Download the SDK from developer.nvidia.com/rtx-video-sdk.
//   2. Configure with -DDLSSVP_WITH_RTX_VIDEO_SDK=ON -DRTX_VIDEO_SDK_DIR=<sdk>.
//   3. Fill in the TODO(SDK) blocks from the SDK's DX11 sample / header.
// Until then the player uses the driver VSR path (VideoProcessor.cpp), which runs
// the same RTX Video Super Resolution model through the NVIDIA driver.

#include "upscalers/IUpscaler.h"

#include "Log.h"

#if defined(DLSSVP_HAS_RTX_VIDEO_SDK)

// TODO(SDK): include the SDK's DX11 API header here, e.g. #include "<sdk header>.h"

namespace
{
class RtxVideoSdkUpscaler final : public IUpscaler
{
public:
    ~RtxVideoSdkUpscaler() override
    {
        if (m_initialized)
        {
            // TODO(SDK): call the SDK's DX11 shutdown / release function.
        }
    }

    const char* Name() const override { return "RTX Video SDK"; }

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context) override
    {
        (void)device;
        (void)context;
        // TODO(SDK): create the SDK's DX11 instance on `device`, enabling super resolution
        // (and optionally artifact reduction). Set m_initialized = true on success.
        LogF("RTX Video SDK backend not wired yet - see TODO(SDK) in RtxVideoSdkUpscaler.cpp");
        return m_initialized;
    }

    // Check these against the SDK's documented texture requirements.
    DXGI_FORMAT InputFormat() const override { return DXGI_FORMAT_B8G8R8A8_UNORM; }
    DXGI_FORMAT OutputFormat() const override { return DXGI_FORMAT_B8G8R8A8_UNORM; }
    UINT InputBindFlags() const override { return D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; }
    UINT OutputBindFlags() const override
    {
        return D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    }

    bool Upscale(ID3D11Texture2D* input, const RECT& inputRect, ID3D11Texture2D* output, const RECT& outputRect,
                 int quality) override
    {
        (void)input;
        (void)inputRect;
        (void)output;
        (void)outputRect;
        (void)quality;
        // TODO(SDK): call the SDK's DX11 evaluate function with the input/output textures,
        // their rects, and the VSR quality level (1..4). Return true on success.
        return false;
    }

private:
    bool m_initialized = false;
};
} // namespace

std::unique_ptr<IUpscaler> CreateRtxVideoSdkUpscaler(ID3D11Device* device, ID3D11DeviceContext* context)
{
    auto upscaler = std::make_unique<RtxVideoSdkUpscaler>();
    if (!upscaler->Init(device, context))
        return nullptr;
    return upscaler;
}

#else

std::unique_ptr<IUpscaler> CreateRtxVideoSdkUpscaler(ID3D11Device*, ID3D11DeviceContext*)
{
    return nullptr;
}

#endif

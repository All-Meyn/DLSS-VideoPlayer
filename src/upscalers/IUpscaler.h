#pragma once

#include <d3d11.h>

#include <memory>

// A texture-to-texture GPU upscaler (e.g. NVIDIA RTX Video SDK). Implementations
// receive an RGBA texture at source resolution and write into an output texture
// at display resolution. Add new backends (DLSS, FSR, a custom shader...) here.
class IUpscaler
{
public:
    virtual ~IUpscaler() = default;

    virtual const char* Name() const = 0;

    virtual bool Init(ID3D11Device* device, ID3D11DeviceContext* context) = 0;

    // Texture formats the backend accepts (input and output use the same format), and the
    // bind flags it needs on its input / output textures.
    virtual bool SupportsFormat(DXGI_FORMAT format) const = 0;
    virtual UINT InputBindFlags() const = 0;
    virtual UINT OutputBindFlags() const = 0;

    // quality: 1 (fastest) .. 4 (best).
    virtual bool Upscale(ID3D11Texture2D* input, const RECT& inputRect, ID3D11Texture2D* output,
                         const RECT& outputRect, int quality) = 0;

    // Optional SDR -> HDR conversion (NVIDIA RTX Video HDR / "TrueHDR"). Input: SDR RGB
    // (RGBA8 or RGB10A2, BT.709 gamma 2.2). Output: HDR10 (RGB10A2, PQ / BT.2020), UAV-capable.
    // Runs after Upscale (VSR first, then SDR->HDR, as NVIDIA's samples do).
    struct SdrToHdrSettings
    {
        unsigned contrast = 100;     // 0..200
        unsigned saturation = 100;   // 0..200
        unsigned middleGray = 50;    // 10..100
        unsigned maxLuminance = 1000; // 400..2000 nits, normally the display's peak
    };
    virtual bool SupportsSdrToHdr() const { return false; }
    virtual bool SdrToHdr(ID3D11Texture2D* /*input*/, const RECT& /*inputRect*/, ID3D11Texture2D* /*output*/,
                          const RECT& /*outputRect*/, const SdrToHdrSettings& /*settings*/)
    {
        return false;
    }
};

// Returns nullptr when the player was built without the RTX Video SDK
// (CMake option DLSSVP_WITH_RTX_VIDEO_SDK=OFF) or the SDK failed to initialize.
std::unique_ptr<IUpscaler> CreateRtxVideoSdkUpscaler(ID3D11Device* device, ID3D11DeviceContext* context);

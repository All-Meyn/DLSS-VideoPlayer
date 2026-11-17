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
};

// Returns nullptr when the player was built without the RTX Video SDK
// (CMake option DLSSVP_WITH_RTX_VIDEO_SDK=OFF) or the SDK failed to initialize.
std::unique_ptr<IUpscaler> CreateRtxVideoSdkUpscaler(ID3D11Device* device, ID3D11DeviceContext* context);

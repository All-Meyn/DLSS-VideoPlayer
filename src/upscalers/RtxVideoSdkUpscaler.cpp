// NVIDIA RTX Video SDK upscaler backend (RTX Video Super Resolution through NGX).
//
// Flow (from nvsdk_ngx.h / nvsdk_ngx_helpers_vsr.h in RTX Video SDK 1.1):
//   NVSDK_NGX_D3D11_Init_with_ProjectID -> AllocateParameters -> CreateFeature(VSR)
//   per frame: NGX_D3D11_EVALUATE_VSR_EXT (Input1 / Output / Rect / OutRect / QualityLevel)
//   teardown:  ReleaseFeature -> DestroyParameters -> Shutdown1(device)
//
// NGX is not thread-safe; like the rest of the renderer, this runs on the UI thread only.

#include "upscalers/IUpscaler.h"

#include "Log.h"

#if defined(DLSSVP_HAS_RTX_VIDEO_SDK)

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_vsr.h>

#include <algorithm>
#include <string>

namespace
{
// No NVIDIA-issued application ID: per nvsdk_ngx_defs.h, use a project ID with
// NVSDK_NGX_ENGINE_TYPE_CUSTOM. Any stable GUID-style string identifying this app.
constexpr char kProjectId[] = "94a5435a-2125-4585-8888-6910f8267c9d";
constexpr char kEngineVersion[] = "0.1.0";

const wchar_t* ResultText(NVSDK_NGX_Result r)
{
    const wchar_t* s = GetNGXResultAsString(r);
    return s ? s : L"?";
}

std::wstring AppDataPath()
{
    // NGX writes logs here; it must be writable.
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"DLSSVideoPlayer";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

class RtxVideoSdkUpscaler final : public IUpscaler
{
public:
    ~RtxVideoSdkUpscaler() override
    {
        if (m_handle)
            NVSDK_NGX_D3D11_ReleaseFeature(m_handle);
        if (m_params)
            NVSDK_NGX_D3D11_DestroyParameters(m_params);
        if (m_ngxInitialized)
            NVSDK_NGX_D3D11_Shutdown1(m_device);
    }

    const char* Name() const override { return "RTX Video SDK"; }

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context) override
    {
        m_device = device;
        m_context = context;

        const std::wstring dataPath = AppDataPath();
        NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                 kEngineVersion, dataPath.c_str(), device);
        if (NVSDK_NGX_FAILED(r))
        {
            LogF("RTX Video SDK: NGX init failed: %ls (0x%08x)", ResultText(r), static_cast<unsigned>(r));
            return false;
        }
        m_ngxInitialized = true;

        // Capability check (best effort: an older driver may not support capability queries).
        NVSDK_NGX_Parameter* caps = nullptr;
        r = NVSDK_NGX_D3D11_GetCapabilityParameters(&caps);
        if (NVSDK_NGX_SUCCEED(r) && caps)
        {
            int available = 0;
            int needsDriver = 0;
            NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_VSR_Available, &available);
            NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_VSR_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_D3D11_DestroyParameters(caps);
            if (needsDriver)
            {
                LogF("RTX Video SDK: VSR needs a newer NVIDIA driver");
                return false;
            }
            if (!available)
                LogF("RTX Video SDK: capability query reports VSR unavailable; trying to create it anyway");
        }

        r = NVSDK_NGX_D3D11_AllocateParameters(&m_params);
        if (NVSDK_NGX_FAILED(r) || !m_params)
        {
            LogF("RTX Video SDK: AllocateParameters failed: %ls", ResultText(r));
            return false;
        }

        NVSDK_NGX_Feature_Create_Params createParams{}; // unused by the D3D11 VSR helper
        r = NGX_D3D11_CREATE_VSR_EXT(context, &m_handle, m_params, &createParams);
        if (NVSDK_NGX_FAILED(r) || !m_handle)
        {
            LogF("RTX Video SDK: CreateFeature(VSR) failed: %ls (0x%08x)", ResultText(r), static_cast<unsigned>(r));
            m_handle = nullptr;
            return false;
        }

        LogF("RTX Video SDK: VSR feature created");
        return true;
    }

    // NGX lists RGBA8 as its 8-bit buffer format; the output must be UAV-capable
    // (NVSDK_NGX_Result_FAIL_RWFlagMissing otherwise).
    DXGI_FORMAT InputFormat() const override { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    DXGI_FORMAT OutputFormat() const override { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    UINT InputBindFlags() const override { return D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; }
    UINT OutputBindFlags() const override
    {
        return D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    }

    bool Upscale(ID3D11Texture2D* input, const RECT& inRect, ID3D11Texture2D* output, const RECT& outRect,
                 int quality) override
    {
        NVSDK_NGX_D3D11_VSR_Eval_Params ep{};
        ep.pInput = input;
        ep.pOutput = output;
        ep.InputSubrectBase = {static_cast<unsigned>(inRect.left), static_cast<unsigned>(inRect.top)};
        ep.InputSubrectSize = {static_cast<unsigned>(inRect.right - inRect.left),
                               static_cast<unsigned>(inRect.bottom - inRect.top)};
        ep.OutputSubrectBase = {static_cast<unsigned>(outRect.left), static_cast<unsigned>(outRect.top)};
        ep.OutputSubrectSize = {static_cast<unsigned>(outRect.right - outRect.left),
                                static_cast<unsigned>(outRect.bottom - outRect.top)};
        // Player quality 1..4 maps directly onto Low / Medium / High / Ultra.
        ep.QualityLevel = static_cast<NVSDK_NGX_VSR_QualityLevel>(std::clamp(quality, 1, 4));

        const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_VSR_EXT(m_context, m_handle, m_params, &ep);
        if (NVSDK_NGX_FAILED(r))
        {
            if (!m_loggedEvalError)
            {
                LogF("RTX Video SDK: evaluate failed: %ls (0x%08x)", ResultText(r), static_cast<unsigned>(r));
                m_loggedEvalError = true;
            }
            return false;
        }
        return true;
    }

private:
    ID3D11Device* m_device = nullptr;          // owned by GpuDevice, outlives this object
    ID3D11DeviceContext* m_context = nullptr;  // owned by GpuDevice
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    bool m_ngxInitialized = false;
    bool m_loggedEvalError = false;
};
} // namespace

std::unique_ptr<IUpscaler> CreateRtxVideoSdkUpscaler(ID3D11Device* device, ID3D11DeviceContext* context)
{
    auto upscaler = std::make_unique<RtxVideoSdkUpscaler>();
    if (!upscaler->Init(device, context))
        return nullptr; // destructor cleans up whatever was initialized
    return upscaler;
}

#else

std::unique_ptr<IUpscaler> CreateRtxVideoSdkUpscaler(ID3D11Device*, ID3D11DeviceContext*)
{
    return nullptr;
}

#endif

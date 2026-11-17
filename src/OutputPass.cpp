#include "OutputPass.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <cstring>

namespace
{
// Full-screen triangle (no vertex buffer) + PQ-domain overlay compositing.
constexpr char kShaderSource[] = R"hlsl(
Texture2D<float4> VideoTex : register(t0);   // PQ-encoded BT.2020 (HDR10)
Texture2D<float4> UiTex    : register(t1);   // premultiplied alpha, sRGB-like gamma, BT.709

cbuffer Constants : register(b0)
{
    float SdrWhiteNits;
    uint  UseOverlay;
    float2 Padding;
};

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// SMPTE ST 2084 (PQ). Linear values are normalized so 1.0 = 10,000 nits.
static const float m1 = 2610.0 / 16384.0;
static const float m2 = 2523.0 / 4096.0 * 128.0;
static const float c1 = 3424.0 / 4096.0;
static const float c2 = 2413.0 / 4096.0 * 32.0;
static const float c3 = 2392.0 / 4096.0 * 32.0;

float3 PqToLinear(float3 pq)
{
    float3 p = pow(saturate(pq), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float3 LinearToPq(float3 lin)
{
    float3 y = pow(saturate(lin), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// BT.709 -> BT.2020 primaries (linear light).
static const float3x3 Bt709To2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950
};

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    int3 p = int3(int2(pos.xy), 0);
    float3 video = VideoTex.Load(p).rgb;
    if (UseOverlay != 0)
    {
        float4 ui = UiTex.Load(p);
        if (ui.a > 0.0)
        {
            float3 uiLinear = mul(Bt709To2020, pow(ui.rgb / ui.a, 2.2)) * (SdrWhiteNits / 10000.0);
            float3 videoLinear = PqToLinear(video);
            video = LinearToPq(uiLinear * ui.a + videoLinear * (1.0 - ui.a));
        }
    }
    return float4(video, 1.0);
}
)hlsl";

struct Constants
{
    float sdrWhiteNits;
    UINT useOverlay;
    float padding[2];
};

ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "OutputPass.hlsl", nullptr, nullptr, entry,
                                  target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        LogF("OutputPass: compiling %s failed (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return nullptr;
    }
    return code;
}
} // namespace

bool OutputPass::Init(GpuDevice* gpu)
{
    m_gpu = gpu;
    ComPtr<ID3DBlob> vs = Compile("VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps = Compile("PSMain", "ps_5_0");
    if (!vs || !ps)
        return false;

    ID3D11Device* device = gpu->Device();
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps)) ||
        FAILED(device->CreateBuffer(&cb, nullptr, &m_constants)))
    {
        LogF("OutputPass: creating shaders failed; HDR output disabled");
        m_ps.Reset();
        return false;
    }
    return true;
}

void OutputPass::ReleaseTarget()
{
    m_rtv.Reset();
}

void OutputPass::Run(ID3D11ShaderResourceView* video, ID3D11ShaderResourceView* overlay, float sdrWhiteNits)
{
    if (!m_ps || !video || !m_gpu->BackBuffer())
        return;
    ID3D11DeviceContext* ctx = m_gpu->Context();
    if (!m_rtv && FAILED(m_gpu->Device()->CreateRenderTargetView(m_gpu->BackBuffer(), nullptr, &m_rtv)))
        return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        const Constants c{sdrWhiteNits, overlay ? 1u : 0u, {0.0f, 0.0f}};
        memcpy(mapped.pData, &c, sizeof(c));
        ctx->Unmap(m_constants.Get(), 0);
    }

    // Set every piece of state this pass relies on (Direct2D / NGX change pipeline state too).
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_gpu->Width()), static_cast<float>(m_gpu->Height()),
                                  0.0f, 1.0f};
    ID3D11ShaderResourceView* srvs[2] = {video, overlay};
    ID3D11RenderTargetView* rtv = m_rtv.Get();
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &viewport);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);

    // Unbind: the video texture is a video-processor output again next frame.
    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nullSrvs);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

#include "OutputPass.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <cstring>

namespace
{
// Full-screen triangle (no vertex buffer) + PQ-domain overlay compositing.
constexpr char kShaderSource[] = R"hlsl(
Texture2D<float4> VideoTex : register(t0);   // PQ-encoded BT.2020 (HDR10) or SDR BT.709
Texture2D<float4> UiTex    : register(t1);   // premultiplied alpha, sRGB-like gamma, BT.709

cbuffer Constants : register(b0)
{
    float SdrWhiteNits;
    uint  UseOverlay;
    uint  Mode;          // 0 = SDR, 1 = HDR10, 2 = PQ -> SDR tone mapping
    float SrcPeakNits;
    float DstPeakNits;
    float3 Padding;
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

float PqToLinear1(float pq) { return PqToLinear(pq.xxx).x; }
float LinearToPq1(float lin) { return LinearToPq(lin.xxx).x; }

// BT.709 <-> BT.2020 primaries (linear light).
static const float3x3 Bt709To2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950
};
static const float3x3 Bt2020To709 = {
     1.6604910, -0.5876411, -0.0728499,
    -0.1245505,  1.1328999, -0.0083494,
    -0.0181508, -0.1005789,  1.1187297
};

// ITU-R BT.2390 EETF (black level 0): maps PQ value e in [0, srcPeakPq] to [0, dstPeakPq]
// with a Hermite-spline knee, leaving everything below the knee untouched.
float Bt2390Eetf(float e, float srcPeakPq, float dstPeakPq)
{
    float e1 = e / srcPeakPq;
    float maxLum = dstPeakPq / srcPeakPq;
    float ks = 1.5 * maxLum - 0.5;
    float e2 = e1;
    if (e1 > ks)
    {
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t;
        float t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    return min(e2, 1.0) * srcPeakPq;
}

float3 ToneMapPqToSdr(float3 pq)
{
    float3 nits = PqToLinear(pq) * 10000.0; // BT.2020 linear, in nits
    float peak = max(max(nits.r, nits.g), nits.b);
    if (peak > 0.0 && SrcPeakNits > DstPeakNits)
    {
        // Tone-map the brightest channel and scale all three by the same factor (keeps hue).
        float srcPeakPq = LinearToPq1(SrcPeakNits / 10000.0);
        float dstPeakPq = LinearToPq1(DstPeakNits / 10000.0);
        float peakPq = LinearToPq1(min(peak, SrcPeakNits) / 10000.0);
        float mapped = PqToLinear1(Bt2390Eetf(peakPq, srcPeakPq, dstPeakPq)) * 10000.0;
        nits *= mapped / peak;
    }
    float3 sdr = saturate(mul(Bt2020To709, nits) / DstPeakNits); // gamut clip to BT.709
    return pow(sdr, 1.0 / 2.2);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    int3 p = int3(int2(pos.xy), 0);
    float3 video = VideoTex.Load(p).rgb;
    if (Mode == 2)
        video = ToneMapPqToSdr(video);

    if (UseOverlay != 0)
    {
        float4 ui = UiTex.Load(p);
        if (ui.a > 0.0)
        {
            if (Mode == 1)
            {
                float3 uiLinear = mul(Bt709To2020, pow(ui.rgb / ui.a, 2.2)) * (SdrWhiteNits / 10000.0);
                float3 videoLinear = PqToLinear(video);
                video = LinearToPq(uiLinear * ui.a + videoLinear * (1.0 - ui.a));
            }
            else
            {
                video = ui.rgb + video * (1.0 - ui.a); // premultiplied "over", same as D2D in 8-bit
            }
        }
    }
    return float4(video, 1.0);
}
)hlsl";

struct Constants // matches the HLSL cbuffer (2 x 16 bytes)
{
    float sdrWhiteNits;
    UINT useOverlay;
    UINT mode;
    float srcPeakNits;
    float dstPeakNits;
    float padding[3];
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

void OutputPass::Run(ID3D11ShaderResourceView* video, ID3D11ShaderResourceView* overlay, const Params& params)
{
    if (!m_ps || !video || !m_gpu->BackBuffer())
        return;
    ID3D11DeviceContext* ctx = m_gpu->Context();
    if (!m_rtv && FAILED(m_gpu->Device()->CreateRenderTargetView(m_gpu->BackBuffer(), nullptr, &m_rtv)))
        return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        const Constants c{params.sdrWhiteNits, overlay ? 1u : 0u, static_cast<UINT>(params.mode),
                          params.srcPeakNits, params.dstPeakNits, {0.0f, 0.0f, 0.0f}};
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

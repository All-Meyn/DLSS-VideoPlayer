#include "VideoProcessor.h"

#include "Log.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace
{
// NVIDIA driver stream extension for RTX Video Super Resolution. This is the same
// GUID / payload used by Chromium and VLC; it is a no-op (returns an error) on
// non-NVIDIA drivers. The user must also have "RTX Video Enhancement > Super
// resolution" enabled in the NVIDIA Control Panel.
constexpr GUID kNvidiaPPEInterfaceGuid = {0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
constexpr UINT kNvStreamExtensionVersionV1 = 0x1;
constexpr UINT kNvStreamExtensionMethodSuperResolution = 0x2;

struct NvStreamExtension
{
    UINT version;
    UINT method;
    UINT enable;
};

enum class Matrix { Bt601, Bt709, Bt2020 };

Matrix PickMatrix(const AVFrame* f)
{
    if (f)
    {
        switch (f->colorspace)
        {
        case AVCOL_SPC_BT709: return Matrix::Bt709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return Matrix::Bt601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return Matrix::Bt2020;
        default: break;
        }
        // Unspecified: common heuristic - SD is 601, HD and up is 709.
        return f->height >= 720 ? Matrix::Bt709 : Matrix::Bt601;
    }
    return Matrix::Bt709;
}
} // namespace

bool VideoProcessor::Init(ID3D11Device* device, ID3D11VideoDevice* videoDevice, ID3D11VideoContext* videoContext)
{
    m_device = device;
    m_videoDevice = videoDevice;
    m_videoContext = videoContext;
    m_videoContext.As(&m_videoContext1); // Optional (Windows 8.1+); falls back to legacy color API.
    return true;
}

bool VideoProcessor::EnsureProcessor(const D3D11_TEXTURE2D_DESC& in, const D3D11_TEXTURE2D_DESC& out)
{
    if (m_processor && in.Width == m_inW && in.Height == m_inH && out.Width == m_outW && out.Height == m_outH)
        return true;

    m_processor.Reset();
    m_enumerator.Reset();
    m_nvVsrStateKnown = false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {1, 1};
    cd.InputWidth = in.Width;
    cd.InputHeight = in.Height;
    cd.OutputFrameRate = {1, 1};
    cd.OutputWidth = out.Width;
    cd.OutputHeight = out.Height;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&cd, &m_enumerator);
    if (FAILED(hr))
    {
        LogF("CreateVideoProcessorEnumerator failed: 0x%08lx", hr);
        return false;
    }

    UINT support = 0;
    if (FAILED(m_enumerator->CheckVideoProcessorFormat(in.Format, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
    {
        LogF("Video processor does not accept input format %d", in.Format);
        return false;
    }
    if (FAILED(m_enumerator->CheckVideoProcessorFormat(out.Format, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
    {
        LogF("Video processor does not support output format %d", out.Format);
        return false;
    }

    hr = m_videoDevice->CreateVideoProcessor(m_enumerator.Get(), 0, &m_processor);
    if (FAILED(hr))
    {
        LogF("CreateVideoProcessor failed: 0x%08lx", hr);
        return false;
    }

    // Disable driver "auto" enhancements so the only processing is what we ask for.
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.Get(), 0, FALSE);
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    m_inW = in.Width;
    m_inH = in.Height;
    m_outW = out.Width;
    m_outH = out.Height;
    return true;
}

void VideoProcessor::ApplyColorSpace(const AVFrame* f)
{
    const Matrix matrix = PickMatrix(f);
    const bool fullRange = f && f->color_range == AVCOL_RANGE_JPEG;
    const bool pq = f && f->color_trc == AVCOL_TRC_SMPTE2084;

    if (m_videoContext1)
    {
        DXGI_COLOR_SPACE_TYPE in = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        switch (matrix)
        {
        case Matrix::Bt601:
            in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
            break;
        case Matrix::Bt709:
            in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
            break;
        case Matrix::Bt2020:
            if (pq)
                in = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020; // HDR10; SDR output tone-mapping is driver-defined
            else
                in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
            break;
        }
        m_videoContext1->VideoProcessorSetStreamColorSpace1(m_processor.Get(), 0, in);
        m_videoContext1->VideoProcessorSetOutputColorSpace1(m_processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
    else
    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.YCbCr_Matrix = matrix == Matrix::Bt601 ? 0 : 1;
        in.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.Get(), 0, &in);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.RGB_Range = 0; // full range RGB
        m_videoContext->VideoProcessorSetOutputColorSpace(m_processor.Get(), &out);
    }
}

void VideoProcessor::ApplyNvidiaVsr(bool enable)
{
    if (m_nvVsrStateKnown && m_nvVsrEnabled == enable)
        return;

    NvStreamExtension ext{kNvStreamExtensionVersionV1, kNvStreamExtensionMethodSuperResolution, enable ? 1u : 0u};
    HRESULT hr = m_videoContext->VideoProcessorSetStreamExtension(m_processor.Get(), 0, &kNvidiaPPEInterfaceGuid,
                                                                  sizeof(ext), &ext);
    m_nvVsrStateKnown = true;
    m_nvVsrEnabled = enable;
    m_nvVsrAccepted = enable && SUCCEEDED(hr);
    if (enable && FAILED(hr))
        LogF("NVIDIA VSR stream extension rejected: 0x%08lx (non-NVIDIA GPU or old driver)", hr);
}

bool VideoProcessor::Blit(ID3D11Texture2D* src, UINT srcSlice, const RECT& srcRect, const AVFrame* colorInfo,
                          ID3D11Texture2D* dst, const RECT& dstRect, bool nvidiaVsr)
{
    D3D11_TEXTURE2D_DESC inDesc{}, outDesc{};
    src->GetDesc(&inDesc);
    dst->GetDesc(&outDesc);
    if (!EnsureProcessor(inDesc, outDesc))
        return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = srcSlice;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(src, m_enumerator.Get(), &ivd, &inView);
    if (FAILED(hr))
    {
        LogF("CreateVideoProcessorInputView failed: 0x%08lx", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    hr = m_videoDevice->CreateVideoProcessorOutputView(dst, m_enumerator.Get(), &ovd, &outView);
    if (FAILED(hr))
    {
        LogF("CreateVideoProcessorOutputView failed: 0x%08lx", hr);
        return false;
    }

    ID3D11VideoProcessor* vp = m_processor.Get();
    const RECT fullOut{0, 0, static_cast<LONG>(outDesc.Width), static_cast<LONG>(outDesc.Height)};
    const D3D11_VIDEO_COLOR black{{0.0f, 0.0f, 0.0f, 1.0f}};

    m_videoContext->VideoProcessorSetOutputTargetRect(vp, TRUE, &fullOut);
    m_videoContext->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);
    m_videoContext->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &srcRect);
    m_videoContext->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dstRect);
    ApplyColorSpace(colorInfo);
    ApplyNvidiaVsr(nvidiaVsr);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    hr = m_videoContext->VideoProcessorBlt(vp, outView.Get(), 0, 1, &stream);
    if (FAILED(hr))
    {
        LogF("VideoProcessorBlt failed: 0x%08lx", hr);
        return false;
    }
    return true;
}

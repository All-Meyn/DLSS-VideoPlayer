#include "DisplayInfo.h"

#include "Log.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace
{
LONG Overlap(const RECT& a, const RECT& b)
{
    const LONG w = std::min(a.right, b.right) - std::max(a.left, b.left);
    const LONG h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
    return (w > 0 && h > 0) ? w * h : 0;
}

// Windows "SDR content brightness" for the monitor with GDI name `deviceName`, in nits.
float QuerySdrWhiteNits(const std::wstring& deviceName)
{
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return 0.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return 0.0f;

    for (UINT32 i = 0; i < pathCount; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || deviceName != source.viewGdiDeviceName)
            continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
            return static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f; // documented scale: 1000 = 80 nits
    }
    return 0.0f;
}

UINT16 Chroma(AVRational v)
{
    return static_cast<UINT16>(std::clamp(av_q2d(v), 0.0, 1.0) * 50000.0 + 0.5); // DXGI unit: 0.00002
}
} // namespace

DisplayInfo QueryDisplayInfo(HWND hwnd)
{
    DisplayInfo info;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return info;

    RECT window{};
    GetWindowRect(hwnd, &window);
    ComPtr<IDXGIOutput> best;
    LONG bestArea = -1;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            const LONG area = Overlap(window, desc.DesktopCoordinates);
            if (area > bestArea)
            {
                bestArea = area;
                best = output;
            }
        }
    }

    ComPtr<IDXGIOutput6> output6;
    if (!best || FAILED(best.As(&output6)))
        return info;
    DXGI_OUTPUT_DESC1 desc{};
    if (FAILED(output6->GetDesc1(&desc)))
        return info;

    info.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    info.maxNits = desc.MaxLuminance;
    info.deviceName = desc.DeviceName;
    const float white = QuerySdrWhiteNits(info.deviceName);
    if (white > 0.0f)
        info.sdrWhiteNits = white;
    return info;
}

bool IsHdr10Frame(const AVFrame* frame, DXGI_FORMAT surfaceFormat)
{
    return frame && frame->color_trc == AVCOL_TRC_SMPTE2084 && surfaceFormat == DXGI_FORMAT_P010;
}

DXGI_HDR_METADATA_HDR10 HdrMetadataFromFrame(const AVFrame* frame)
{
    // Defaults: BT.2020 primaries, D65 white, 1000-nit mastering display.
    DXGI_HDR_METADATA_HDR10 md{};
    md.RedPrimary[0] = 35400;   // 0.708
    md.RedPrimary[1] = 14600;   // 0.292
    md.GreenPrimary[0] = 8500;  // 0.170
    md.GreenPrimary[1] = 39850; // 0.797
    md.BluePrimary[0] = 6550;   // 0.131
    md.BluePrimary[1] = 2300;   // 0.046
    md.WhitePoint[0] = 15635;   // 0.3127
    md.WhitePoint[1] = 16450;   // 0.3290
    md.MaxMasteringLuminance = 1000;
    md.MinMasteringLuminance = 50; // 0.005 nits (unit 0.0001)

    if (!frame)
        return md;
    if (const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
    {
        const auto* m = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
        if (m->has_primaries)
        {
            // FFmpeg stores the primaries in R, G, B order.
            md.RedPrimary[0] = Chroma(m->display_primaries[0][0]);
            md.RedPrimary[1] = Chroma(m->display_primaries[0][1]);
            md.GreenPrimary[0] = Chroma(m->display_primaries[1][0]);
            md.GreenPrimary[1] = Chroma(m->display_primaries[1][1]);
            md.BluePrimary[0] = Chroma(m->display_primaries[2][0]);
            md.BluePrimary[1] = Chroma(m->display_primaries[2][1]);
            md.WhitePoint[0] = Chroma(m->white_point[0]);
            md.WhitePoint[1] = Chroma(m->white_point[1]);
        }
        if (m->has_luminance)
        {
            md.MaxMasteringLuminance = static_cast<UINT>(av_q2d(m->max_luminance) + 0.5);
            md.MinMasteringLuminance = static_cast<UINT>(av_q2d(m->min_luminance) * 10000.0 + 0.5);
        }
    }
    if (const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL))
    {
        const auto* c = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
        md.MaxContentLightLevel = static_cast<UINT16>(std::min<unsigned>(c->MaxCLL, 65535));
        md.MaxFrameAverageLightLevel = static_cast<UINT16>(std::min<unsigned>(c->MaxFALL, 65535));
    }
    return md;
}

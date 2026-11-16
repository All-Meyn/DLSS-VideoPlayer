#include "VideoDecoder.h"

#include "Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace
{
std::string Utf8FromWide(const std::wstring& w)
{
    if (w.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

std::string AvErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// Milestone 1 requires GPU decode: we only accept D3D11 surfaces.
AVPixelFormat GetD3D11Format(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == AV_PIX_FMT_D3D11)
            return *p;
    }
    LogF("Decoder did not offer AV_PIX_FMT_D3D11 (codec/profile not supported by GPU decoder)");
    return AV_PIX_FMT_NONE;
}
} // namespace

VideoDecoder::~VideoDecoder()
{
    Close();
}

bool VideoDecoder::Open(const std::wstring& path, ID3D11Device* device, std::string& error)
{
    Close();

    const std::string utf8Path = Utf8FromWide(path);
    int ret = avformat_open_input(&m_fmtCtx, utf8Path.c_str(), nullptr, nullptr);
    if (ret < 0)
    {
        error = "Could not open file: " + AvErr(ret);
        return false;
    }
    if ((ret = avformat_find_stream_info(m_fmtCtx, nullptr)) < 0)
    {
        error = "Could not read stream info: " + AvErr(ret);
        Close();
        return false;
    }

    const AVCodec* codec = nullptr;
    m_streamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m_streamIndex < 0 || !codec)
    {
        error = "No decodable video stream found";
        Close();
        return false;
    }

    bool hasD3D11 = false;
    for (int i = 0;; ++i)
    {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg)
            break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA)
            hasD3D11 = true;
    }
    if (!hasD3D11)
    {
        error = std::string("Codec '") + codec->name + "' has no D3D11VA hardware decoder in this FFmpeg build";
        Close();
        return false;
    }

    AVStream* stream = m_fmtCtx->streams[m_streamIndex];
    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, stream->codecpar);

    // Wrap our own D3D11 device so decoded surfaces live on the renderer's device (zero-copy).
    m_hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDevice->data);
    auto* d3dCtx = static_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);
    device->AddRef(); // FFmpeg releases this reference when the hw device context is freed.
    d3dCtx->device = device;
    if ((ret = av_hwdevice_ctx_init(m_hwDevice)) < 0)
    {
        error = "av_hwdevice_ctx_init failed: " + AvErr(ret);
        Close();
        return false;
    }

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDevice);
    m_codecCtx->get_format = GetD3D11Format;
    // Single-threaded decode keeps all D3D11 context use on the UI thread.
    m_codecCtx->thread_count = 1;
    // We hold up to two frames (current + next) on top of the decoder's reference set.
    m_codecCtx->extra_hw_frames = 4;
    m_codecCtx->pkt_timebase = stream->time_base;

    if ((ret = avcodec_open2(m_codecCtx, codec, nullptr)) < 0)
    {
        error = "avcodec_open2 failed: " + AvErr(ret);
        Close();
        return false;
    }

    m_packet = av_packet_alloc();
    m_timeBase = stream->time_base;
    m_startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;
    m_sar = av_guess_sample_aspect_ratio(m_fmtCtx, stream, nullptr);
    if (m_sar.num <= 0 || m_sar.den <= 0)
        m_sar = AVRational{1, 1};
    m_duration = m_fmtCtx->duration > 0 ? static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE : 0.0;
    m_codecName = codec->name;
    m_flushSent = false;
    m_lastPts = 0.0;

    LogF("Opened %s: %dx%d, codec %s, duration %.2fs", utf8Path.c_str(), m_width, m_height, m_codecName, m_duration);
    return true;
}

void VideoDecoder::Close()
{
    av_packet_free(&m_packet);
    avcodec_free_context(&m_codecCtx);
    av_buffer_unref(&m_hwDevice);
    avformat_close_input(&m_fmtCtx);
    m_streamIndex = -1;
}

VideoDecoder::Result VideoDecoder::Decode(VideoFrame& out)
{
    if (!m_codecCtx)
        return Result::Error;

    AVFramePtr frame(av_frame_alloc());
    for (;;)
    {
        int ret = avcodec_receive_frame(m_codecCtx, frame.get());
        if (ret == 0)
        {
            int64_t ts = frame->best_effort_timestamp;
            if (ts != AV_NOPTS_VALUE)
            {
                m_lastPts = static_cast<double>(ts - m_startTime) * av_q2d(m_timeBase);
            }
            else if (frame->duration > 0)
            {
                m_lastPts += static_cast<double>(frame->duration) * av_q2d(m_timeBase);
            }
            if (frame->width > 0 && frame->height > 0)
            {
                m_width = frame->width;
                m_height = frame->height;
            }
            out.pts = m_lastPts;
            out.frame = std::move(frame);
            return Result::Frame;
        }
        if (ret == AVERROR_EOF)
            return Result::EndOfStream;
        if (ret != AVERROR(EAGAIN))
        {
            LogF("avcodec_receive_frame: %s", AvErr(ret).c_str());
            return Result::Error;
        }

        // Decoder needs more input.
        if (m_flushSent)
            return Result::EndOfStream;

        ret = av_read_frame(m_fmtCtx, m_packet);
        if (ret < 0)
        {
            // End of file (or read error): drain the decoder.
            avcodec_send_packet(m_codecCtx, nullptr);
            m_flushSent = true;
            continue;
        }
        if (m_packet->stream_index == m_streamIndex)
        {
            ret = avcodec_send_packet(m_codecCtx, m_packet);
            if (ret < 0 && ret != AVERROR(EAGAIN))
                LogF("avcodec_send_packet: %s (skipping packet)", AvErr(ret).c_str());
        }
        av_packet_unref(m_packet);
    }
}

bool VideoDecoder::Seek(double seconds)
{
    if (!m_fmtCtx)
        return false;
    if (seconds < 0.0)
        seconds = 0.0;

    const int64_t ts = m_startTime + static_cast<int64_t>(seconds / av_q2d(m_timeBase));
    int ret = av_seek_frame(m_fmtCtx, m_streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
    {
        LogF("av_seek_frame: %s", AvErr(ret).c_str());
        return false;
    }
    avcodec_flush_buffers(m_codecCtx);
    m_flushSent = false;
    m_lastPts = seconds;
    return true;
}

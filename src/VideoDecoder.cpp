#include "VideoDecoder.h"

#include "Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
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

std::wstring Widen(const char* s)
{
    if (!s || !*s)
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 1)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), len);
    return w;
}

// "English - Commentary (ac3, 5.1)" from the stream's metadata.
StreamInfo DescribeStream(const AVStream* st, int number)
{
    StreamInfo info;
    info.index = st->index;
    info.isDefault = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
    info.isForced = (st->disposition & AV_DISPOSITION_FORCED) != 0;
    const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
    const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);

    std::wstring label = lang ? Widen(lang->value) : L"Track " + std::to_wstring(number);
    if (title && title->value[0])
        label += L" - " + Widen(title->value);
    label += L" (" + Widen(avcodec_get_name(st->codecpar->codec_id));
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        const int ch = st->codecpar->ch_layout.nb_channels;
        label += ch == 6 ? L", 5.1" : ch == 8 ? L", 7.1" : ch == 2 ? L", stereo" : ch == 1 ? L", mono"
                                                                                : L", " + std::to_wstring(ch) + L" ch";
    }
    label += L")";
    if (info.isForced)
        label += L" [forced]";
    info.label = std::move(label);
    return info;
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

    // Audio / subtitle tracks for the selection menus.
    m_audioStreams.clear();
    m_subtitleStreams.clear();
    for (unsigned i = 0; i < m_fmtCtx->nb_streams; ++i)
    {
        const AVStream* st = m_fmtCtx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            m_audioStreams.push_back(DescribeStream(st, static_cast<int>(m_audioStreams.size()) + 1));
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            m_subtitleStreams.push_back(DescribeStream(st, static_cast<int>(m_subtitleStreams.size()) + 1));
    }
    m_defaultAudio = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, m_streamIndex, nullptr, 0);

    LogF("Opened %s: %dx%d, codec %s, duration %.2fs, %zu audio / %zu subtitle tracks", utf8Path.c_str(), m_width,
         m_height, m_codecName, m_duration, m_audioStreams.size(), m_subtitleStreams.size());
    return true;
}

void VideoDecoder::CloseSubtitleDecoder()
{
    avcodec_free_context(&m_subCtx);
    m_subStreamIndex = -1;
    m_subs.Clear();
}

bool VideoDecoder::SelectSubtitleStream(int streamIndex)
{
    CloseSubtitleDecoder();
    if (streamIndex < 0 || !m_fmtCtx || streamIndex >= static_cast<int>(m_fmtCtx->nb_streams))
        return streamIndex < 0;
    const AVStream* st = m_fmtCtx->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
    {
        LogF("Subtitles: no decoder for %s", avcodec_get_name(st->codecpar->codec_id));
        return false;
    }
    m_subCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_subCtx, st->codecpar);
    m_subCtx->pkt_timebase = st->time_base;
    if (avcodec_open2(m_subCtx, codec, nullptr) < 0)
    {
        LogF("Subtitles: could not open the %s decoder", codec->name);
        avcodec_free_context(&m_subCtx);
        return false;
    }
    m_subStreamIndex = streamIndex;
    // Image subtitles are positioned on their own canvas (usually the video size).
    m_subCanvasW = m_subCtx->width > 0 ? m_subCtx->width : m_width;
    m_subCanvasH = m_subCtx->height > 0 ? m_subCtx->height : m_height;
    LogF("Subtitles: stream %d (%s)", streamIndex, codec->name);
    return true;
}

void VideoDecoder::Close()
{
    CloseSubtitleDecoder();
    m_audioStreams.clear();
    m_subtitleStreams.clear();
    m_defaultAudio = -1;
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
            if (m_lastPts - m_lastPrune > 5.0 || m_lastPts < m_lastPrune)
            {
                m_subs.Prune(m_lastPts, 30.0); // image subtitles can be large: keep ~30 s of history
                m_lastPrune = m_lastPts;
            }
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
        else if (m_subCtx && m_packet->stream_index == m_subStreamIndex)
        {
            // Subtitle decoding is cheap and synchronous: do it right here.
            AVSubtitle sub{};
            int got = 0;
            if (avcodec_decode_subtitle2(m_subCtx, &sub, &got, m_packet) >= 0 && got)
            {
                m_subs.Add(sub, static_cast<double>(m_startTime) * av_q2d(m_timeBase), m_subCanvasW, m_subCanvasH);
                avsubtitle_free(&sub);
            }
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
    if (m_subCtx)
        avcodec_flush_buffers(m_subCtx);
    m_subs.Clear(); // re-decoded from the new position
    m_flushSent = false;
    m_lastPts = seconds;
    return true;
}

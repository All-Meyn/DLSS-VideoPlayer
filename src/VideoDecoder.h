#pragma once

#include <d3d11.h>

#include "Subtitles.h"

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/rational.h>
}

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVBufferRef;

struct AVFrameDeleter
{
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// A decoded GPU frame plus its presentation time in seconds (relative to stream start).
struct VideoFrame
{
    AVFramePtr frame; // format == AV_PIX_FMT_D3D11: data[0] = ID3D11Texture2D*, data[1] = array slice
    double pts = 0.0;

    explicit operator bool() const { return frame != nullptr; }
    ID3D11Texture2D* Texture() const { return reinterpret_cast<ID3D11Texture2D*>(frame->data[0]); }
    UINT ArraySlice() const { return static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1])); }
};

// A selectable audio or subtitle stream of the open file.
struct StreamInfo
{
    int index = -1;     // FFmpeg stream index
    std::wstring label; // e.g. "English - Commentary (ac3, 5.1)"
    bool isDefault = false;
    bool isForced = false;
};

// Demuxes a file with libavformat and decodes its best video stream with D3D11VA,
// on the caller's ID3D11Device. Packets of the selected embedded subtitle stream are
// decoded along the way (same reader). All calls must come from the same thread.
class VideoDecoder
{
public:
    enum class Result { Frame, EndOfStream, Error };

    ~VideoDecoder();

    bool Open(const std::wstring& path, ID3D11Device* device, std::string& error);
    void Close();
    bool IsOpen() const { return m_codecCtx != nullptr; }

    Result Decode(VideoFrame& out);

    // Seeks to the keyframe at or before `seconds`. Callers should discard decoded
    // frames whose pts is earlier than the target.
    bool Seek(double seconds);

    int Width() const { return m_width; }
    int Height() const { return m_height; }
    AVRational SampleAspectRatio() const { return m_sar; }
    double Duration() const { return m_duration; }
    const char* CodecName() const { return m_codecName; }

    // Tracks found in the file. DefaultAudioStream() is FFmpeg's "best" audio stream (-1 = none).
    const std::vector<StreamInfo>& AudioStreams() const { return m_audioStreams; }
    const std::vector<StreamInfo>& SubtitleStreams() const { return m_subtitleStreams; }
    int DefaultAudioStream() const { return m_defaultAudio; }

    // Embedded subtitles: select a subtitle stream index (-1 = none). Events decoded so far
    // are in Subtitles(); callers usually re-seek to the current position after switching
    // so lines already past the read position are decoded again.
    bool SelectSubtitleStream(int streamIndex);
    int SelectedSubtitleStream() const { return m_subStreamIndex; }
    const SubtitleTrack& Subtitles() const { return m_subs; }

private:
    void CloseSubtitleDecoder();
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVPacket* m_packet = nullptr;
    AVBufferRef* m_hwDevice = nullptr;
    int m_streamIndex = -1;
    bool m_flushSent = false;

    AVRational m_timeBase{0, 1};
    int64_t m_startTime = 0;
    int m_width = 0;
    int m_height = 0;
    AVRational m_sar{1, 1};
    double m_duration = 0.0;
    double m_lastPts = 0.0;
    const char* m_codecName = "";

    std::vector<StreamInfo> m_audioStreams;
    std::vector<StreamInfo> m_subtitleStreams;
    int m_defaultAudio = -1;
    int m_subStreamIndex = -1;
    AVCodecContext* m_subCtx = nullptr;
    int m_subCanvasW = 0, m_subCanvasH = 0;
    SubtitleTrack m_subs;
    double m_lastPrune = 0.0;
};

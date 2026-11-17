#include "AudioPlayer.h"

#include "Log.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace
{
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally to avoid ksmedia.h / GUID linkage.
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr REFERENCE_TIME kBufferDuration = 1'000'000; // 100 ms in 100-ns units
constexpr double kMaxClockExtrapolation = 0.25;      // seconds

double NowSeconds()
{
    static const double toSeconds = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * toSeconds;
}

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

template <typename T> void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}
} // namespace

AudioPlayer::~AudioPlayer()
{
    Close();
}

// ---------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------

bool AudioPlayer::Open(const std::wstring& path, double startPts, bool startPaused, float volume)
{
    Close();

    const std::string utf8 = Utf8FromWide(path);
    int ret = avformat_open_input(&m_fmtCtx, utf8.c_str(), nullptr, nullptr);
    if (ret < 0 || (ret = avformat_find_stream_info(m_fmtCtx, nullptr)) < 0)
    {
        LogF("Audio: could not open file: %s", AvErr(ret).c_str());
        Close();
        return false;
    }

    const AVCodec* codec = nullptr;
    m_streamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (m_streamIndex < 0 || !codec)
    {
        LogF("Audio: no decodable audio stream; playing video only");
        Close();
        return false;
    }
    // This reader only needs the audio stream: let the demuxer skip everything else.
    for (unsigned i = 0; i < m_fmtCtx->nb_streams; ++i)
        m_fmtCtx->streams[i]->discard = (static_cast<int>(i) == m_streamIndex) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

    AVStream* stream = m_fmtCtx->streams[m_streamIndex];
    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    m_codecCtx->pkt_timebase = stream->time_base;
    if ((ret = avcodec_open2(m_codecCtx, codec, nullptr)) < 0)
    {
        LogF("Audio: avcodec_open2 failed: %s", AvErr(ret).c_str());
        Close();
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_timeBase = av_q2d(stream->time_base);
    m_startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

    m_fifo.clear();
    m_fifoRead = 0;
    m_writtenPts = startPts;
    m_dropUntil = startPts;
    m_ptsSynced = false;
    m_hasWritten = false;
    m_eof = false;
    m_flushSent = false;
    m_drainedLogged = false;
    m_deviceFailed = false;
    m_quit = false;
    m_paused = startPaused;
    m_volume = volume;
    m_seekPending = false;
    m_appliedSeekGen = m_seekGen.load();
    PublishClock(startPts, false, false);

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    std::promise<bool> initPromise;
    std::future<bool> initResult = initPromise.get_future();
    m_thread = std::thread(&AudioPlayer::ThreadMain, this, std::move(initPromise), startPaused);
    if (!initResult.get())
    {
        Close(); // joins the thread (it already exited) and frees everything
        return false;
    }

    char desc[160];
    snprintf(desc, sizeof(desc), "%s %d Hz %d ch -> device %d Hz %d ch", codec->name, m_codecCtx->sample_rate,
             m_codecCtx->ch_layout.nb_channels, m_outRate, m_outChannels);
    m_description = desc;
    LogF("Audio: %s", m_description.c_str());
    return true;
}

void AudioPlayer::Close()
{
    if (m_thread.joinable())
    {
        m_quit = true;
        SetEvent(m_wakeEvent);
        m_thread.join();
    }
    if (m_wakeEvent)
    {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    swr_free(&m_swr);
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    avcodec_free_context(&m_codecCtx);
    avformat_close_input(&m_fmtCtx);
    m_streamIndex = -1;
    m_description.clear();
    PublishClock(0.0, false, false);
}

void AudioPlayer::SetPaused(bool paused)
{
    if (!IsOpen())
        return;
    m_paused = paused;
    SetEvent(m_wakeEvent);
}

void AudioPlayer::Seek(double pts)
{
    if (!IsOpen())
        return;
    {
        // Target and generation change together (the audio thread reads both under this
        // lock). Bumping the generation invalidates the clock immediately, so the UI never
        // sees a stale pre-seek position published before the audio thread handles the seek.
        std::lock_guard<std::mutex> lock(m_seekMutex);
        m_seekPending = true;
        m_seekTarget = pts;
        m_seekGen.fetch_add(1);
    }
    SetEvent(m_wakeEvent);
}

bool AudioPlayer::GetClock(double& seconds) const
{
    std::lock_guard<std::mutex> lock(m_clockMutex);
    if (!m_clockValid || m_clockGen != m_seekGen.load())
        return false;
    seconds = m_clockPts;
    if (m_clockRunning)
        seconds += std::min(NowSeconds() - m_clockStamp, kMaxClockExtrapolation);
    return true;
}

void AudioPlayer::PublishClock(double pts, bool valid, bool running)
{
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_clockPts = pts;
    m_clockStamp = NowSeconds();
    m_clockValid = valid;
    m_clockRunning = running;
    m_clockGen = m_appliedSeekGen;
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void AudioPlayer::ThreadMain(std::promise<bool> initResult, bool startPaused)
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = InitDevice();
    initResult.set_value(ok);
    if (!ok)
    {
        ShutdownDevice();
        if (SUCCEEDED(coHr))
            CoUninitialize();
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    Fill(); // pre-roll before starting
    if (!startPaused && SUCCEEDED(m_client->Start()))
        m_clientRunning = true;

    HANDLE handles[2] = {m_audioEvent, m_wakeEvent};
    while (!m_quit)
    {
        WaitForMultipleObjects(2, handles, FALSE, 50);
        if (m_quit)
            break;

        bool seek = false;
        double target = 0.0;
        uint32_t seekGen = 0;
        {
            std::lock_guard<std::mutex> lock(m_seekMutex);
            if (m_seekPending)
            {
                seek = true;
                target = m_seekTarget;
                seekGen = m_seekGen.load();
                m_seekPending = false;
            }
        }
        if (seek && !m_deviceFailed)
        {
            m_appliedSeekGen = seekGen;
            DoSeek(target);
        }
        if (m_deviceFailed)
            continue;

        const bool wantRunning = !m_paused.load();
        if (wantRunning != m_clientRunning)
        {
            if (wantRunning)
            {
                Fill(); // make sure there is data queued before starting
                m_clientRunning = SUCCEEDED(m_client->Start());
            }
            else
            {
                m_client->Stop();
                m_clientRunning = false;
            }
        }
        Fill();
    }

    ShutdownDevice();
    if (SUCCEEDED(coHr))
        CoUninitialize();
}

bool AudioPlayer::InitDevice()
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr))
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_client));
    SafeRelease(device);
    SafeRelease(enumerator);
    if (FAILED(hr))
    {
        LogF("Audio: no output device (0x%08lx); playing video only", static_cast<unsigned long>(hr));
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(hr = m_client->GetMixFormat(&mix)))
    {
        LogF("Audio: GetMixFormat failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    m_outRate = static_cast<int>(mix->nSamplesPerSec);
    m_outChannels = mix->nChannels;
    DWORD channelMask = 0;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        channelMask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    CoTaskMemFree(mix);

    // Always render 32-bit float at the device's own rate / channel count.
    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = static_cast<WORD>(m_outChannels);
    wfx.Format.nSamplesPerSec = static_cast<DWORD>(m_outRate);
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = static_cast<WORD>(m_outChannels * 4);
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = channelMask;
    wfx.SubFormat = kSubtypeIeeeFloat;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &wfx.Format, nullptr);
    if (FAILED(hr))
    {
        LogF("Audio: IAudioClient::Initialize failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    m_audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr = m_client->SetEventHandle(m_audioEvent)) || FAILED(hr = m_client->GetBufferSize(&m_bufferFrames)) ||
        FAILED(hr = m_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_render))))
    {
        LogF("Audio: WASAPI setup failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    // Resampler: decoder format -> interleaved float at the device rate and layout.
    AVChannelLayout outLayout{};
    if (channelMask == 0 || av_channel_layout_from_mask(&outLayout, channelMask) < 0 ||
        outLayout.nb_channels != m_outChannels)
    {
        av_channel_layout_uninit(&outLayout);
        av_channel_layout_default(&outLayout, m_outChannels);
    }
    AVChannelLayout inLayout{};
    if (m_codecCtx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&inLayout, m_codecCtx->ch_layout.nb_channels);
    else
        av_channel_layout_copy(&inLayout, &m_codecCtx->ch_layout);

    int ret = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, &inLayout, m_codecCtx->sample_fmt,
                                  m_codecCtx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    av_channel_layout_uninit(&inLayout);
    if (ret < 0 || (ret = swr_init(m_swr)) < 0)
    {
        LogF("Audio: resampler setup failed: %s", AvErr(ret).c_str());
        return false;
    }
    return true;
}

void AudioPlayer::ShutdownDevice()
{
    if (m_client && m_clientRunning)
        m_client->Stop();
    m_clientRunning = false;
    SafeRelease(m_render);
    SafeRelease(m_client);
    if (m_audioEvent)
    {
        CloseHandle(m_audioEvent);
        m_audioEvent = nullptr;
    }
}

void AudioPlayer::DoSeek(double pts)
{
    if (m_clientRunning)
    {
        m_client->Stop();
        m_clientRunning = false;
    }
    m_client->Reset(); // drop everything queued in the device buffer

    const int64_t ts = m_startTime + static_cast<int64_t>(pts / m_timeBase);
    const int ret = av_seek_frame(m_fmtCtx, m_streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
        LogF("Audio: seek failed: %s", AvErr(ret).c_str());
    avcodec_flush_buffers(m_codecCtx);
    swr_close(m_swr); // re-init clears the resampler's internal delay line
    swr_init(m_swr);

    m_fifo.clear();
    m_fifoRead = 0;
    m_writtenPts = pts;
    m_dropUntil = pts;
    m_ptsSynced = false;
    m_hasWritten = false;
    m_eof = false;
    m_flushSent = false;
    m_drainedLogged = false;
    PublishClock(pts, false, false);
    // The thread loop pre-fills and restarts the client if playback isn't paused.
}

bool AudioPlayer::DecodeMore()
{
    for (;;)
    {
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == 0)
        {
            const size_t fifoFrames = (m_fifo.size() - m_fifoRead) / static_cast<size_t>(m_outChannels);
            const int64_t ts = m_frame->best_effort_timestamp;
            if (!m_ptsSynced && ts != AV_NOPTS_VALUE && fifoFrames == 0)
            {
                m_writtenPts = static_cast<double>(ts - m_startTime) * m_timeBase;
                m_ptsSynced = true;
            }

            const int maxOut = swr_get_out_samples(m_swr, m_frame->nb_samples);
            const size_t needed = static_cast<size_t>(std::max(maxOut, 0)) * static_cast<size_t>(m_outChannels);
            if (m_convert.size() < needed)
                m_convert.resize(needed); // grows a few times at most, then reused
            uint8_t* out = reinterpret_cast<uint8_t*>(m_convert.data());
            const int got = swr_convert(m_swr, &out, maxOut, const_cast<const uint8_t**>(m_frame->extended_data),
                                        m_frame->nb_samples);
            av_frame_unref(m_frame);
            if (got <= 0)
                continue;

            // After open/seek, drop samples that come before the requested position.
            size_t drop = 0;
            if (m_dropUntil >= 0.0)
            {
                if (fifoFrames == 0)
                {
                    const double behind = (m_dropUntil - m_writtenPts) * m_outRate;
                    drop = behind > 0.0 ? std::min(static_cast<size_t>(std::llround(behind)), static_cast<size_t>(got)) : 0;
                    m_writtenPts += static_cast<double>(drop) / m_outRate;
                }
                if (drop < static_cast<size_t>(got))
                    m_dropUntil = -1.0;
            }

            const size_t ch = static_cast<size_t>(m_outChannels);
            m_fifo.insert(m_fifo.end(), m_convert.begin() + static_cast<ptrdiff_t>(drop * ch),
                          m_convert.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(got) * ch));
            return true;
        }
        if (ret == AVERROR_EOF)
        {
            m_eof = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN))
        {
            LogF("Audio: decode error: %s", AvErr(ret).c_str());
            m_eof = true;
            return false;
        }
        if (m_flushSent)
        {
            m_eof = true;
            return false;
        }

        ret = av_read_frame(m_fmtCtx, m_packet);
        if (ret < 0)
        {
            avcodec_send_packet(m_codecCtx, nullptr); // drain
            m_flushSent = true;
            continue;
        }
        if (m_packet->stream_index == m_streamIndex)
            avcodec_send_packet(m_codecCtx, m_packet); // bad packets are skipped
        av_packet_unref(m_packet);
    }
}

void AudioPlayer::Fill()
{
    UINT32 padding = 0;
    HRESULT hr = m_client->GetCurrentPadding(&padding);
    if (FAILED(hr))
    {
        m_deviceFailed = true;
        LogF("Audio: output device lost or changed (0x%08lx); continuing without audio",
             static_cast<unsigned long>(hr));
        PublishClock(m_writtenPts, false, false);
        return;
    }

    const size_t ch = static_cast<size_t>(m_outChannels);
    const UINT32 freeFrames = m_bufferFrames - padding;
    size_t fifoFrames = (m_fifo.size() - m_fifoRead) / ch;
    while (fifoFrames < freeFrames && !m_eof && DecodeMore())
        fifoFrames = (m_fifo.size() - m_fifoRead) / ch;

    const UINT32 n = static_cast<UINT32>(std::min<size_t>(freeFrames, fifoFrames));
    if (n > 0)
    {
        BYTE* data = nullptr;
        if (FAILED(hr = m_render->GetBuffer(n, &data)))
        {
            m_deviceFailed = true;
            LogF("Audio: GetBuffer failed (0x%08lx); continuing without audio", static_cast<unsigned long>(hr));
            PublishClock(m_writtenPts, false, false);
            return;
        }
        const float volume = m_volume.load();
        const float* src = m_fifo.data() + m_fifoRead;
        float* dst = reinterpret_cast<float*>(data);
        const size_t count = static_cast<size_t>(n) * ch;
        for (size_t i = 0; i < count; ++i)
            dst[i] = src[i] * volume;
        m_render->ReleaseBuffer(n, 0);

        m_fifoRead += count;
        if (m_fifoRead == m_fifo.size())
        {
            m_fifo.clear(); // keeps capacity
            m_fifoRead = 0;
        }
        else if (m_fifoRead > 65536 && m_fifoRead * 2 > m_fifo.size())
        {
            m_fifo.erase(m_fifo.begin(), m_fifo.begin() + static_cast<ptrdiff_t>(m_fifoRead));
            m_fifoRead = 0;
        }
        m_writtenPts += static_cast<double>(n) / m_outRate;
        m_hasWritten = true;
    }

    // Audible position = next sample to write minus everything still queued in the device.
    const UINT32 queued = padding + n;
    const bool drained = m_eof && m_fifo.size() == m_fifoRead && queued == 0;
    if (drained && !m_drainedLogged)
    {
        LogF("Audio: end of audio stream");
        m_drainedLogged = true;
    }
    PublishClock(m_writtenPts - static_cast<double>(queued) / m_outRate, m_hasWritten && !drained, m_clientRunning);
}

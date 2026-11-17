#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;
struct IAudioClient;
struct IAudioRenderClient;

// Plays a file's audio track through WASAPI (shared mode, default output device) and
// provides the master playback clock.
//
// Threading: Open/Close/SetPaused/Seek/SetVolume/GetClock are called from the UI thread.
// A dedicated audio thread owns its own demuxer (a second reader on the same file),
// decoder, resampler and the WASAPI client; the UI thread only exchanges commands and
// the clock with it through atomics / a small mutex.
class AudioPlayer
{
public:
    ~AudioPlayer();

    // Returns false (and logs why) if the file has no usable audio or the device fails;
    // the player then runs video-only on its own clock. Samples before startPts are dropped.
    // streamIndex: FFmpeg stream to play, or -1 for the file's default audio stream.
    bool Open(const std::wstring& path, double startPts, bool startPaused, float volume, int streamIndex = -1);
    int StreamIndex() const { return m_streamIndex; }

    // True once the output device was lost/changed mid-playback (the app then reopens audio).
    bool DeviceFailed() const { return m_deviceFailedFlag.load(); }
    void Close();
    bool IsOpen() const { return m_thread.joinable(); }

    void SetPaused(bool paused);
    void Seek(double pts); // asynchronous; the clock is invalid until audio resumes
    void SetVolume(float volume) { m_volume.store(volume); }

    // Current audible position in seconds. Returns false when audio can't drive the clock
    // (not started yet, after a seek, drained at end of stream, or device lost).
    bool GetClock(double& seconds) const;

    const std::string& Description() const { return m_description; }

private:
    void ThreadMain(std::promise<bool> initResult, bool startPaused);
    bool InitDevice();
    void ShutdownDevice();
    void Fill();
    bool DecodeMore(); // appends to the FIFO; false at end of stream
    void DoSeek(double pts);
    void PublishClock(double pts, bool valid, bool running);

    // --- Owned by the audio thread after Open() returns ---
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    SwrContext* m_swr = nullptr;
    int m_streamIndex = -1;
    double m_timeBase = 0.0; // seconds per stream tick
    int64_t m_startTime = 0;

    IAudioClient* m_client = nullptr;
    IAudioRenderClient* m_render = nullptr;
    HANDLE m_audioEvent = nullptr;
    UINT32 m_bufferFrames = 0;
    int m_outRate = 0;
    int m_outChannels = 0;
    bool m_clientRunning = false;
    bool m_deviceFailed = false;

    std::vector<float> m_fifo; // interleaved float samples waiting for the device
    size_t m_fifoRead = 0;     // read offset into m_fifo, in floats
    std::vector<float> m_convert;
    double m_writtenPts = 0.0; // pts of the next sample handed to the device
    double m_dropUntil = -1.0; // drop decoded samples earlier than this (after open/seek)
    bool m_ptsSynced = false;
    bool m_hasWritten = false;
    bool m_eof = false;
    bool m_flushSent = false;
    bool m_drainedLogged = false;

    // --- Shared between threads ---
    std::thread m_thread;
    HANDLE m_wakeEvent = nullptr; // auto-reset: new command for the audio thread
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_deviceFailedFlag{false};
    std::atomic<bool> m_paused{false};
    std::atomic<float> m_volume{1.0f};
    std::mutex m_seekMutex;
    bool m_seekPending = false;
    double m_seekTarget = 0.0;
    std::atomic<uint32_t> m_seekGen{0}; // bumped by Seek() on the UI thread
    uint32_t m_appliedSeekGen = 0;      // last generation the audio thread has applied

    mutable std::mutex m_clockMutex;
    double m_clockPts = 0.0;
    double m_clockStamp = 0.0; // QPC seconds when m_clockPts was measured
    bool m_clockValid = false;
    bool m_clockRunning = false;
    uint32_t m_clockGen = 0; // seek generation the published clock belongs to

    std::string m_description;
};

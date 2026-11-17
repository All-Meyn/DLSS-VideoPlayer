#include "App.h"

#include "Log.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace
{
constexpr wchar_t kWindowClass[] = L"DLSSVideoPlayerWindow";
constexpr wchar_t kAppName[] = L"DLSS-VideoPlayer";
constexpr double kSeekStep = 5.0;
constexpr double kSeekStepLarge = 30.0;
constexpr int kMaxCatchUpFrames = 8;
constexpr int kMaxSeekDecodeFrames = 1000;
constexpr double kTitleUpdateInterval = 0.25;
constexpr double kResyncThreshold = 1.0; // video this far behind the audio clock: skip ahead
constexpr float kVolumeStep = 0.05f;

void FormatTime(double s, wchar_t* out, size_t n)
{
    if (s < 0)
        s = 0;
    const int total = static_cast<int>(s);
    swprintf(out, n, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}
} // namespace

int App::Run(HINSTANCE instance, int showCmd, const Options& options)
{
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    m_qpcToSeconds = 1.0 / static_cast<double>(freq.QuadPart);

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd)
    {
        wchar_t msg[128];
        swprintf(msg, ARRAYSIZE(msg), L"Could not create the main window (error %lu).", GetLastError());
        MessageBoxW(nullptr, msg, kAppName, MB_ICONERROR);
        return 1;
    }

    if (!m_gpu.Init(m_hwnd, options.d3dDebug) || !m_renderer.Init(&m_gpu, !options.noSdk))
    {
        MessageBoxW(m_hwnd, L"Failed to initialize Direct3D 11 with video support.", kAppName, MB_ICONERROR);
        return 1;
    }
    m_initialized = true;
    if (m_renderer.HasRtxVideoSdk())
        m_mode = UpscaleMode::RtxVideoSdk; // prefer the SDK when it initialized

    DragAcceptFiles(m_hwnd, TRUE);
    ShowWindow(m_hwnd, showCmd);

    if (!m_gpu.IsNvidia())
        LogF("Warning: no NVIDIA GPU selected; RTX Video Super Resolution will be unavailable.");
    if (!options.initialFile.empty())
        OpenFile(options.initialFile);
    UpdateTitle(true);

    bool running = true;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        if (m_minimized || m_deviceLost)
        {
            UpdateKeepAwake(); // releases it after a device loss
            WaitMessage();
            continue;
        }

        const bool playing = m_hasVideo && !m_paused;
        const bool frameChanged = playing && AdvancePlayback();

        if (frameChanged || m_dirty)
            RenderNow(); // re-process (upscale) + present
        else if (playing)
            m_renderer.Present(); // same frame, keeps vsync pacing
        else
            WaitMessage(); // paused / idle: sleep until input

        if (HandleDeviceLost())
            continue;
        UpdateKeepAwake();
        UpdateTitle(false);
    }

    CloseVideo();
    UpdateKeepAwake(); // releases the request (no video any more)
    return 0;
}

// ---------------------------------------------------------------------------
// Window messages
// ---------------------------------------------------------------------------

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* creating = static_cast<App*>(cs->lpCreateParams);
        // Store the handle now: CreateWindowExW hasn't returned yet, and HandleMessage
        // passes m_hwnd to DefWindowProcW (a null hwnd there makes window creation fail).
        creating->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creating));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app)
        return app->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        m_minimized = (wp == SIZE_MINIMIZED);
        if (m_initialized && !m_minimized)
        {
            if (m_gpu.Resize(LOWORD(lp), HIWORD(lp)))
                m_renderer.OnResize();
            // Render immediately so live-resizing (modal size loop) stays responsive.
            RenderNow();
        }
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        EndPaint(m_hwnd, &ps);
        m_dirty = true;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;

    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN))
        {
            ToggleFullscreen();
            return 0;
        }
        break;

    case WM_LBUTTONDBLCLK:
        ToggleFullscreen();
        return 0;

    case WM_DROPFILES:
    {
        auto drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[MAX_PATH * 4] = {};
        if (DragQueryFileW(drop, 0, path, ARRAYSIZE(path)))
            OpenFile(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

void App::OnKeyDown(WPARAM key)
{
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (key)
    {
    case VK_SPACE: TogglePause(); break;
    case VK_LEFT: SeekBy(shift ? -kSeekStepLarge : -kSeekStep); break;
    case VK_RIGHT: SeekBy(shift ? kSeekStepLarge : kSeekStep); break;
    case 'U': CycleUpscaleMode(); break;
    case '1':
    case '2':
    case '3':
    case '4':
        m_quality = static_cast<int>(key - '0');
        m_dirty = true;
        break;
    case 'F': ToggleFullscreen(); break;
    case VK_UP: ChangeVolume(kVolumeStep); break;
    case VK_DOWN: ChangeVolume(-kVolumeStep); break;
    case 'M':
        m_muted = !m_muted;
        ApplyVolume();
        break;
    case VK_ESCAPE:
        if (m_fullscreen)
            ToggleFullscreen();
        break;
    case 'O': ShowOpenDialog(); break;
    default: return;
    }
    UpdateTitle(true);
}

// ---------------------------------------------------------------------------
// File handling
// ---------------------------------------------------------------------------

void App::ShowOpenDialog()
{
    wchar_t path[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{sizeof(OPENFILENAMEW)};
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Video files\0*.mp4;*.mkv;*.mov;*.webm;*.avi;*.ts;*.m2ts;*.wmv\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        OpenFile(path);
}

void App::CloseVideo()
{
    // Release frames before the decoder so surfaces return to their pool first.
    m_audio.Close();
    m_hasAudio = false;
    m_current = VideoFrame{};
    m_next = VideoFrame{};
    m_decoder.Close();
    m_hasVideo = false;
    m_eof = false;
}

void App::OpenFile(const std::wstring& path)
{
    CloseVideo();

    std::string error;
    if (!m_decoder.Open(path, m_gpu.Device(), error))
    {
        std::wstring msg = L"Could not play this file:\n\n";
        msg.append(error.begin(), error.end());
        MessageBoxW(m_hwnd, msg.c_str(), kAppName, MB_ICONERROR);
        m_dirty = true;
        UpdateTitle(true);
        return;
    }

    const size_t slash = path.find_last_of(L"\\/");
    m_fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);

    DecodeNext();
    if (!m_next)
    {
        MessageBoxW(m_hwnd, L"The file opened but no frames could be decoded on the GPU.", kAppName, MB_ICONERROR);
        CloseVideo();
        UpdateTitle(true);
        return;
    }
    m_current = std::move(m_next);
    DecodeNext();

    m_hasVideo = true;
    m_paused = false;
    m_loggedMode = -1; // log the upscaler state again for the new file
    m_hasAudio = m_audio.Open(path, m_current.pts, false, m_muted ? 0.0f : m_volume);
    AnchorClock(m_current.pts);
    m_dirty = true;
    UpdateTitle(true);
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

double App::Now() const
{
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * m_qpcToSeconds;
}

double App::MediaTime()
{
    if (m_paused)
        return m_anchorMedia;
    // Audio is the master clock. While it can't provide one (just after open/seek, after the
    // audio stream ended, or device lost) the QPC clock continues from the last audio time.
    double audioClock = 0.0;
    if (m_hasAudio && m_audio.GetClock(audioClock))
    {
        AnchorClock(audioClock);
        return audioClock;
    }
    return m_anchorMedia + (Now() - m_anchorTime);
}

void App::AnchorClock(double mediaTime)
{
    m_anchorMedia = mediaTime;
    m_anchorTime = Now();
}

void App::DecodeNext()
{
    m_next = VideoFrame{};
    if (m_eof)
        return;
    switch (m_decoder.Decode(m_next))
    {
    case VideoDecoder::Result::Frame: break;
    case VideoDecoder::Result::EndOfStream: m_eof = true; break;
    case VideoDecoder::Result::Error:
        LogF("Decode error; stopping playback");
        m_eof = true;
        break;
    }
}

bool App::AdvancePlayback()
{
    const double t = MediaTime();
    bool changed = false;
    int steps = 0;
    while (m_next && m_next.pts <= t && steps < kMaxCatchUpFrames)
    {
        m_current = std::move(m_next);
        DecodeNext();
        changed = true;
        ++steps;
    }

    if (m_next && m_next.pts <= t && steps == kMaxCatchUpFrames)
    {
        if (!m_hasAudio)
        {
            // No audio: resync the clock to the video rather than spiral.
            AnchorClock(m_current.pts);
        }
        else if (t - m_next.pts > kResyncThreshold)
        {
            // The audio clock can't wait for video (e.g. after the window was minimized):
            // skip the video ahead to the audio position instead of decoding every frame.
            LogF("Video %.2fs behind audio; skipping ahead to %.2fs", t - m_next.pts, t);
            if (!SeekVideo(t))
                return true;
            changed = true;
        }
    }

    if (!m_next && m_eof)
    {
        // End of stream: hold the last frame.
        m_anchorMedia = m_current.pts;
        m_paused = true;
        m_audio.SetPaused(true);
        UpdateTitle(true);
    }
    return changed;
}

void App::TogglePause()
{
    if (!m_hasVideo)
        return;
    if (m_paused)
    {
        if (m_eof && !m_next)
        {
            SeekTo(0.0); // at the end: replay from the start
            if (!m_hasVideo)
                return;
        }
        m_paused = false;
        AnchorClock(m_anchorMedia);
        m_audio.SetPaused(false);
    }
    else
    {
        m_anchorMedia = MediaTime();
        m_paused = true;
        m_audio.SetPaused(true);
    }
}

void App::SeekBy(double delta)
{
    if (m_hasVideo)
        SeekTo(MediaTime() + delta);
}

void App::SeekTo(double target)
{
    if (!m_hasVideo)
        return;
    const double duration = m_decoder.Duration();
    target = std::max(0.0, duration > 0.0 ? std::min(target, duration - 0.1) : target);
    if (!SeekVideo(target))
        return;

    // Seeking keeps the current paused/playing state. Audio restarts exactly where the
    // video landed (the first frame at or after the target).
    AnchorClock(m_current.pts);
    if (m_hasAudio)
        m_audio.Seek(m_current.pts);
}

bool App::SeekVideo(double target)
{
    m_current = VideoFrame{};
    m_next = VideoFrame{};
    m_eof = false;
    if (!m_decoder.Seek(target))
    {
        m_eof = true;
        return false;
    }

    // Decode forward from the keyframe until we reach the target.
    for (int i = 0; i < kMaxSeekDecodeFrames; ++i)
    {
        DecodeNext();
        if (!m_next || m_next.pts >= target - 0.001)
            break;
        m_current = std::move(m_next);
    }
    if (m_next)
    {
        m_current = std::move(m_next);
        DecodeNext();
    }
    if (!m_current)
    {
        CloseVideo();
        m_dirty = true;
        return false;
    }
    m_dirty = true;
    return true;
}

void App::CycleUpscaleMode()
{
    // With the SDK active the cycle is Off <-> RTX Video SDK: driver VSR is left out
    // because it crashes the device when used alongside the SDK (see Renderer.cpp).
    // Without the SDK (not built, failed to start, or --no-sdk): Off <-> driver VSR.
    if (m_mode != UpscaleMode::Off)
        m_mode = UpscaleMode::Off;
    else
        m_mode = m_renderer.HasRtxVideoSdk() ? UpscaleMode::RtxVideoSdk : UpscaleMode::DriverVsr;
    m_dirty = true;
}

// ---------------------------------------------------------------------------
// Rendering & UI
// ---------------------------------------------------------------------------

void App::RenderNow()
{
    if (!m_initialized || m_deviceLost)
        return;
    if (m_hasVideo && m_current)
    {
        if (!m_renderer.RenderFrame(m_current, m_decoder.SampleAspectRatio(), m_mode, m_quality, m_effectiveMode))
            m_renderer.RenderBlack();
        LogUpscalerStateIfChanged();
    }
    else
    {
        m_renderer.RenderBlack();
    }
    m_renderer.Present();
    m_dirty = false;
}

void App::ChangeVolume(float delta)
{
    m_volume = std::clamp(m_volume + delta, 0.0f, 1.0f);
    m_muted = false;
    ApplyVolume();
}

void App::ApplyVolume()
{
    m_audio.SetVolume(m_muted ? 0.0f : m_volume);
}

// While a video is playing, keep the display on and the PC awake; release when paused,
// stopped, at the end of the file or after a device loss. Only calls Windows on a change.
void App::UpdateKeepAwake()
{
    const bool want = m_hasVideo && !m_paused && !m_deviceLost;
    if (want == m_keepAwake)
        return;
    m_keepAwake = want;
    SetThreadExecutionState(want ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

// Stops rendering (instead of failing every frame) once the driver has removed the device.
// Recovering would mean recreating the device, decoder and upscalers; for now: restart.
bool App::HandleDeviceLost()
{
    if (m_deviceLost || !m_gpu.CheckDeviceRemoved())
        return m_deviceLost;
    m_deviceLost = true;
    m_paused = true;
    m_audio.SetPaused(true);
    LogF("Rendering stopped after device removal (last upscaler: requested %s, using %s)", UpscaleModeName(m_mode),
         UpscaleModeName(m_effectiveMode));
    MessageBoxW(m_hwnd,
                L"The GPU driver reset the graphics device, so playback has stopped.\n\n"
                L"Details are in %TEMP%\\DLSSVideoPlayer\\DLSSVideoPlayer.log. Please restart the player.",
                kAppName, MB_ICONERROR);
    return true;
}

// Logs only when the requested mode, the mode actually used, or the quality changes,
// so it is cheap to call every rendered frame.
void App::LogUpscalerStateIfChanged()
{
    const int mode = static_cast<int>(m_mode);
    const int effective = static_cast<int>(m_effectiveMode);
    if (mode == m_loggedMode && effective == m_loggedEffectiveMode && m_quality == m_loggedQuality)
        return;
    m_loggedMode = mode;
    m_loggedEffectiveMode = effective;
    m_loggedQuality = m_quality;

    const RECT d = m_renderer.LastDestRect();
    LogF("Upscaler: requested %s, using %s, quality %d, %dx%d -> %ldx%ld", UpscaleModeName(m_mode),
         UpscaleModeName(m_effectiveMode), m_quality, m_decoder.Width(), m_decoder.Height(), d.right - d.left,
         d.bottom - d.top);
}

void App::ToggleFullscreen()
{
    const DWORD style = static_cast<DWORD>(GetWindowLongW(m_hwnd, GWL_STYLE));
    if (!m_fullscreen)
    {
        MONITORINFO mi{sizeof(MONITORINFO)};
        if (GetWindowPlacement(m_hwnd, &m_windowedPlacement) &&
            GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLongW(m_hwnd, GWL_STYLE, static_cast<LONG>(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            m_fullscreen = true;
        }
    }
    else
    {
        SetWindowLongW(m_hwnd, GWL_STYLE, static_cast<LONG>(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(m_hwnd, &m_windowedPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_fullscreen = false;
    }
    m_dirty = true;
}

void App::UpdateTitle(bool force)
{
    const double now = Now();
    if (!force && now - m_lastTitleUpdate < kTitleUpdateInterval)
        return;
    m_lastTitleUpdate = now;

    wchar_t title[512];
    if (!m_hasVideo)
    {
        swprintf(title, ARRAYSIZE(title), L"%ls  -  drop a video here or press O to open", kAppName);
    }
    else
    {
        const RECT d = m_renderer.LastDestRect();
        wchar_t pos[32], dur[32];
        FormatTime(MediaTime(), pos, ARRAYSIZE(pos));
        FormatTime(m_decoder.Duration(), dur, ARRAYSIZE(dur));

        wchar_t modeText[96];
        if (m_effectiveMode == m_mode)
            swprintf(modeText, ARRAYSIZE(modeText), L"%hs", UpscaleModeName(m_mode));
        else
            swprintf(modeText, ARRAYSIZE(modeText), L"%hs -> %hs", UpscaleModeName(m_mode),
                     UpscaleModeName(m_effectiveMode));

        wchar_t audioText[32];
        if (!m_hasAudio)
            swprintf(audioText, ARRAYSIZE(audioText), L"No audio");
        else if (m_muted)
            swprintf(audioText, ARRAYSIZE(audioText), L"Muted");
        else
            swprintf(audioText, ARRAYSIZE(audioText), L"Vol %d%%", static_cast<int>(std::lround(m_volume * 100.0f)));

        swprintf(title, ARRAYSIZE(title),
                 L"%ls  |  %dx%d -> %ldx%ld  |  Upscale: %ls  Q%d  |  %ls  |  %ls / %ls%ls  -  %ls", m_fileName.c_str(),
                 m_decoder.Width(), m_decoder.Height(), d.right - d.left, d.bottom - d.top, modeText, m_quality,
                 audioText, pos, dur, m_paused ? L"  [Paused]" : L"", kAppName);
    }
    SetWindowTextW(m_hwnd, title);
}

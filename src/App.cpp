#include "App.h"

#include "Log.h"

#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
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
constexpr double kBarHideDelay = 2.5; // seconds without mouse movement before the bar hides
constexpr double kBarFadeTime = 0.15;
constexpr double kOsdDuration = 1.2;
constexpr double kOsdFadeTime = 0.3;
constexpr UINT kMsgAudioDeviceChanged = WM_APP + 1;
constexpr UINT_PTR kTimerAudioReopen = 1;
constexpr UINT kAudioReopenDelayMs = 400; // let device changes settle (they come in bursts)
constexpr double kMinSecondsBetweenRecoveries = 10.0;

const wchar_t* QualityName(int q)
{
    switch (q)
    {
    case 1: return L"Low";
    case 2: return L"Medium";
    case 3: return L"High";
    default: return L"Ultra";
    }
}

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

    m_options = options;
    if (!m_gpu.Init(m_hwnd, options.d3dDebug) || !m_renderer.Init(&m_gpu, !options.noSdk))
    {
        MessageBoxW(m_hwnd, L"Failed to initialize Direct3D 11 with video support.", kAppName, MB_ICONERROR);
        return 1;
    }
    m_initialized = true;
    m_settings.Load();
    m_overlay.Init(&m_gpu, m_hwnd); // optional: without it the player is keyboard-only
    if (!m_outputPass.Init(&m_gpu))
        LogF("HDR output unavailable (shader setup failed); HDR video will be shown as SDR");
    if (m_renderer.HasRtxVideoSdk())
        m_mode = UpscaleMode::RtxVideoSdk; // prefer the SDK when it initialized

    DragAcceptFiles(m_hwnd, TRUE);
    m_audioWatcher.Start(m_hwnd, kMsgAudioDeviceChanged);
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
        UpdateOverlay();

        if (frameChanged || m_dirty)
            RenderNow(); // re-process (upscale) + present
        else if (playing || m_overlayDirty || OverlayAnimating())
            PresentFrame(); // same video frame (vsync pacing / overlay update)
        else
            WaitMessage(); // paused / idle: sleep until input

        if (HandleDeviceLost())
            continue;
        if (m_hasAudio && !m_audioReopenPending && m_audio.DeviceFailed())
        {
            m_audioReopenPending = true;
            SetTimer(m_hwnd, kTimerAudioReopen, kAudioReopenDelayMs, nullptr);
        }
        UpdateKeepAwake();
        UpdateTitle(false);
    }

    CloseVideo();
    UpdateKeepAwake(); // releases the request (no video any more)
    m_settings.Save();
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
            // Both hold views of the back buffer, which ResizeBuffers needs released.
            m_overlay.ReleaseTarget();
            m_outputPass.ReleaseTarget();
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

    case WM_EXITSIZEMOVE:   // window dragged (possibly to another monitor)
    case WM_DISPLAYCHANGE:  // resolution / HDR toggled / monitor added or removed
        UpdateOutputMode();
        return 0;

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
        // Double-click on the video toggles fullscreen; on the control bar it's just a click.
        if (m_overlay.HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == OverlayPart::None)
            ToggleFullscreen();
        else
            OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        m_mouseInside = false;
        m_hover = OverlayPart::None;
        if (!m_draggingSeek)
            m_seekPreview = -1.0;
        m_overlayDirty = true;
        return 0;

    case WM_LBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
        OnMouseUp();
        return 0;

    case WM_CAPTURECHANGED:
        // Capture taken away (e.g. Alt+Tab) mid-drag: cancel without seeking.
        if (reinterpret_cast<HWND>(lp) != m_hwnd && (m_draggingSeek || m_draggingVolume || m_draggingTune >= 0))
        {
            m_draggingSeek = false;
            m_draggingVolume = false;
            m_draggingTune = -1;
            m_seekPreview = -1.0;
            m_overlayDirty = true;
        }
        return 0;

    case WM_MOUSEWHEEL:
        ChangeVolume(kVolumeStep * static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && m_cursorHidden)
        {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
    {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        m_overlay.OnDpiChanged();
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        m_overlayDirty = true;
        return 0;
    }

    case WM_DROPFILES:
    {
        auto drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[MAX_PATH * 4] = {};
        if (DragQueryFileW(drop, 0, path, ARRAYSIZE(path)))
            OpenFile(path);
        DragFinish(drop);
        return 0;
    }

    case kMsgAudioDeviceChanged:
        // Default output changed: reopen audio there once the burst of notifications settles.
        m_audioReopenPending = true;
        SetTimer(m_hwnd, kTimerAudioReopen, kAudioReopenDelayMs, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == kTimerAudioReopen)
        {
            KillTimer(m_hwnd, kTimerAudioReopen);
            m_audioReopenPending = false;
            ReopenAudio("audio output changed");
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

void App::OnKeyDown(WPARAM key)
{
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (m_tuneOpen)
    {
        // While the tuning panel is open the arrows edit it (Shift = fine steps), Esc/T close it.
        const unsigned values[3] = {m_settings.thdrContrast, m_settings.thdrSaturation, m_settings.thdrMiddleGray};
        const int step = shift ? 1 : 5;
        switch (key)
        {
        case VK_UP: m_tuneRow = (m_tuneRow + 2) % 3; m_overlayDirty = true; return;
        case VK_DOWN: m_tuneRow = (m_tuneRow + 1) % 3; m_overlayDirty = true; return;
        case VK_LEFT:
            SetTuneValue(m_tuneRow, static_cast<unsigned>(std::max(0, static_cast<int>(values[m_tuneRow]) - step)));
            return;
        case VK_RIGHT: SetTuneValue(m_tuneRow, values[m_tuneRow] + static_cast<unsigned>(step)); return;
        case VK_ESCAPE:
        case 'T': ToggleTunePanel(); return;
        default: break;
        }
    }
    switch (key)
    {
    case VK_SPACE: TogglePause(); break;
    case VK_LEFT: SeekBy(shift ? -kSeekStepLarge : -kSeekStep); break;
    case VK_RIGHT: SeekBy(shift ? kSeekStepLarge : kSeekStep); break;
    case 'U': CycleUpscaleMode(); break;
    case '1':
    case '2':
    case '3':
    case '4': SetQuality(static_cast<int>(key - '0')); break;
    case 'F': ToggleFullscreen(); break;
    case VK_UP: ChangeVolume(kVolumeStep); break;
    case VK_DOWN: ChangeVolume(-kVolumeStep); break;
    case 'M': ToggleMute(); break;
    case 'H': ToggleTrueHdr(); break;
    case 'T': ToggleTunePanel(); break;
    case 'A':
        if (!m_audioMenu.empty())
            SelectAudioTrack((m_audioChoice + 1) % static_cast<int>(m_audioMenu.size()));
        break;
    case 'S':
        if (m_subMenu.size() > 1)
            SelectSubtitleTrack((m_subChoice + 1) % static_cast<int>(m_subMenu.size()));
        break;
    case VK_ESCAPE:
        if (m_openMenu != OverlayPart::None)
        {
            m_openMenu = OverlayPart::None;
            m_overlayDirty = true;
        }
        else if (m_fullscreen)
        {
            ToggleFullscreen();
        }
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
    m_externalSubs.clear();
    m_audioMenu.clear();
    m_subMenu.clear();
    m_audioChoice = -1;
    m_subChoice = 0;
    m_openMenu = OverlayPart::None;
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
        UpdateOutputMode();
        UpdateTitle(true);
        return;
    }

    const size_t slash = path.find_last_of(L"\\/");
    m_fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    m_filePath = path;
    m_externalSubs = LoadExternalSubtitles(path);
    BuildTrackMenus(); // before decoding, so a default subtitle track sees packets from the start

    DecodeNext();
    if (!m_next)
    {
        MessageBoxW(m_hwnd, L"The file opened but no frames could be decoded on the GPU.", kAppName, MB_ICONERROR);
        CloseVideo();
        UpdateOutputMode();
        UpdateTitle(true);
        return;
    }
    m_current = std::move(m_next);
    DecodeNext();

    m_hasVideo = true;
    m_paused = false;
    m_loggedMode = -1; // log the upscaler state again for the new file
    UpdateOutputMode(); // HDR10 file on an HDR display -> HDR10 output
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
    ShowOsd(m_paused ? L"Paused" : L"Playing");
}

void App::SeekBy(double delta)
{
    if (m_hasVideo)
    {
        SeekTo(MediaTime() + delta);
        ShowSeekOsd();
    }
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
    ShowOsd(L"Upscaler: %hs", UpscaleModeName(m_mode));
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
    PresentFrame();
    m_dirty = false;
}

void App::ChangeVolume(float delta)
{
    SetVolume(m_volume + delta);
}

void App::SetVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    m_muted = false;
    ApplyVolume();
    if (m_hasAudio)
        ShowOsd(L"Volume %d%%", static_cast<int>(std::lround(m_volume * 100.0f)));
    else
        ShowOsd(L"No audio track");
}

void App::ToggleMute()
{
    m_muted = !m_muted;
    ApplyVolume();
    if (m_muted)
        ShowOsd(L"Muted");
    else
        ShowOsd(L"Volume %d%%", static_cast<int>(std::lround(m_volume * 100.0f)));
}

void App::SetQuality(int quality)
{
    m_quality = std::clamp(quality, 1, 4);
    m_dirty = true;
    ShowOsd(L"VSR quality %d (%ls)", m_quality, QualityName(m_quality));
}

void App::ToggleTrueHdr()
{
    m_trueHdrEnabled = !m_trueHdrEnabled;
    UpdateOutputMode();
    if (!m_trueHdrEnabled)
        ShowOsd(L"RTX Video HDR: off");
    else if (m_trueHdrOutput)
        ShowOsd(L"RTX Video HDR: on");
    else if (!m_renderer.SupportsTrueHdr())
        ShowOsd(L"RTX Video HDR: not available");
    else if (!m_display.hdr)
        ShowOsd(L"RTX Video HDR: turn on Windows HDR");
    else
        ShowOsd(L"RTX Video HDR: on (applies to SDR videos)");
}

void App::ToggleTunePanel()
{
    if (!m_tuneOpen && !m_renderer.SupportsTrueHdr())
    {
        ShowOsd(L"RTX Video HDR not available");
        return;
    }
    m_tuneOpen = !m_tuneOpen;
    if (!m_tuneOpen)
        m_settings.Save(); // persist when the panel closes (and on exit)
    m_lastMouseMove = Now();
    m_overlayDirty = true;
}

void App::SetTuneValue(int row, unsigned value)
{
    switch (row)
    {
    case 0: m_settings.thdrContrast = std::min(value, 200u); break;
    case 1: m_settings.thdrSaturation = std::min(value, 200u); break;
    default: m_settings.thdrMiddleGray = std::clamp(value, 10u, 100u); break;
    }
    m_tuneRow = std::clamp(row, 0, 2);
    ApplyTrueHdrSettings();
    if (m_trueHdrOutput)
        m_dirty = true; // re-process the current frame with the new settings
    m_overlayDirty = true;
}

void App::ApplyTrueHdrSettings()
{
    IUpscaler::SdrToHdrSettings s;
    s.contrast = m_settings.thdrContrast;
    s.saturation = m_settings.thdrSaturation;
    s.middleGray = m_settings.thdrMiddleGray;
    s.maxLuminance = m_thdrMaxLuminance;
    m_renderer.SetSdrToHdrSettings(s);
}

void App::ApplyVolume()
{
    m_audio.SetVolume(m_muted ? 0.0f : m_volume);
}

// ---------------------------------------------------------------------------
// On-screen controls
// ---------------------------------------------------------------------------

void App::PresentFrame()
{
    if (m_outputMode != OutputMode::Sdr8)
    {
        // 10-bit / HDR10: the overlay is drawn offscreen and blended in by the output shader.
        const bool overlayDrawn = m_overlay.IsAvailable() && m_overlay.Draw(BuildOverlayState());
        OutputPass::Params params;
        params.mode = m_outputMode == OutputMode::Hdr10 ? OutputPass::Mode::Hdr10
                      : m_toneMap                       ? OutputPass::Mode::ToneMap
                                                        : OutputPass::Mode::Sdr;
        params.sdrWhiteNits = m_display.sdrWhiteNits;
        params.srcPeakNits = m_contentPeakNits;
        m_outputPass.Run(m_renderer.PresentSrv(), overlayDrawn ? m_overlay.OffscreenSrv() : nullptr, params);
    }
    else
    {
        m_renderer.CopyToBackBuffer();
        if (m_overlay.IsAvailable())
            m_overlay.Draw(BuildOverlayState());
    }
    m_gpu.Present();
    m_overlayDirty = false;
}

void App::UpdateOutputMode()
{
    if (!m_initialized || m_deviceLost)
        return;
    m_display = QueryDisplayInfo(m_hwnd);

    bool hdrContent = false;
    bool tenBit = false;
    if (m_hasVideo && m_current)
    {
        D3D11_TEXTURE2D_DESC desc{};
        m_current.Texture()->GetDesc(&desc);
        tenBit = desc.Format == DXGI_FORMAT_P010;
        hdrContent = IsHdr10Frame(m_current.frame.get(), desc.Format);
    }
    m_contentIsHdr10 = hdrContent;
    // RTX Video HDR: SDR video on an HDR display is converted to HDR10 after upscaling.
    const bool trueHdr = m_trueHdrEnabled && m_hasVideo && m_current && !hdrContent && m_display.hdr &&
                         m_renderer.SupportsTrueHdr() && m_outputPass.IsAvailable();
    const DXGI_FORMAT sdrFormat = tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    m_thdrMaxLuminance =
        m_display.maxNits > 0.0f ? std::clamp(static_cast<unsigned>(m_display.maxNits), 400u, 2000u) : 1000u;
    ApplyTrueHdrSettings();

    OutputMode want = OutputMode::Sdr8;
    if (m_outputPass.IsAvailable())
    {
        if ((hdrContent || trueHdr) && m_display.hdr)
            want = OutputMode::Hdr10;
        else if (tenBit)
            want = OutputMode::Sdr10; // 10-bit SDR, and HDR10 files tone-mapped by OutputPass
    }
    // HDR10 files stay PQ through VSR; on an SDR output OutputPass tone-maps them (BT.2390).
    const bool pqVideo = hdrContent && want != OutputMode::Sdr8;
    if (hdrContent)
        LogF("HDR10 video; display %ls: Windows HDR %s, peak %.0f nits, SDR white %.0f nits",
             m_display.deviceName.c_str(), m_display.hdr ? "on" : "off", m_display.maxNits, m_display.sdrWhiteNits);
    if (want == m_outputMode && trueHdr == m_trueHdrOutput && pqVideo == m_pqVideo &&
        (!trueHdr || sdrFormat == m_sdrFormat))
        return;

    // Everything holding a view of the back buffer or the presentation texture lets go first.
    m_overlay.ReleaseTarget();
    m_outputPass.ReleaseTarget();
    DXGI_HDR_METADATA_HDR10 metadata =
        HdrMetadataFromFrame(want == OutputMode::Hdr10 && !trueHdr ? m_current.frame.get() : nullptr);
    if (trueHdr)
    {
        // RTX Video HDR targets the display's peak: describe that to the display.
        metadata.MaxMasteringLuminance = m_thdrMaxLuminance;
        metadata.MaxContentLightLevel = static_cast<UINT16>(m_thdrMaxLuminance);
    }
    if (hdrContent)
    {
        // Tone-mapping source peak: MaxCLL, else the mastering display's peak, else 1000 nits.
        const DXGI_HDR_METADATA_HDR10 content = HdrMetadataFromFrame(m_current.frame.get());
        m_contentPeakNits = content.MaxContentLightLevel > 0 ? static_cast<float>(content.MaxContentLightLevel)
                            : content.MaxMasteringLuminance > 0 ? static_cast<float>(content.MaxMasteringLuminance)
                                                                : 1000.0f;
        m_contentPeakNits = std::clamp(m_contentPeakNits, 100.0f, 10000.0f);
    }
    m_outputMode = m_gpu.SetOutputMode(want, &metadata);
    m_trueHdrOutput = trueHdr && m_outputMode == OutputMode::Hdr10;
    m_pqVideo = hdrContent && m_outputMode != OutputMode::Sdr8;
    m_toneMap = m_pqVideo && m_outputMode == OutputMode::Sdr10;
    m_sdrFormat = sdrFormat;
    m_renderer.SetOutputMode(m_outputMode, m_trueHdrOutput, sdrFormat, m_pqVideo);
    m_overlay.SetOffscreen(m_outputMode != OutputMode::Sdr8);
    if (m_trueHdrOutput)
        LogF("Output: HDR10 via RTX Video HDR (SDR %s source, max luminance %u nits)", tenBit ? "10-bit" : "8-bit",
             m_thdrMaxLuminance);
    else if (m_toneMap)
        LogF("Output: SDR 10-bit, BT.2390 tone mapping (content peak %.0f nits -> 203 nits)", m_contentPeakNits);
    else if (m_outputMode == OutputMode::Hdr10)
        LogF("Output: HDR10 (mastering %u nits, MaxCLL %u, MaxFALL %u)", metadata.MaxMasteringLuminance,
             metadata.MaxContentLightLevel, metadata.MaxFrameAverageLightLevel);
    else if (m_outputMode == want)
        LogF("Output: %s", OutputModeName(m_outputMode));
    else
        LogF("Output: %s (requested %s)", OutputModeName(m_outputMode), OutputModeName(want));
    m_dirty = true;
}

bool App::BarWanted(double now) const
{
    if (!m_hasVideo || !m_overlay.IsAvailable())
        return false;
    if (m_paused || m_draggingSeek || m_draggingVolume || m_tuneOpen || m_draggingTune >= 0 ||
        m_openMenu != OverlayPart::None)
        return true;
    // Stays up while the mouse moves or rests on the bar itself.
    return m_mouseInside && (now - m_lastMouseMove < kBarHideDelay || m_hover != OverlayPart::None);
}

float App::OsdOpacity(double now) const
{
    return static_cast<float>(std::clamp((m_osdUntil - now) / kOsdFadeTime, 0.0, 1.0));
}

bool App::OverlayAnimating() const
{
    const double now = Now();
    const float target = BarWanted(now) ? 1.0f : 0.0f;
    return m_barOpacity != target || OsdOpacity(now) > 0.0f;
}

void App::UpdateOverlay()
{
    const double now = Now();
    const double dt = std::min(now - m_lastOverlayUpdate, 0.1);
    m_lastOverlayUpdate = now;

    const bool want = BarWanted(now);
    const float step = static_cast<float>(dt / kBarFadeTime);
    m_barOpacity = want ? std::min(1.0f, m_barOpacity + step) : std::max(0.0f, m_barOpacity - step);

    // In fullscreen the cursor disappears together with the bar.
    const bool hideCursor = m_fullscreen && m_hasVideo && m_mouseInside && !want;
    if (hideCursor != m_cursorHidden)
    {
        m_cursorHidden = hideCursor;
        SetCursor(hideCursor ? nullptr : LoadCursor(nullptr, IDC_ARROW));
    }
}

OverlayState App::BuildOverlayState()
{
    OverlayState s;
    if (!m_hasVideo)
    {
        s.osdText = L"Drop a video here or press O to open";
        s.osdOpacity = 1.0f;
        return s;
    }
    const double now = Now();
    s.barOpacity = m_barOpacity;
    s.paused = m_paused;
    s.hasAudio = m_hasAudio;
    s.muted = m_muted;
    s.fullscreen = m_fullscreen;
    s.position = MediaTime();
    s.duration = m_decoder.Duration();
    s.seekPreview = m_seekPreview;
    s.draggingSeek = m_draggingSeek;
    s.volume = m_volume;
    s.upscalerOn = m_mode != UpscaleMode::Off;
    s.upscalerLabel = m_mode == UpscaleMode::Off           ? L"VSR off"
                      : m_mode == UpscaleMode::RtxVideoSdk ? L"RTX VSR"
                                                           : L"VSR (driver)";
    s.showQuality = m_mode == UpscaleMode::RtxVideoSdk;
    // The RTX HDR chip only appears where it can do something: SDR video on an HDR display.
    s.showTrueHdr = m_display.hdr && m_renderer.SupportsTrueHdr() && !m_contentIsHdr10;
    s.trueHdrOn = m_trueHdrOutput && m_renderer.TrueHdrActive();
    s.tuneOpen = m_tuneOpen;

    if (const SubtitleTrack* track = CurrentSubtitleTrack())
        s.subCount = track->Active(s.position, s.subs, OverlayState::kMaxSubs);
    s.videoRect = m_renderer.LastDestRect();
    s.videoW = m_decoder.Width();
    s.videoH = m_decoder.Height();
    s.showAudioButton = m_audioMenu.size() > 1;
    s.showSubsButton = m_subMenu.size() > 1;
    s.subsOn = m_subChoice > 0;
    s.openMenu = m_openMenu;
    s.menuItems = m_openMenu == OverlayPart::AudioButton  ? &m_audioMenu
                  : m_openMenu == OverlayPart::SubsButton ? &m_subMenu
                                                          : nullptr;
    s.menuSelected = m_openMenu == OverlayPart::AudioButton ? m_audioChoice : m_subChoice;
    s.menuHover = m_menuHover;
    s.tuneRow = m_tuneRow;
    s.tuneValues[0] = m_settings.thdrContrast;
    s.tuneValues[1] = m_settings.thdrSaturation;
    s.tuneValues[2] = m_settings.thdrMiddleGray;
    s.quality = m_quality;
    s.hover = m_hover;
    const float osd = OsdOpacity(now);
    if (osd > 0.0f)
    {
        s.osdText = m_osdText;
        s.osdOpacity = osd;
    }
    return s;
}

void App::ShowOsd(const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    vswprintf(m_osdText, ARRAYSIZE(m_osdText), format, args);
    va_end(args);
    m_osdUntil = Now() + kOsdDuration;
    m_overlayDirty = true;
}

void App::ShowSeekOsd()
{
    if (!m_hasVideo)
        return;
    wchar_t pos[32], dur[32];
    FormatTime(MediaTime(), pos, ARRAYSIZE(pos));
    FormatTime(m_decoder.Duration(), dur, ARRAYSIZE(dur));
    ShowOsd(L"%ls / %ls", pos, dur);
}

void App::OnMouseMove(int x, int y)
{
    if (!m_trackingMouse)
    {
        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hwnd, 0};
        TrackMouseEvent(&tme);
        m_trackingMouse = true;
    }
    m_mouseInside = true;
    // Windows also sends WM_MOUSEMOVE without movement (e.g. on resize); only real
    // movement should keep the bar visible.
    if (x != m_mouseX || y != m_mouseY)
    {
        m_mouseX = x;
        m_mouseY = y;
        m_lastMouseMove = Now();
    }
    if (!m_hasVideo)
        return;

    const double duration = m_decoder.Duration();
    if (m_draggingTune >= 0)
    {
        const float f = m_overlay.TuneFraction(m_draggingTune, x);
        SetTuneValue(m_draggingTune, m_draggingTune == 2 ? 10u + static_cast<unsigned>(std::lround(f * 90.0f))
                                                         : static_cast<unsigned>(std::lround(f * 200.0f)));
    }
    if (m_draggingSeek)
        m_seekPreview = m_overlay.SeekFraction(x) * duration;
    else if (m_draggingVolume)
        SetVolume(m_overlay.VolumeFraction(x));

    int item = -1;
    m_hover = m_overlay.HitTest(x, y, &item);
    m_menuHover = m_hover == OverlayPart::MenuItem ? item : -1;
    if (!m_draggingSeek)
        m_seekPreview = (m_hover == OverlayPart::Seek && duration > 0.0) ? m_overlay.SeekFraction(x) * duration : -1.0;
    m_overlayDirty = true;
}

void App::OnMouseDown(int x, int y)
{
    if (!m_hasVideo)
        return;
    m_lastMouseMove = Now();
    int item = -1;
    m_hover = m_overlay.HitTest(x, y, &item);
    // An open track menu takes the click: pick an entry, or close it when clicking elsewhere.
    if (m_openMenu != OverlayPart::None && m_hover != OverlayPart::Menu && m_hover != m_openMenu)
    {
        const OverlayPart menu = m_openMenu;
        m_openMenu = OverlayPart::None;
        m_overlayDirty = true;
        if (m_hover == OverlayPart::MenuItem)
        {
            if (menu == OverlayPart::AudioButton)
                SelectAudioTrack(item);
            else
                SelectSubtitleTrack(item);
        }
        return;
    }
    switch (m_hover)
    {
    case OverlayPart::AudioButton:
    case OverlayPart::SubsButton: ToggleMenu(m_hover); break;
    case OverlayPart::TuneContrast:
    case OverlayPart::TuneSaturation:
    case OverlayPart::TuneMiddleGray:
    {
        const int row = static_cast<int>(m_hover) - static_cast<int>(OverlayPart::TuneContrast);
        SetCapture(m_hwnd);
        m_draggingTune = row;
        const float f = m_overlay.TuneFraction(row, x);
        SetTuneValue(row, row == 2 ? 10u + static_cast<unsigned>(std::lround(f * 90.0f))
                                   : static_cast<unsigned>(std::lround(f * 200.0f)));
        break;
    }
    case OverlayPart::TuneReset:
        m_settings.thdrContrast = 100;
        m_settings.thdrSaturation = 100;
        SetTuneValue(2, 50);
        ShowOsd(L"RTX Video HDR: default tuning");
        break;
    case OverlayPart::None:
        if (m_tuneOpen)
            ToggleTunePanel(); // click on the video closes the panel
        break;
    case OverlayPart::Seek:
        if (m_decoder.Duration() > 0.0)
        {
            SetCapture(m_hwnd);
            m_draggingSeek = true;
            m_seekPreview = m_overlay.SeekFraction(x) * m_decoder.Duration();
        }
        break;
    case OverlayPart::Volume:
        SetCapture(m_hwnd);
        m_draggingVolume = true;
        SetVolume(m_overlay.VolumeFraction(x));
        break;
    case OverlayPart::PlayPause: TogglePause(); break;
    case OverlayPart::Mute: ToggleMute(); break;
    case OverlayPart::TrueHdr: ToggleTrueHdr(); break;
    case OverlayPart::Upscaler: CycleUpscaleMode(); break;
    case OverlayPart::Quality: SetQuality(m_quality % 4 + 1); break;
    case OverlayPart::Fullscreen: ToggleFullscreen(); break;
    default: break;
    }
    m_overlayDirty = true;
}

void App::OnMouseUp()
{
    if (m_draggingSeek)
    {
        const double target = m_seekPreview;
        m_draggingSeek = false; // before ReleaseCapture (WM_CAPTURECHANGED)
        m_seekPreview = -1.0;
        ReleaseCapture();
        if (target >= 0.0)
        {
            SeekTo(target);
            ShowSeekOsd();
        }
    }
    else if (m_draggingVolume)
    {
        m_draggingVolume = false;
        ReleaseCapture();
    }
    else if (m_draggingTune >= 0)
    {
        m_draggingTune = -1;
        ReleaseCapture();
    }
    m_overlayDirty = true;
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

    // Rebuild everything on a new device, unless the last recovery was very recent
    // (repeated resets point at a persistent problem: stop instead of looping).
    const double now = Now();
    if (now - m_lastDeviceRecovery > kMinSecondsBetweenRecoveries)
    {
        m_lastDeviceRecovery = now;
        if (RecoverDevice())
            return true; // this loop iteration is done; continue with the new device
    }
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

// ---------------------------------------------------------------------------
// Audio / subtitle tracks
// ---------------------------------------------------------------------------

void App::BuildTrackMenus()
{
    m_audioMenu.clear();
    m_subMenu.clear();
    m_audioChoice = -1;
    m_subChoice = 0;
    m_openMenu = OverlayPart::None;

    const auto& audio = m_decoder.AudioStreams();
    for (size_t i = 0; i < audio.size(); ++i)
    {
        m_audioMenu.push_back(audio[i].label);
        if (audio[i].index == m_decoder.DefaultAudioStream())
            m_audioChoice = static_cast<int>(i);
    }

    // Subtitles: Off, embedded tracks, external files. Default: a forced track, else one the
    // file marks as default, else off.
    m_subMenu.push_back(L"Off");
    const auto& subs = m_decoder.SubtitleStreams();
    int forced = -1, flagged = -1;
    for (size_t i = 0; i < subs.size(); ++i)
    {
        m_subMenu.push_back(subs[i].label);
        if (subs[i].isForced && forced < 0)
            forced = static_cast<int>(i) + 1;
        if (subs[i].isDefault && flagged < 0)
            flagged = static_cast<int>(i) + 1;
    }
    for (const ExternalSubtitle& ext : m_externalSubs)
        m_subMenu.push_back(L"External: " + ext.label);

    const int initial = forced > 0 ? forced : flagged > 0 ? flagged : 0;
    if (initial > 0)
        SelectSubtitleTrack(initial, false); // at open: nothing decoded yet, no re-seek needed
}

void App::SelectAudioTrack(int choice)
{
    const auto& audio = m_decoder.AudioStreams();
    if (choice < 0 || choice >= static_cast<int>(audio.size()) || m_filePath.empty())
        return;
    if (choice != m_audioChoice || !m_hasAudio)
    {
        const double pos = MediaTime();
        AnchorClock(pos); // keep time flowing smoothly while the new track starts
        m_hasAudio = m_audio.Open(m_filePath, pos, m_paused, m_muted ? 0.0f : m_volume, audio[choice].index);
        m_audioChoice = choice;
    }
    ShowOsd(L"Audio: %ls", m_audioMenu[static_cast<size_t>(choice)].c_str());
}

void App::SelectSubtitleTrack(int choice, bool reseek)
{
    if (choice < 0 || choice >= static_cast<int>(m_subMenu.size()))
        return;
    m_subChoice = choice;
    const auto& embedded = m_decoder.SubtitleStreams();
    if (choice >= 1 && choice <= static_cast<int>(embedded.size()))
    {
        m_decoder.SelectSubtitleStream(embedded[static_cast<size_t>(choice - 1)].index);
        // Re-read from the last keyframe so lines already on screen now are decoded too.
        if (reseek && m_hasVideo && m_current)
            SeekVideo(MediaTime());
    }
    else
    {
        m_decoder.SelectSubtitleStream(-1); // off, or an external file (fully loaded already)
    }
    m_dirty = true;
    ShowOsd(choice == 0 ? L"Subtitles: off" : L"Subtitles: %ls", m_subMenu[static_cast<size_t>(choice)].c_str());
}

const SubtitleTrack* App::CurrentSubtitleTrack() const
{
    const int embeddedCount = static_cast<int>(m_decoder.SubtitleStreams().size());
    if (m_subChoice >= 1 && m_subChoice <= embeddedCount)
        return &m_decoder.Subtitles();
    const int ext = m_subChoice - embeddedCount - 1;
    if (m_subChoice > 0 && ext >= 0 && ext < static_cast<int>(m_externalSubs.size()))
        return &m_externalSubs[static_cast<size_t>(ext)].track;
    return nullptr;
}

void App::ToggleMenu(OverlayPart menu)
{
    m_openMenu = m_openMenu == menu ? OverlayPart::None : menu;
    m_menuHover = -1;
    m_overlayDirty = true;
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------

void App::ReopenAudio(const char* reason)
{
    if (!m_hasVideo || m_filePath.empty() || m_audioMenu.empty())
        return;
    const int stream = m_audioChoice >= 0 ? m_decoder.AudioStreams()[static_cast<size_t>(m_audioChoice)].index : -1;
    const double pos = MediaTime();
    AnchorClock(pos);
    LogF("Audio: reopening (%s)", reason);
    m_hasAudio = m_audio.Open(m_filePath, pos, m_paused, m_muted ? 0.0f : m_volume, stream);
    ShowOsd(m_hasAudio ? L"Audio output changed" : L"No audio output available");
}

bool App::RecoverDevice()
{
    LogF("Recovering from GPU device loss: rebuilding the graphics pipeline");
    const bool hadVideo = m_hasVideo;
    const double pos = hadVideo ? MediaTime() : 0.0;
    const int subChoice = m_subChoice;

    // Release everything created on the old device: users first, the device last.
    m_current = VideoFrame{};
    m_next = VideoFrame{};
    m_decoder.Close();
    m_overlay = Overlay();
    m_outputPass = OutputPass();
    m_renderer = Renderer();
    if (m_gpu.Context())
    {
        // Drop the context's references to the old swap chain now: only one flip-model swap
        // chain may exist per window, and the new one is created next.
        m_gpu.Context()->ClearState();
        m_gpu.Context()->Flush();
    }
    m_gpu = GpuDevice();
    m_outputMode = OutputMode::Sdr8;
    m_trueHdrOutput = false;
    m_pqVideo = false;
    m_toneMap = false;

    if (!m_gpu.Init(m_hwnd, m_options.d3dDebug) || !m_renderer.Init(&m_gpu, !m_options.noSdk))
    {
        LogF("Recovery failed: could not recreate the D3D11 device");
        return false;
    }
    m_overlay.Init(&m_gpu, m_hwnd);
    m_outputPass.Init(&m_gpu);
    ApplyTrueHdrSettings();
    if (!m_renderer.HasRtxVideoSdk() && m_mode == UpscaleMode::RtxVideoSdk)
        m_mode = UpscaleMode::Off;

    if (hadVideo)
    {
        // Audio keeps playing; the video catches up at the audio position.
        std::string error;
        if (!m_decoder.Open(m_filePath, m_gpu.Device(), error))
        {
            LogF("Recovery: could not reopen the video: %s", error.c_str());
            CloseVideo();
        }
        else
        {
            m_eof = false;
            const auto& embedded = m_decoder.SubtitleStreams();
            if (subChoice >= 1 && subChoice <= static_cast<int>(embedded.size()))
                m_decoder.SelectSubtitleStream(embedded[static_cast<size_t>(subChoice - 1)].index);
            if (!SeekVideo(pos))
                CloseVideo();
        }
    }
    UpdateOutputMode();
    m_dirty = true;
    m_loggedMode = -1;
    ShowOsd(L"Recovered from a graphics driver reset");
    LogF("Recovery complete");
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
    UpdateOutputMode(); // the window may now be on a different monitor
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
                 L"%ls  |  %dx%d -> %ldx%ld%ls  |  Upscale: %ls  Q%d  |  %ls  |  %ls / %ls%ls  -  %ls",
                 m_fileName.c_str(), m_decoder.Width(), m_decoder.Height(), d.right - d.left, d.bottom - d.top,
                 m_trueHdrOutput                     ? L"  RTX HDR"
                 : m_toneMap                         ? L"  HDR\u2192SDR"
                 : m_outputMode == OutputMode::Hdr10 ? L"  HDR10"
                 : m_outputMode == OutputMode::Sdr10 ? L"  10-bit"
                                                     : L"",
                 modeText, m_quality, audioText, pos, dur,
                 m_paused ? L"  [Paused]" : L"", kAppName);
    }
    SetWindowTextW(m_hwnd, title);
}

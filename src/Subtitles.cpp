#include "Subtitles.h"

#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace
{
constexpr double kOpenEnded = std::numeric_limits<double>::infinity();
constexpr double kMaxOpenTextSeconds = 10.0; // text without an end time disappears after this
constexpr int kActiveScanLimit = 64;          // how far back Active() looks for long events

std::wstring WideFromUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string Utf8FromWide(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

void TrimTrailing(std::wstring& s)
{
    while (!s.empty() && (s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
}

// Palette-indexed (PAL8) bitmap -> premultiplied BGRA.
SubImage ConvertBitmap(const AVSubtitleRect& r)
{
    SubImage img;
    img.x = r.x;
    img.y = r.y;
    img.w = r.w;
    img.h = r.h;
    img.bgra.resize(static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 4);
    const auto* palette = reinterpret_cast<const uint32_t*>(r.data[1]); // 0xAARRGGBB
    for (int y = 0; y < r.h; ++y)
    {
        const uint8_t* src = r.data[0] + static_cast<ptrdiff_t>(y) * r.linesize[0];
        uint8_t* dst = img.bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(r.w) * 4;
        for (int x = 0; x < r.w; ++x)
        {
            const uint32_t argb = palette[src[x]];
            const uint32_t a = argb >> 24;
            dst[x * 4 + 0] = static_cast<uint8_t>((argb & 0xFF) * a / 255);         // B
            dst[x * 4 + 1] = static_cast<uint8_t>(((argb >> 8) & 0xFF) * a / 255);  // G
            dst[x * 4 + 2] = static_cast<uint8_t>(((argb >> 16) & 0xFF) * a / 255); // R
            dst[x * 4 + 3] = static_cast<uint8_t>(a);
        }
    }
    return img;
}

bool LoadSubtitleFile(const std::wstring& path, SubtitleTrack& track)
{
    AVFormatContext* fmt = nullptr;
    const std::string utf8 = Utf8FromWide(path);
    if (avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr) < 0)
        return false;
    bool ok = false;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    const AVCodec* codec = nullptr;
    int index = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0)
        index = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, &codec, 0);
    if (index >= 0 && codec)
    {
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, fmt->streams[index]->codecpar);
        ctx->pkt_timebase = fmt->streams[index]->time_base;
        if (avcodec_open2(ctx, codec, nullptr) >= 0)
        {
            while (av_read_frame(fmt, pkt) >= 0)
            {
                if (pkt->stream_index == index)
                {
                    AVSubtitle sub{};
                    int got = 0;
                    if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) >= 0 && got)
                    {
                        track.Add(sub, 0.0, 0, 0);
                        avsubtitle_free(&sub);
                    }
                }
                av_packet_unref(pkt);
            }
            ok = !track.Empty();
        }
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok;
}
} // namespace

std::wstring AssToPlainText(const char* line)
{
    // FFmpeg's decoded ASS events: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text".
    const char* p = line;
    int commas = 0;
    while (*p && commas < 8)
    {
        if (*p == ',')
            ++commas;
        ++p;
    }
    if (commas < 8)
        p = line; // not that format: take the whole line

    std::string out;
    bool inTag = false;
    bool drawing = false; // inside a {\p1}...{\p0} vector drawing: not text
    for (; *p; ++p)
    {
        const char c = *p;
        if (inTag)
        {
            if (c == '}')
                inTag = false;
            else if (c == '\\' && p[1] == 'p' && std::isdigit(static_cast<unsigned char>(p[2])))
                drawing = p[2] != '0';
            continue;
        }
        if (c == '{')
        {
            inTag = true;
            continue;
        }
        if (drawing)
            continue;
        if (c == '\\' && (p[1] == 'N' || p[1] == 'n'))
        {
            out += '\n';
            ++p;
            continue;
        }
        if (c == '\\' && p[1] == 'h')
        {
            out += ' ';
            ++p;
            continue;
        }
        if (c != '\r')
            out += c;
    }
    std::wstring w = WideFromUtf8(out);
    TrimTrailing(w);
    return w;
}

void SubtitleTrack::Add(const AVSubtitle& sub, double offsetSeconds, int canvasW, int canvasH)
{
    if (sub.pts == AV_NOPTS_VALUE)
        return;
    const double base = static_cast<double>(sub.pts) / AV_TIME_BASE - offsetSeconds;
    const double start = base + sub.start_display_time / 1000.0;
    double end = kOpenEnded;
    if (sub.end_display_time > sub.start_display_time && sub.end_display_time < 0x7FFFFFFFu)
        end = base + sub.end_display_time / 1000.0;

    // A new event ends any still-open earlier ones (image subs signal "clear" this way).
    int scanned = 0;
    for (auto it = m_events.rbegin(); it != m_events.rend() && scanned < kActiveScanLimit; ++it, ++scanned)
    {
        if (it->end == kOpenEnded && it->start < start)
            it->end = start;
    }
    if (sub.num_rects == 0)
        return; // pure "clear" event

    SubEvent ev;
    ev.id = m_nextId++;
    ev.start = start;
    ev.end = end;
    ev.canvasW = canvasW;
    ev.canvasH = canvasH;
    for (unsigned i = 0; i < sub.num_rects; ++i)
    {
        const AVSubtitleRect* r = sub.rects[i];
        std::wstring text;
        if (r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0 && r->data[0] && r->data[1])
            ev.images.push_back(ConvertBitmap(*r));
        else if (r->type == SUBTITLE_ASS && r->ass)
            text = AssToPlainText(r->ass);
        else if (r->type == SUBTITLE_TEXT && r->text)
            text = WideFromUtf8(r->text);
        if (!text.empty())
        {
            if (!ev.text.empty())
                ev.text += L'\n';
            ev.text += text;
        }
    }
    if (ev.text.empty() && ev.images.empty())
        return;

    const auto pos = std::upper_bound(m_events.begin(), m_events.end(), start,
                                      [](double t, const SubEvent& e) { return t < e.start; });
    m_events.insert(pos, std::move(ev));
}

void SubtitleTrack::Prune(double t, double keepSeconds)
{
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
                                  [&](const SubEvent& e) { return e.end < t - keepSeconds; }),
                   m_events.end());
}

int SubtitleTrack::Active(double t, const SubEvent** out, int maxOut) const
{
    // Events are sorted by start: look back from the last one that has started.
    const auto it = std::upper_bound(m_events.begin(), m_events.end(), t,
                                     [](double v, const SubEvent& e) { return v < e.start; });
    int count = 0;
    int scanned = 0;
    for (auto r = std::make_reverse_iterator(it); r != m_events.rend() && scanned < kActiveScanLimit && count < maxOut;
         ++r, ++scanned)
    {
        double end = r->end;
        if (end == kOpenEnded && r->images.empty())
            end = r->start + kMaxOpenTextSeconds;
        if (t < end)
            out[count++] = &*r;
    }
    std::reverse(out, out + count); // start order: earlier lines on top
    return count;
}

std::vector<ExternalSubtitle> LoadExternalSubtitles(const std::wstring& videoPath)
{
    std::vector<ExternalSubtitle> result;
    const size_t slash = videoPath.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"" : videoPath.substr(0, slash + 1);
    std::wstring base = slash == std::wstring::npos ? videoPath : videoPath.substr(slash + 1);
    const size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos)
        base.resize(dot);

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + base + L"*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return result;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::wstring name = fd.cFileName;
        const size_t ext = name.find_last_of(L'.');
        if (ext == std::wstring::npos)
            continue;
        std::wstring e = name.substr(ext + 1);
        for (auto& ch : e)
            ch = static_cast<wchar_t>(std::towlower(ch));
        if (e != L"srt" && e != L"ass" && e != L"ssa" && e != L"vtt")
            continue;

        ExternalSubtitle sub;
        sub.path = dir + name;
        sub.label = name;
        if (LoadSubtitleFile(sub.path, sub.track))
        {
            LogF("Subtitles: loaded external file %ls", name.c_str());
            result.push_back(std::move(sub));
        }
        else
        {
            LogF("Subtitles: could not read %ls", name.c_str());
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return result;
}

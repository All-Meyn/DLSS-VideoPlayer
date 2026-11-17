#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AVSubtitle;

// One bitmap of an image subtitle (Blu-ray PGS / DVD VobSub), already converted to
// premultiplied BGRA so Direct2D can use it directly.
struct SubImage
{
    int x = 0, y = 0, w = 0, h = 0; // placement on the subtitle canvas
    std::vector<uint8_t> bgra;      // w * h * 4 bytes, premultiplied
};

// A subtitle shown from `start` to `end` (seconds on the video's timeline).
struct SubEvent
{
    uint64_t id = 0; // unique per track, used to cache rendered bitmaps / layouts
    double start = 0.0;
    double end = 0.0; // +infinity until the next event closes it (PGS-style "clear")
    std::wstring text; // plain text (ASS tags stripped), may contain '\n'
    std::vector<SubImage> images;
    int canvasW = 0, canvasH = 0; // coordinate space of `images` (0 = video size)
};

// Time-ordered list of subtitle events with lookup of the ones visible at a time.
class SubtitleTrack
{
public:
    // Converts one decoded subtitle (text, ASS or bitmap rects). offsetSeconds is subtracted
    // from its pts so it lines up with the video timeline.
    void Add(const AVSubtitle& sub, double offsetSeconds, int canvasW, int canvasH);
    void Clear() { m_events.clear(); }
    // Drops events that ended more than `keepSeconds` before `t` (bounds memory for image subs).
    void Prune(double t, double keepSeconds);

    // Fills `out` with up to `maxOut` events visible at time t (in start order); returns count.
    int Active(double t, const SubEvent** out, int maxOut) const;
    bool Empty() const { return m_events.empty(); }

private:
    std::vector<SubEvent> m_events;
    uint64_t m_nextId = 1;
};

// Plain text from an FFmpeg-decoded ASS line ("ReadOrder,Layer,Style,...,Text"): drops the
// leading fields and {override tags} (and \p vector drawings), turns \N into newlines.
std::wstring AssToPlainText(const char* assLine);

// An external subtitle file next to the video, fully loaded at open.
struct ExternalSubtitle
{
    std::wstring path;
    std::wstring label; // e.g. "Movie.en.srt"
    SubtitleTrack track;
};

// Finds <video base name>*.srt / *.ass / *.ssa / *.vtt in the video's folder and loads them.
std::vector<ExternalSubtitle> LoadExternalSubtitles(const std::wstring& videoPath);

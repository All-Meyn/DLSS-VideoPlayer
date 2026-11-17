#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Per-file playback memory: where you stopped and which audio / subtitle track you picked.
// Stored in %APPDATA%\VSRPlayer\history.txt (UTF-8, one tab-separated line per file, most
// recent first, at most kMaxEntries). UI thread only.
struct FileState
{
    std::wstring path;
    double position = 0.0;  // seconds; 0 = start from the beginning
    int audioStream = -1;   // FFmpeg stream index, -1 = default
    std::wstring subtitle;  // "" = file default, "off", "s:<stream index>", "x:<external file name>"
    int64_t lastUsed = 0;   // unix time
};

class PlaybackHistory
{
public:
    static constexpr size_t kMaxEntries = 500;

    void Load();
    void Save() const;
    const FileState* Find(const std::wstring& path) const;
    void Update(FileState state); // adds or replaces, moves to the front

private:
    std::vector<FileState> m_entries;
};

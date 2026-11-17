#include "PlaybackHistory.h"

#include "Log.h"
#include "Settings.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace
{
std::wstring HistoryPath()
{
    const std::wstring dir = AppDataFolder();
    return dir.empty() ? std::wstring() : dir + L"\\history.txt";
}

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0; // Windows paths are case-insensitive
}
} // namespace

void PlaybackHistory::Load()
{
    m_entries.clear();
    const std::wstring path = HistoryPath();
    if (path.empty())
        return;
    std::ifstream in(path.c_str(), std::ios::binary);
    std::string line;
    // Line format: lastUsed \t position \t audioStream \t subtitle \t path
    while (std::getline(in, line) && m_entries.size() < kMaxEntries)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string fields[5];
        std::istringstream ss(line);
        int n = 0;
        while (n < 4 && std::getline(ss, fields[n], '\t'))
            ++n;
        std::getline(ss, fields[4]); // the path is last and may contain anything but newlines
        if (n < 4 || fields[4].empty())
            continue;
        FileState st;
        st.lastUsed = std::strtoll(fields[0].c_str(), nullptr, 10);
        st.position = std::strtod(fields[1].c_str(), nullptr);
        st.audioStream = static_cast<int>(std::strtol(fields[2].c_str(), nullptr, 10));
        st.subtitle = FromUtf8(fields[3]);
        st.path = FromUtf8(fields[4]);
        m_entries.push_back(std::move(st));
    }
}

void PlaybackHistory::Save() const
{
    const std::wstring path = HistoryPath();
    if (path.empty())
        return;
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LogF("History: could not write %ls", path.c_str());
        return;
    }
    for (const FileState& st : m_entries)
    {
        char head[96];
        snprintf(head, sizeof(head), "%lld\t%.3f\t%d\t", static_cast<long long>(st.lastUsed), st.position, st.audioStream);
        out << head << ToUtf8(st.subtitle) << '\t' << ToUtf8(st.path) << '\n';
    }
}

const FileState* PlaybackHistory::Find(const std::wstring& path) const
{
    for (const FileState& st : m_entries)
        if (SamePath(st.path, path))
            return &st;
    return nullptr;
}

void PlaybackHistory::Update(FileState state)
{
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
    {
        if (SamePath(it->path, state.path))
        {
            m_entries.erase(it);
            break;
        }
    }
    state.lastUsed = static_cast<int64_t>(std::time(nullptr));
    m_entries.insert(m_entries.begin(), std::move(state));
    if (m_entries.size() > kMaxEntries)
        m_entries.resize(kMaxEntries);
}

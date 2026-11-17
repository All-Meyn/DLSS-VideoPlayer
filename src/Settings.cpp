#include "Settings.h"

#include "Log.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace
{
constexpr wchar_t kSection[] = L"RtxVideoHdr";
constexpr wchar_t kPlayerSection[] = L"Player";

std::wstring SettingsPath(const wchar_t* folderName, bool createFolder)
{
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        return {};
    std::wstring dir = std::wstring(appData) + L"\\" + folderName;
    CoTaskMemFree(appData);
    if (createFolder)
        CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

bool FileExists(const std::wstring& path)
{
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

constexpr wchar_t kFolder[] = L"VSRPlayer";
constexpr wchar_t kOldFolder[] = L"DLSSVideoPlayer"; // name before the rename

unsigned ReadUInt(const std::wstring& path, const wchar_t* key, unsigned fallback, unsigned lo, unsigned hi,
                  const wchar_t* section = kSection)
{
    const UINT v = GetPrivateProfileIntW(section, key, static_cast<INT>(fallback), path.c_str());
    return std::clamp(static_cast<unsigned>(v), lo, hi);
}

void WriteUInt(const std::wstring& path, const wchar_t* key, unsigned value, const wchar_t* section = kSection)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"%u", value);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}
} // namespace

void Settings::Load()
{
    std::wstring path = SettingsPath(kFolder, false);
    if (!FileExists(path))
    {
        // First run after the rename: pick up the old settings (saved to the new place on exit).
        const std::wstring old = SettingsPath(kOldFolder, false);
        if (FileExists(old))
        {
            path = old;
            LogF("Settings: migrating from %ls", old.c_str());
        }
    }
    if (path.empty())
        return;
    thdrContrast = ReadUInt(path, L"Contrast", thdrContrast, 0, 200);
    thdrSaturation = ReadUInt(path, L"Saturation", thdrSaturation, 0, 200);
    thdrMiddleGray = ReadUInt(path, L"MiddleGray", thdrMiddleGray, 10, 100);
    volumePercent = ReadUInt(path, L"Volume", volumePercent, 0, 100, kPlayerSection);
    muted = ReadUInt(path, L"Muted", muted ? 1u : 0u, 0, 1, kPlayerSection) != 0;
}

void Settings::Save() const
{
    const std::wstring path = SettingsPath(kFolder, true);
    if (path.empty())
        return;
    WriteUInt(path, L"Contrast", thdrContrast);
    WriteUInt(path, L"Saturation", thdrSaturation);
    WriteUInt(path, L"MiddleGray", thdrMiddleGray);
    WriteUInt(path, L"Volume", volumePercent, kPlayerSection);
    WriteUInt(path, L"Muted", muted ? 1u : 0u, kPlayerSection);
}

std::wstring AppDataFolder()
{
    const std::wstring ini = SettingsPath(kFolder, true);
    return ini.empty() ? std::wstring() : ini.substr(0, ini.find_last_of(L'\\'));
}

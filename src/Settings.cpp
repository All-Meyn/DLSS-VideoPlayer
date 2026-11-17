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

std::wstring SettingsPath(bool createFolder)
{
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        return {};
    std::wstring dir = std::wstring(appData) + L"\\DLSSVideoPlayer";
    CoTaskMemFree(appData);
    if (createFolder)
        CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

unsigned ReadUInt(const std::wstring& path, const wchar_t* key, unsigned fallback, unsigned lo, unsigned hi)
{
    const UINT v = GetPrivateProfileIntW(kSection, key, static_cast<INT>(fallback), path.c_str());
    return std::clamp(static_cast<unsigned>(v), lo, hi);
}

void WriteUInt(const std::wstring& path, const wchar_t* key, unsigned value)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"%u", value);
    WritePrivateProfileStringW(kSection, key, buf, path.c_str());
}
} // namespace

void Settings::Load()
{
    const std::wstring path = SettingsPath(false);
    if (path.empty())
        return;
    thdrContrast = ReadUInt(path, L"Contrast", thdrContrast, 0, 200);
    thdrSaturation = ReadUInt(path, L"Saturation", thdrSaturation, 0, 200);
    thdrMiddleGray = ReadUInt(path, L"MiddleGray", thdrMiddleGray, 10, 100);
}

void Settings::Save() const
{
    const std::wstring path = SettingsPath(true);
    if (path.empty())
        return;
    WriteUInt(path, L"Contrast", thdrContrast);
    WriteUInt(path, L"Saturation", thdrSaturation);
    WriteUInt(path, L"MiddleGray", thdrMiddleGray);
}

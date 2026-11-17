#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <string>

// Log file: %TEMP%\VSRPlayer\VSRPlayer.log (recreated on each start).
// Opened once on first use; shared-read so it can be viewed while the player runs.
inline FILE* LogFile()
{
    static FILE* file = [] {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"VSRPlayer";
        CreateDirectoryW(dir.c_str(), nullptr);
        return _wfsopen((dir + L"\\VSRPlayer.log").c_str(), L"w", _SH_DENYNO);
    }();
    return file;
}

// Logs to the debugger output (VS Code Debug Console / DebugView) and to the log file.
// Not for per-frame use: it formats a string and flushes the file on every call.
inline void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA("[VSRPlayer] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    static const ULONGLONG startTick = GetTickCount64();
    if (FILE* f = LogFile())
    {
        // Seconds since the first log line.
        fprintf(f, "[%8.3f] %s\n", static_cast<double>(GetTickCount64() - startTick) / 1000.0, buf);
        fflush(f);
    }
}

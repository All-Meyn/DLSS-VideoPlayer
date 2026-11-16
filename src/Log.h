#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>

// Minimal logging: goes to the debugger output window (Visual Studio / DebugView).
inline void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA("[DLSSVP] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

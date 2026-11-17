#include "App.h"

#include <objbase.h>
#include <shellapi.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // for the common file dialog

    // Usage: DLSSVideoPlayer.exe [--d3d-debug] [video file]
    std::wstring initialFile;
    bool d3dDebug = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            if (wcscmp(argv[i], L"--d3d-debug") == 0)
                d3dDebug = true;
            else
                initialFile = argv[i];
        }
        LocalFree(argv);
    }

    int result = 0;
    {
        App app;
        result = app.Run(instance, showCmd, initialFile, d3dDebug);
    }

    CoUninitialize();
    return result;
}

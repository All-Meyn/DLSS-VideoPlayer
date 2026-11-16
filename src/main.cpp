#include "App.h"

#include <objbase.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // for the common file dialog

    std::wstring initialFile;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        if (argc > 1)
            initialFile = argv[1];
        LocalFree(argv);
    }

    int result = 0;
    {
        App app;
        result = app.Run(instance, showCmd, initialFile);
    }

    CoUninitialize();
    return result;
}

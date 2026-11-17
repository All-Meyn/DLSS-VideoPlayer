#pragma once

#include <windows.h>

struct IMMDeviceEnumerator;
class DeviceNotificationClient;

// Posts `message` to `hwnd` when Windows' default audio output device changes (headphones
// plugged in, output switched in the volume flyout, device removed...). The notification
// arrives on a system thread; the app handles it on the UI thread.
class AudioDeviceWatcher
{
public:
    ~AudioDeviceWatcher();
    bool Start(HWND hwnd, UINT message);
    void Stop();

private:
    IMMDeviceEnumerator* m_enumerator = nullptr;
    DeviceNotificationClient* m_client = nullptr;
};

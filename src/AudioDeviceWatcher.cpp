#include "AudioDeviceWatcher.h"

#include "Log.h"

#include <mmdeviceapi.h>

#include <atomic>

class DeviceNotificationClient final : public IMMNotificationClient
{
public:
    DeviceNotificationClient(HWND hwnd, UINT message) : m_hwnd(hwnd), m_message(message) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if (refs == 0)
            delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient (called on a system thread: only post a message)
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole)
            PostMessageW(m_hwnd, m_message, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> m_refs{1};
    HWND m_hwnd;
    UINT m_message;
};

AudioDeviceWatcher::~AudioDeviceWatcher()
{
    Stop();
}

bool AudioDeviceWatcher::Start(HWND hwnd, UINT message)
{
    Stop();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&m_enumerator));
    if (FAILED(hr))
        return false;
    m_client = new DeviceNotificationClient(hwnd, message);
    hr = m_enumerator->RegisterEndpointNotificationCallback(m_client);
    if (FAILED(hr))
    {
        LogF("Audio: could not watch for device changes (0x%08lx)", static_cast<unsigned long>(hr));
        Stop();
        return false;
    }
    return true;
}

void AudioDeviceWatcher::Stop()
{
    if (m_enumerator && m_client)
        m_enumerator->UnregisterEndpointNotificationCallback(m_client);
    if (m_client)
    {
        m_client->Release();
        m_client = nullptr;
    }
    if (m_enumerator)
    {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

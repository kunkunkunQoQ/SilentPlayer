// Temporary diagnostic: show audio sessions of a given PID (WASAPI).
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "ole32.lib")

static DWORD g_pid = 0;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: audio_session_probe.exe <pid>\n");
        return 1;
    }
    g_pid = static_cast<DWORD>(_wtoi(argv[1]));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     IID_PPV_ARGS(&pEnum));
    IMMDevice* pDevice = nullptr;
    HRESULT hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        wprintf(L"GetDefaultAudioEndpoint failed: 0x%08lX\n", hr);
        return 1;
    }
    IAudioSessionManager2* pMgr = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                           (void**)&pMgr);
    if (FAILED(hr)) {
        wprintf(L"Activate session manager failed: 0x%08lX\n", hr);
        return 1;
    }
    IAudioSessionEnumerator* pSessions = nullptr;
    hr = pMgr->GetSessionEnumerator(&pSessions);
    if (FAILED(hr)) {
        wprintf(L"GetSessionEnumerator failed: 0x%08lX\n", hr);
        return 1;
    }
    int count = 0;
    pSessions->GetCount(&count);
    wprintf(L"total audio sessions: %d\n", count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* pCtl = nullptr;
        pSessions->GetSession(i, &pCtl);
        if (!pCtl) continue;
        IAudioSessionControl2* pCtl2 = nullptr;
        pCtl->QueryInterface(IID_PPV_ARGS(&pCtl2));
        DWORD pid = 0;
        pCtl2->GetProcessId(&pid);
        IAudioMeterInformation* pMeter = nullptr;
        pCtl->QueryInterface(IID_PPV_ARGS(&pMeter));
        float peak = 0.0f;
        if (pMeter) {
            pMeter->GetPeakValue(&peak);
            pMeter->Release();
        }
        AudioSessionState sstate = AudioSessionStateInactive;
        pCtl->GetState(&sstate);
        wprintf(L"  session pid=%lu state=%lu peak=%.4f%s\n", pid, sstate, peak,
                pid == g_pid ? L"  <== target" : L"");
        if (pCtl2) pCtl2->Release();
        if (pCtl) pCtl->Release();
    }
    pSessions->Release();
    pMgr->Release();
    pDevice->Release();
    pEnum->Release();
    CoUninitialize();
    return 0;
}

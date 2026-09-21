// Temp diagnostic: enumerate ALL render endpoints and their audio sessions.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "ole32.lib")

static void PrintDeviceName(IMMDevice* pDevice, const wchar_t* prefix) {
    IPropertyStore* pProps = nullptr;
    pDevice->OpenPropertyStore(STGM_READ, &pProps);
    if (!pProps) return;
    PROPVARIANT var{};
    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) &&
        var.vt == VT_LPWSTR) {
        wprintf(L"%ls '%ls' ", prefix, var.pwszVal);
    }
    PropVariantClear(&var);
    pProps->Release();
}

static void DumpEndpoint(IMMDevice* pDevice, const wchar_t* prefix) {
    DWORD state = 0;
    pDevice->GetState(&state);
    PrintDeviceName(pDevice, prefix);
    wprintf(L"state=%lu\n", state);

    IAudioSessionManager2* pMgr = nullptr;
    HRESULT hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                   nullptr, (void**)&pMgr);
    if (FAILED(hr)) return;
    IAudioSessionEnumerator* pSessions = nullptr;
    hr = pMgr->GetSessionEnumerator(&pSessions);
    if (SUCCEEDED(hr)) {
        int count = 0;
        pSessions->GetCount(&count);
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
            float vol = -1.0f;
            ISimpleAudioVolume* pVol = nullptr;
            if (SUCCEEDED(pCtl->QueryInterface(IID_PPV_ARGS(&pVol)))) {
                pVol->GetMasterVolume(&vol);
                pVol->Release();
            }
            float instVol = -1.0f;
            IChannelAudioVolume* pChan = nullptr;
            if (SUCCEEDED(pCtl->QueryInterface(IID_PPV_ARGS(&pChan)))) {
                pChan->GetChannelVolume(0, &instVol);
                pChan->Release();
            }
            wprintf(L"    session pid=%lu state=%d peak=%.4f sessionVol=%.4f chan0Vol=%.4f\n",
                    pid, (int)sstate, peak, vol, instVol);
            if (pCtl2) pCtl2->Release();
            if (pCtl) pCtl->Release();
        }
        pSessions->Release();
    }
    pMgr->Release();
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) { wprintf(L"no enumerator 0x%08lX\n", hr); return 1; }

    // 当前默认渲染端点
    IMMDevice* pDef = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDef);
    wprintf(L"default render (eConsole): hr=0x%08lX\n", hr);
    if (SUCCEEDED(hr)) {
        DumpEndpoint(pDef, L"  [DEFAULT]");
        pDef->Release();
    }

    // 全部渲染端点
    IMMDeviceCollection* pCol = nullptr;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
    wprintf(L"active render endpoints: hr=0x%08lX\n", hr);
    if (SUCCEEDED(hr)) {
        UINT n = 0;
        pCol->GetCount(&n);
        wprintf(L"count=%u\n", n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* pDev = nullptr;
            pCol->Item(i, &pDev);
            if (pDev) {
                DumpEndpoint(pDev, L"  [device]");
                pDev->Release();
            }
        }
        pCol->Release();
    }
    pEnum->Release();
    CoUninitialize();
    return 0;
}

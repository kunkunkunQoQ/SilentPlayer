// Diagnostic probe: WASAPI loopback capture of the endpoint that a given
// process is rendering to, and report the measured amplitude.
//
// Purpose: verify that the volume slider really changes the loudness. Neither
// IAudioMeterInformation::GetPeakValue nor ISimpleAudioVolume reflects what the
// Media Foundation session applies internally, so the only reliable check is to
// capture the actual rendered stream and measure it.
//
// usage: audio_loopback_probe.exe <pid> <milliseconds>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

#pragma comment(lib, "ole32.lib")

namespace {

// Find the render endpoint that has an audio session belonging to pid.
IMMDevice* FindEndpointForPid(IMMDeviceEnumerator* pEnum, DWORD pid) {
    IMMDeviceCollection* pColl = nullptr;
    if (FAILED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl))) {
        return nullptr;
    }
    IMMDevice* found = nullptr;
    UINT n = 0;
    pColl->GetCount(&n);
    for (UINT i = 0; i < n && !found; ++i) {
        IMMDevice* pDev = nullptr;
        if (FAILED(pColl->Item(i, &pDev))) {
            continue;
        }
        IAudioSessionManager2* pMgr = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                     nullptr, (void**)&pMgr))) {
            IAudioSessionEnumerator* pSessions = nullptr;
            if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessions))) {
                int count = 0;
                pSessions->GetCount(&count);
                for (int s = 0; s < count; ++s) {
                    IAudioSessionControl* pCtl = nullptr;
                    if (FAILED(pSessions->GetSession(s, &pCtl)) || !pCtl) {
                        continue;
                    }
                    IAudioSessionControl2* pCtl2 = nullptr;
                    DWORD spid = 0;
                    if (SUCCEEDED(pCtl->QueryInterface(IID_PPV_ARGS(&pCtl2)))) {
                        pCtl2->GetProcessId(&spid);
                    }
                    if (spid == pid) {
                        found = pDev;
                        pDev->AddRef();
                    }
                    if (pCtl2) pCtl2->Release();
                    pCtl->Release();
                    if (found) break;
                }
                pSessions->Release();
            }
            pMgr->Release();
        }
        pDev->Release();
    }
    pColl->Release();
    return found;
}

std::wstring DeviceName(IMMDevice* pDev) {
    IPropertyStore* pProps = nullptr;
    std::wstring name = L"<unknown>";
    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
        PROPVARIANT var{};
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) &&
            var.vt == VT_LPWSTR) {
            name = var.pwszVal;
        }
        PropVariantClear(&var);
        pProps->Release();
    }
    return name;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: audio_loopback_probe.exe <pid> <milliseconds>\n");
        return 1;
    }
    const DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    const int ms = _wtoi(argv[2]);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pEnum)))) {
        wprintf(L"no enumerator\n");
        return 1;
    }
    IMMDevice* pDev = FindEndpointForPid(pEnum, pid);
    if (!pDev) {
        wprintf(L"no render endpoint with a session for pid %lu\n", pid);
        pEnum->Release();
        return 1;
    }
    wprintf(L"endpoint = %ls\n", DeviceName(pDev).c_str());

    IAudioClient* pClient = nullptr;
    HRESULT hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                (void**)&pClient);
    if (FAILED(hr)) {
        wprintf(L"activate IAudioClient failed 0x%08lX\n", hr);
        return 1;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        wprintf(L"GetMixFormat failed 0x%08lX\n", hr);
        return 1;
    }
    const int channels = pwfx->nChannels;
    const int bits = pwfx->wBitsPerSample;
    const bool isFloat = pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx)
                                  ->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
    wprintf(L"format = %u ch, %d bits, %lu Hz, %ls\n", channels, bits,
            pwfx->nSamplesPerSec, isFloat ? L"float" : L"pcm");

    hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000 /*1s*/, 0,
                             pwfx, nullptr);
    if (FAILED(hr)) {
        wprintf(L"Initialize(loopback) failed 0x%08lX\n", hr);
        return 1;
    }
    IAudioCaptureClient* pCapture = nullptr;
    hr = pClient->GetService(IID_PPV_ARGS(&pCapture));
    if (FAILED(hr)) {
        wprintf(L"GetService(IAudioCaptureClient) failed 0x%08lX\n", hr);
        return 1;
    }
    hr = pClient->Start();
    if (FAILED(hr)) {
        wprintf(L"Start failed 0x%08lX\n", hr);
        return 1;
    }

    double peak = 0.0;
    double sumSq = 0.0;
    long long frames = 0;
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(ms);
    while (GetTickCount() < deadline) {
        Sleep(20);
        UINT32 packet = 0;
        if (FAILED(pCapture->GetNextPacketSize(&packet))) {
            break;
        }
        while (packet != 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            if (FAILED(pCapture->GetBuffer(&data, &numFrames, &flags, nullptr,
                                           nullptr))) {
                break;
            }
            if (data && numFrames > 0 &&
                !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                for (UINT32 f = 0; f < numFrames; ++f) {
                    for (int c = 0; c < channels; ++c) {
                        double v = 0.0;
                        if (isFloat) {
                            v = reinterpret_cast<float*>(data)[f * channels + c];
                        } else if (bits == 16) {
                            v = reinterpret_cast<short*>(data)[f * channels + c] /
                                32768.0;
                        } else if (bits == 32) {
                            v = reinterpret_cast<int*>(data)[f * channels + c] /
                                2147483648.0;
                        }
                        const double a = std::fabs(v);
                        if (a > peak) peak = a;
                        sumSq += v * v;
                    }
                }
                frames += numFrames;
            }
            pCapture->ReleaseBuffer(numFrames);
            if (FAILED(pCapture->GetNextPacketSize(&packet))) {
                break;
            }
        }
    }
    pClient->Stop();

    const double rms = frames > 0 ? std::sqrt(sumSq / (frames * channels)) : 0.0;
    wprintf(L"captured %lld frames  peak=%.5f  rms=%.5f\n", frames, peak, rms);

    pCapture->Release();
    pClient->Release();
    CoTaskMemFree(pwfx);
    pDev->Release();
    pEnum->Release();
    CoUninitialize();
    return 0;
}

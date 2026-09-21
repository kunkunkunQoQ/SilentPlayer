// 路由探针：把测试音渲染到指定输出设备，同时从指定采集设备录音，测采集到的电平。
//
// 用途：确定"把音频渲染到哪个虚拟输出设备，它才会出现在虚拟麦克风里"——
// 这是实现「麦克风输出」开关时选择目标设备的依据，不靠猜。
//
// usage: audio_route_probe.exe <输出设备名子串> <采集设备名子串> [毫秒]
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <algorithm>

#pragma comment(lib, "ole32.lib")

namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return s;
}

std::wstring FriendlyName(IMMDevice* dev) {
    IPropertyStore* props = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props)) || !props) {
        return {};
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) &&
        var.vt == VT_LPWSTR && var.pwszVal) {
        name = var.pwszVal;
    }
    PropVariantClear(&var);
    props->Release();
    return name;
}

IMMDevice* FindDevice(IMMDeviceEnumerator* en, EDataFlow flow,
                      const std::wstring& needle, std::wstring* foundName) {
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev)) || !dev) {
                continue;
            }
            const std::wstring name = FriendlyName(dev);
            if (ToLower(name).find(ToLower(needle)) != std::wstring::npos) {
                *foundName = name;
                col->Release();
                return dev;
            }
            dev->Release();
        }
        col->Release();
    }
    return nullptr;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        wprintf(L"usage: audio_route_probe.exe <renderName> <captureName> [ms]\n");
        return 1;
    }
    const std::wstring renderNeedle = argv[1];
    const std::wstring captureNeedle = argv[2];
    const int ms = argc > 3 ? _wtoi(argv[3]) : 2000;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) ||
        !en) {
        wprintf(L"no enumerator\n");
        return 1;
    }

    // ---- 采集侧 ----
    std::wstring capName;
    IMMDevice* capDev = FindDevice(en, eCapture, captureNeedle, &capName);
    if (!capDev) {
        wprintf(L"capture device '%ls' not found\n", captureNeedle.c_str());
        en->Release();
        return 1;
    }
    IAudioClient* capClient = nullptr;
    WAVEFORMATEX* capFmt = nullptr;
    IAudioCaptureClient* cap = nullptr;
    if (FAILED(capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                (void**)&capClient)) ||
        FAILED(capClient->GetMixFormat(&capFmt)) ||
        FAILED(capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0,
                                     capFmt, nullptr)) ||
        FAILED(capClient->GetService(IID_PPV_ARGS(&cap)))) {
        wprintf(L"capture init failed\n");
        return 1;
    }

    // ---- 渲染侧：1kHz 正弦 ----
    std::wstring renName;
    IMMDevice* renDev = FindDevice(en, eRender, renderNeedle, &renName);
    if (!renDev) {
        wprintf(L"render device '%ls' not found\n", renderNeedle.c_str());
        return 1;
    }
    IAudioClient* renClient = nullptr;
    WAVEFORMATEX* renFmt = nullptr;
    IAudioRenderClient* ren = nullptr;
    if (FAILED(renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                (void**)&renClient)) ||
        FAILED(renClient->GetMixFormat(&renFmt)) ||
        FAILED(renClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0,
                                     renFmt, nullptr)) ||
        FAILED(renClient->GetService(IID_PPV_ARGS(&ren)))) {
        wprintf(L"render init failed\n");
        return 1;
    }

    wprintf(L"render  -> '%ls'\n", renName.c_str());
    wprintf(L"capture <- '%ls'\n", capName.c_str());

    capClient->Start();
    renClient->Start();

    UINT32 bufFrames = 0;
    renClient->GetBufferSize(&bufFrames);
    const UINT32 rate = renFmt->nSamplesPerSec;
    const UINT32 chans = renFmt->nChannels;
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979 * 1000.0 / rate;

    float peak = 0.0f;
    double sumSq = 0.0;
    UINT64 totalFrames = 0;
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(ms > 0 ? ms : 1);

    while (GetTickCount() < deadline) {
        // 填一段正弦到渲染缓冲
        UINT32 padding = 0;
        if (SUCCEEDED(renClient->GetCurrentPadding(&padding))) {
            const UINT32 avail = bufFrames - padding;
            if (avail > 0) {
                BYTE* data = nullptr;
                if (SUCCEEDED(ren->GetBuffer(avail, &data)) && data) {
                    float* f = reinterpret_cast<float*>(data);
                    for (UINT32 i = 0; i < avail; ++i) {
                        const float v = static_cast<float>(0.35 * sin(phase));
                        phase += step;
                        for (UINT32 c = 0; c < chans; ++c) {
                            f[i * chans + c] = v;
                        }
                    }
                    ren->ReleaseBuffer(avail, 0);
                }
            }
        }
        // 读采集缓冲
        BYTE* cdata = nullptr;
        UINT32 cframes = 0;
        DWORD flags = 0;
        while (SUCCEEDED(cap->GetBuffer(&cdata, &cframes, &flags, nullptr,
                                        nullptr)) &&
               cframes > 0) {
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && cdata) {
                const float* f = reinterpret_cast<const float*>(cdata);
                for (UINT32 i = 0; i < cframes; ++i) {
                    const float v = f[i];
                    peak = (std::max)(peak, std::fabs(v));
                    sumSq += static_cast<double>(v) * v;
                }
                totalFrames += cframes;
            }
            cap->ReleaseBuffer(cframes);
        }
        Sleep(20);
    }

    const double rms =
        totalFrames > 0 ? std::sqrt(sumSq / static_cast<double>(totalFrames)) : 0.0;
    wprintf(L"peak=%.4f rms=%.4f frames=%llu  => %ls\n", peak, rms,
            static_cast<unsigned long long>(totalFrames),
            (peak > 0.001) ? L"YES 声音到达了这个麦克风" : L"no 没有到达");

    renClient->Stop();
    capClient->Stop();
    ren->Release();
    renClient->Release();
    cap->Release();
    capClient->Release();
    renDev->Release();
    capDev->Release();
    en->Release();
    CoUninitialize();
    return 0;
}

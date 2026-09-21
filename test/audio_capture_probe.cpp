// 从指定采集端点（麦克风 / 虚拟麦克风）录音并测量电平。
//
// 用途：验证"把音频渲染到某个虚拟设备后，它是否真的出现在对应的虚拟麦克风里"——
// 也就是判断「让语音软件听到播放器的声音」这条链路是否走通。
//
// usage: audio_capture_probe.exe [设备名子串] [毫秒数]
//   不给设备名子串则用系统默认采集设备。
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

// 按名字子串找采集端点；找不到返回默认采集端点。
IMMDevice* FindCapture(IMMDeviceEnumerator* en, const std::wstring& needle,
                       std::wstring* foundName) {
    if (!needle.empty()) {
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                             &col)) &&
            col) {
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
                    return dev; // 调用方负责 Release
                }
                dev->Release();
            }
            col->Release();
        }
    }
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def)) && def) {
        *foundName = FriendlyName(def) + L"  (默认采集设备)";
    }
    return def;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring needle;
    int ms = 2000;
    if (argc > 1) {
        needle = argv[1];
    }
    if (argc > 2) {
        ms = _wtoi(argv[2]);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) ||
        !en) {
        wprintf(L"no enumerator\n");
        return 1;
    }

    std::wstring devName;
    IMMDevice* dev = FindCapture(en, needle, &devName);
    if (!dev) {
        wprintf(L"capture endpoint not found (needle='%ls')\n", needle.c_str());
        en->Release();
        return 1;
    }
    wprintf(L"capture device: '%ls'\n", devName.c_str());

    IAudioClient* client = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&client)) ||
        !client) {
        wprintf(L"IAudioClient activate failed\n");
        dev->Release();
        en->Release();
        return 1;
    }

    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(client->GetMixFormat(&fmt)) || !fmt) {
        wprintf(L"GetMixFormat failed\n");
        client->Release();
        dev->Release();
        en->Release();
        return 1;
    }
    wprintf(L"format: %u Hz, %u ch, %u bits\n", fmt->nSamplesPerSec,
            fmt->nChannels, fmt->wBitsPerSample);

    // 共享模式 + 事件回调，采集 N 毫秒。
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, fmt,
                                  nullptr))) {
        // 采集端点用 LOOPBACK 会失败，退回普通采集
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0,
                                      fmt, nullptr))) {
            wprintf(L"Initialize failed\n");
            CoTaskMemFree(fmt);
            client->Release();
            dev->Release();
            en->Release();
            return 1;
        }
    }
    CoTaskMemFree(fmt);

    IAudioCaptureClient* cap = nullptr;
    if (FAILED(client->GetService(IID_PPV_ARGS(&cap))) || !cap) {
        wprintf(L"GetService(IAudioCaptureClient) failed\n");
        client->Release();
        dev->Release();
        en->Release();
        return 1;
    }

    if (FAILED(client->Start())) {
        wprintf(L"Start failed\n");
        cap->Release();
        client->Release();
        dev->Release();
        en->Release();
        return 1;
    }

    float peak = 0.0f;
    double sumSq = 0.0;
    UINT64 totalFrames = 0;
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(ms > 0 ? ms : 1);
    while (GetTickCount() < deadline) {
        Sleep(50);
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        while (SUCCEEDED(cap->GetBuffer(&data, &frames, &flags, nullptr,
                                        nullptr)) &&
               frames > 0) {
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
                const float* f = reinterpret_cast<const float*>(data);
                // 混合格式通常是 32-bit float
                for (UINT32 i = 0; i < frames; ++i) {
                    const float v = f[i];
                    peak = (std::max)(peak, std::fabs(v));
                    sumSq += static_cast<double>(v) * v;
                }
                totalFrames += frames;
            }
            cap->ReleaseBuffer(frames);
        }
    }

    const double rms =
        totalFrames > 0 ? std::sqrt(sumSq / static_cast<double>(totalFrames)) : 0.0;
    wprintf(L"\n==== result ====\n");
    wprintf(L"device  : %ls\n", devName.c_str());
    wprintf(L"frames  : %llu\n", static_cast<unsigned long long>(totalFrames));
    wprintf(L"peak    : %.4f\n", peak);
    wprintf(L"rms     : %.4f\n", rms);
    wprintf(L"%ls\n", (peak > 0.001 || rms > 0.001) ? L"=> 有信号（听到声音）"
                                                    : L"=> 静音（没有信号）");

    client->Stop();
    cap->Release();
    client->Release();
    dev->Release();
    en->Release();
    CoUninitialize();
    return 0;
}

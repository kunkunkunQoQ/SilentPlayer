// Diagnostic probe: figure out how to make the MF source resolver honour the
// real container type instead of the (possibly wrong) file extension.
//
// Tries several documented mechanisms and reports which one actually resolves
// the file, plus what streams the resulting media source exposes.
//
// usage: resolve_probe.exe <file>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Sniff {
    const char* label;
    const wchar_t* mime;
};

bool SniffMime(const std::wstring& path, std::wstring* mime, const char** label) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    unsigned char b[64] = {};
    DWORD got = 0;
    ReadFile(h, b, sizeof(b), &got, nullptr);
    CloseHandle(h);
    if (got < 12) {
        return false;
    }
    auto at = [&](const char* s, int n, int off) {
        return off + n <= static_cast<int>(got) &&
               memcmp(b + off, s, n) == 0;
    };
    if (at("ftyp", 4, 4)) { *mime = L"video/mp4"; *label = "ftyp -> video/mp4"; return true; }
    if (b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) {
        *mime = L"audio/x-matroska"; *label = "matroska"; return true;
    }
    if ((at("RIFF", 4, 0) || at("RF64", 4, 0)) && at("WAVE", 4, 8)) {
        *mime = L"audio/wav"; *label = "RIFF/WAVE -> audio/wav"; return true;
    }
    if (at("fLaC", 4, 0)) { *mime = L"audio/flac"; *label = "fLaC -> audio/flac"; return true; }
    static const unsigned char kAsf[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF,
                                           0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    if (got >= 16 && memcmp(b, kAsf, 16) == 0) {
        *mime = L"video/x-ms-asf"; *label = "ASF GUID -> video/x-ms-asf"; return true;
    }
    if (b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) {
        *mime = L"audio/vnd.dlna.adts"; *label = "ADTS -> audio/vnd.dlna.adts"; return true;
    }
    if (at("ID3", 3, 0)) { *mime = L"audio/mpeg"; *label = "ID3 -> audio/mpeg"; return true; }
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) {
        *mime = L"audio/mpeg"; *label = "MPEG sync -> audio/mpeg"; return true;
    }
    if (b[0] == 0x0B && b[1] == 0x77) { *mime = L"audio/eac3"; *label = "AC3"; return true; }
    return false;
}

void DescribeSource(IMFMediaSource* src, const wchar_t* tag) {
    if (!src) {
        wprintf(L"    [%ls] no source\n", tag);
        return;
    }
    ComPtr<IMFPresentationDescriptor> pd;
    if (FAILED(src->CreatePresentationDescriptor(&pd))) {
        wprintf(L"    [%ls] CreatePresentationDescriptor FAILED\n", tag);
        return;
    }
    UINT64 dur = 0;
    pd->GetUINT64(MF_PD_DURATION, &dur);
    DWORD n = 0;
    pd->GetStreamDescriptorCount(&n);
    wprintf(L"    [%ls] streams=%lu duration=%.2fs\n", tag, n,
            dur / 10000000.0);
    for (DWORD i = 0; i < n; ++i) {
        BOOL sel = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &sel, &sd))) {
            continue;
        }
        ComPtr<IMFMediaTypeHandler> mh;
        GUID major = GUID_NULL;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&mh))) {
            mh->GetMajorType(&major);
        }
        const wchar_t* kind = L"?";
        if (major == MFMediaType_Audio) kind = L"audio";
        else if (major == MFMediaType_Video) kind = L"video";
        wprintf(L"      stream[%lu] %ls selected=%d\n", i, kind, sel ? 1 : 0);
    }
}

bool TryUrl(IMFSourceResolver* r, const std::wstring& path, IPropertyStore* props,
            IMFMediaSource** out) {
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> unk;
    const HRESULT hr = r->CreateObjectFromURL(path.c_str(),
                                              MF_RESOLUTION_MEDIASOURCE, props,
                                              &type, &unk);
    if (FAILED(hr) || type != MF_OBJECT_MEDIASOURCE) {
        return false;
    }
    return SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(out)));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: resolve_probe.exe <file>\n");
        return 1;
    }
    const std::wstring path = argv[1];
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    wprintf(L"file = %ls\n", path.c_str());

    std::wstring mime;
    const char* label = "";
    const bool sniffed = SniffMime(path, &mime, &label);
    wprintf(L"sniff = %hs (mime=%ls)\n", sniffed ? label : "not recognised",
            sniffed ? mime.c_str() : L"-");

    ComPtr<IMFSourceResolver> resolver;
    MFCreateSourceResolver(&resolver);

    // --- 1) 基线：按 URL（扩展名）解析 ---
    {
        ComPtr<IMFMediaSource> src;
        const bool ok = TryUrl(resolver.Get(), path, nullptr, &src);
        wprintf(L"\n[1] CreateObjectFromURL (extension-based): %ls\n",
                ok ? L"OK" : L"FAILED");
        if (ok) DescribeSource(src.Get(), L"url");
    }

    // --- 2) 字节流 + IMFAttributes 上的 MIME ---
    if (sniffed) {
        ComPtr<IMFByteStream> bs;
        HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                                  MF_FILEFLAGS_NONE, path.c_str(), &bs);
        wprintf(L"\n[2] byte stream route\n");
        wprintf(L"    MFCreateFile hr=0x%08lX\n", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            ComPtr<IMFAttributes> attrs;
            const HRESULT hrQi = bs.As(&attrs);
            wprintf(L"    QI IMFAttributes hr=0x%08lX %ls\n", (unsigned long)hrQi,
                    SUCCEEDED(hrQi) ? L"(supported)" : L"(NOT supported)");
            if (SUCCEEDED(hrQi)) {
                attrs->SetString(MF_BYTESTREAM_CONTENT_TYPE, mime.c_str());
            }
            MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
            ComPtr<IUnknown> unk;
            const HRESULT hr2 = resolver->CreateObjectFromByteStream(
                bs.Get(), nullptr, MF_RESOLUTION_MEDIASOURCE, nullptr, &type, &unk);
            const bool ok2 = SUCCEEDED(hr2) && type == MF_OBJECT_MEDIASOURCE;
            wprintf(L"    CreateObjectFromByteStream(url=nullptr) hr=0x%08lX -> %ls\n",
                    (unsigned long)hr2, ok2 ? L"OK" : L"FAILED");
            if (ok2) {
                ComPtr<IMFMediaSource> src;
                if (SUCCEEDED(unk.As(&src))) {
                    DescribeSource(src.Get(), L"bytestream");
                }
            }
        }
    }

    // --- 3) URL + 属性存储里的 MIME ---
    if (sniffed) {
        ComPtr<IPropertyStore> props;
        const HRESULT hrPs = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&props));
        wprintf(L"\n[3] CreateObjectFromURL + property store(MIME)\n");
        wprintf(L"    PSCreateMemoryPropertyStore hr=0x%08lX\n", (unsigned long)hrPs);
        if (SUCCEEDED(hrPs)) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<wchar_t*>(mime.c_str());
            // MF 的属性是 GUID；IPropertyStore 需要 PROPERTYKEY，pid 用 0。
            PROPERTYKEY key;
            key.fmtid = MF_BYTESTREAM_CONTENT_TYPE;
            key.pid = 0;
            props->SetValue(key, pv);
            ComPtr<IMFMediaSource> src;
            const bool ok3 = TryUrl(resolver.Get(), path, props.Get(), &src);
            wprintf(L"    -> %ls\n", ok3 ? L"OK" : L"FAILED");
            if (ok3) {
                DescribeSource(src.Get(), L"props");
            }
        }
    }

    MFShutdown();
    CoUninitialize();
    return 0;
}

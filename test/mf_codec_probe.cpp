// Diagnostic probe: enumerate the Media Foundation audio decoders and encoders
// available on this machine, and print the media subtypes they accept/produce.
//
// This is the authoritative answer to "which formats can SilentPlayer play
// natively, without any third-party library".
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

struct GuidName {
    const GUID* guid;
    const wchar_t* name;
};

const GuidName kAudioSubtypes[] = {
    {&MFMediaType_Audio, L"(major:audio)"},
    {&MFAudioFormat_PCM, L"PCM"},
    {&MFAudioFormat_Float, L"IEEE-Float"},
    {&MFAudioFormat_LPCM, L"LPCM"},
    {&MFAudioFormat_MP3, L"MP3"},
    {&MFAudioFormat_MPEG, L"MPEG"},
    {&MFAudioFormat_AAC, L"AAC"},
    {&MFAudioFormat_ADTS, L"ADTS(AAC)"},
    {&MFAudioFormat_FLAC, L"FLAC"},
    {&MFAudioFormat_ALAC, L"ALAC"},
    {&MFAudioFormat_Opus, L"Opus"},
    {&MFAudioFormat_Vorbis, L"Vorbis"},
    {&MFAudioFormat_AMR_NB, L"AMR-NB"},
    {&MFAudioFormat_AMR_WB, L"AMR-WB"},
    {&MFAudioFormat_WMAudioV8, L"WMA v8"},
    {&MFAudioFormat_WMAudioV9, L"WMA v9"},
    {&MFAudioFormat_WMAudio_Lossless, L"WMA Lossless"},
    {&MFAudioFormat_WMASPDIF, L"WMA SPDIF"},
    {&MFAudioFormat_Dolby_AC3, L"Dolby AC3"},
    {&MFAudioFormat_Dolby_AC3_SPDIF, L"AC3 SPDIF"},
    {&MFAudioFormat_Dolby_DDPlus, L"Dolby DD+"},
    {&MFAudioFormat_DTS, L"DTS"},
};

const wchar_t* NameOf(const GUID& g) {
    for (const auto& e : kAudioSubtypes) {
        if (IsEqualGUID(*e.guid, g)) {
            return e.name;
        }
    }
    return L"?";
}

void Dump(const wchar_t* title, const GUID& category, const GUID& majorType) {
    struct Combo {
        const wchar_t* label;
        UINT32 flags;
        bool filterByType;
    };
    const Combo combos[] = {
        {L"ALL|SORTANDFILTER, input=audio", MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER, true},
        {L"ALL|SORTANDFILTER, no filter", MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER, false},
        {L"ALL, no filter", MFT_ENUM_FLAG_ALL, false},
        {L"SYNCMFT|SORTANDFILTER, no filter", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false},
        {L"0 (default), no filter", 0, false},
    };
    for (const Combo& c : combos) {
        MFT_REGISTER_TYPE_INFO info{majorType, GUID_NULL};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(category, c.flags, nullptr,
                               c.filterByType ? &info : nullptr, &acts, &count);
        wprintf(L"\n=== %ls [%ls] hr=0x%08lX count=%u ===\n", title, c.label,
                static_cast<unsigned long>(hr), count);
        if (FAILED(hr) || count == 0) {
            if (acts) {
                CoTaskMemFree(acts);
            }
            continue;
        }
        for (UINT32 i = 0; i < count; ++i) {
            LPWSTR name = nullptr;
            UINT32 nameLen = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                      &name, &nameLen))) {
                wprintf(L"[%2u] %ls\n", i, name);
                CoTaskMemFree(name);
            }
            IMFTransform* pMft = nullptr;
            if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&pMft)))) {
                for (DWORD s = 0;; ++s) {
                    IMFMediaType* pType = nullptr;
                    if (FAILED(pMft->GetInputAvailableType(0, s, &pType))) {
                        break;
                    }
                    GUID sub = GUID_NULL;
                    pType->GetGUID(MF_MT_SUBTYPE, &sub);
                    wprintf(L"       in : %ls\n", NameOf(sub));
                    pType->Release();
                }
                for (DWORD s = 0;; ++s) {
                    IMFMediaType* pType = nullptr;
                    if (FAILED(pMft->GetOutputAvailableType(0, s, &pType))) {
                        break;
                    }
                    GUID sub = GUID_NULL;
                    pType->GetGUID(MF_MT_SUBTYPE, &sub);
                    wprintf(L"       out: %ls\n", NameOf(sub));
                    pType->Release();
                }
                pMft->Release();
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
}

} // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        wprintf(L"MFStartup failed\n");
        return 1;
    }
    Dump(L"AUDIO DECODER", MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio);
    Dump(L"AUDIO ENCODER", MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio);
    MFShutdown();
    CoUninitialize();
    return 0;
}

#include "AudioDevices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

// 名字里出现这些词，认为多半是"虚拟音频设备"（由驱动/软件提供，而非真实硬件）。
// 只是启发式：仅用于给用户排序和提示，检测结果里会原样列出设备名让用户自行判断。
const wchar_t* const kVirtualWords[] = {
    L"virtual",      L"虚拟",        L"sonar",       L"streaming",
    L"vb-audio",     L"cable",       L"voicemeeter", L"droidcam",
    L"voicemod",     L"nvidia broadcast",            L"loopback",
    L"virtual desktop",
};

// 名字里出现这些词，认为它是"回环/监听"类设备（能把系统播放的声音当麦克风用）。
const wchar_t* const kLoopbackWords[] = {
    L"stereo mix", L"立体声混音", L"what u hear", L"loopback", L"监听",
};

// 名字里出现这些词，认为它偏向"麦克风/采集侧"（虚拟驱动常把同一个名字
// 同时暴露在渲染与采集两侧，列成输出目标会误导用户）。
const wchar_t* const kMicSideWords[] = {
    L"microphone", L"麦克风", L"mic ", L"input",
};

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return s;
}

bool ContainsAny(std::wstring lower, const wchar_t* const* words, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (lower.find(words[i]) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool LooksMicSide(const std::wstring& name) {
    return ContainsAny(ToLower(name), kMicSideWords, std::size(kMicSideWords));
}

// 取"设备家族"标识：优先用名字里最后一对括号内的内容
// （例如 "麦克风 (Steam Streaming Microphone)" -> "Steam Streaming Microphone"），
// 没有括号就用整名。用于判断某个麦克风和某个输出设备是否来自同一个驱动/软件。
std::wstring FamilyKey(const std::wstring& name) {
    const size_t close = name.rfind(L')');
    if (close != std::wstring::npos) {
        const size_t open = name.rfind(L'(', close);
        if (open != std::wstring::npos && close > open + 1) {
            return ToLower(name.substr(open + 1, close - open - 1));
        }
    }
    return ToLower(name);
}

// 挑一个最可能"能用"的虚拟麦克风：优先与虚拟输出设备同家族的那个
// （说明同一个软件同时提供输出侧和麦克风侧，配对概率最高）。
std::wstring PickBestVirtualMic(
    const std::vector<AudioEndpointInfo>& captures,
    const std::vector<AudioEndpointInfo>& renders) {
    std::wstring best;
    size_t bestScore = 0;
    for (const auto& c : captures) {
        if (!c.likelyVirtual) {
            continue;
        }
        const std::wstring key = FamilyKey(c.name);
        size_t score = 0;
        for (const auto& r : renders) {
            if (r.likelyVirtual && FamilyKey(r.name) == key) {
                ++score;
            }
        }
        if (score > bestScore) {
            bestScore = score;
            best = c.name;
        }
    }
    if (best.empty()) {
        for (const auto& c : captures) {
            if (c.likelyVirtual) {
                return c.name;
            }
        }
    }
    return best;
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
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
    return name;
}

std::vector<AudioEndpointInfo> Enumerate(EDataFlow flow) {
    std::vector<AudioEndpointInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              &collection))) {
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        AudioEndpointInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }
        info.name = FriendlyName(device.Get());
        const std::wstring lower = ToLower(info.name);
        info.likelyVirtual =
            ContainsAny(lower, kVirtualWords, std::size(kVirtualWords));
        info.likelyLoopback =
            ContainsAny(lower, kLoopbackWords, std::size(kLoopbackWords));
        out.push_back(std::move(info));
    }
    // 虚拟设备排前面，方便用户找。
    std::stable_sort(out.begin(), out.end(),
                     [](const AudioEndpointInfo& a, const AudioEndpointInfo& b) {
                         return a.likelyVirtual > b.likelyVirtual;
                     });
    return out;
}

// labelRenderSide = true 时给设备标「输出侧 / 麦克风侧」——只对"输出设备"列表用；
// 麦克风列表本身就是让用户选麦克风的，不需要这个标注。
void AppendList(std::wstring* text, const std::vector<AudioEndpointInfo>& items,
                bool onlyVirtual, bool labelRenderSide) {
    int shown = 0;
    for (const auto& item : items) {
        if (onlyVirtual && !item.likelyVirtual) {
            continue;
        }
        text->append(L"    · ");
        text->append(item.name.empty() ? L"(未知设备)" : item.name);
        if (item.likelyLoopback) {
            text->append(L"   [回环/监听]");
        } else if (labelRenderSide) {
            text->append(LooksMicSide(item.name) ? L"   [麦克风侧，一般不作输出]"
                                                 : L"   [输出侧]");
        }
        text->append(L"\r\n");
        ++shown;
    }
    if (shown == 0) {
        text->append(L"    （无）\r\n");
    }
}

} // namespace

std::vector<AudioEndpointInfo> AudioDevices::RenderEndpoints() {
    return Enumerate(eRender);
}

std::vector<AudioEndpointInfo> AudioDevices::CaptureEndpoints() {
    return Enumerate(eCapture);
}

std::wstring AudioDevices::DefaultRenderName() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return {};
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return {};
    }
    return FriendlyName(device.Get());
}

std::wstring AudioDevices::BuildMicRoutingAdvice(
    const std::wstring& activeOutputName) {
    const auto renders = RenderEndpoints();
    const auto captures = CaptureEndpoints();

    int virtualMics = 0;
    int loopbacks = 0;
    for (const auto& c : captures) {
        if (c.likelyVirtual) {
            ++virtualMics;
        }
        if (c.likelyLoopback) {
            ++loopbacks;
        }
    }
    const std::wstring bestMic = PickBestVirtualMic(captures, renders);

    std::wstring t;

    // ===== 普通用户部分：短，只讲"现在什么状态、还要做什么" =====
    t += L"【输出到麦克风】\r\n\r\n";

    {
        t += L"已开启：播放器的声音现在输出到\r\n";
        t += L"    " +
             (activeOutputName.empty() ? std::wstring(L"(虚拟麦克风设备)")
                                       : activeOutputName) +
             L"\r\n\r\n";
        t += L"  所以：语音里的朋友能听到你放的歌；\r\n";
        t += L"        你自己的耳机里暂时听不到——取消勾选即可切回原设备继续听。\r\n\r\n";
        t += L"还需要在语音软件里做一步（只做一次）：\r\n";
        t += L"  在语音软件（微信 / Discord / OBS 等）里，把「麦克风」选成：\r\n";
        t += L"      " +
             (bestMic.empty() ? std::wstring(L"(见下方列表)") : bestMic) +
             L"\r\n";
    }

    // ===== 技术部分：检测详情 =====
    t += L"\r\n────────── 以下为检测详情（普通用户可忽略） ──────────\r\n\r\n";
    t += L"原理：普通程序无法直接写进麦克风（采集端点只能被读取）。\r\n";
    t += L"但虚拟音频驱动会把同一个设备同时暴露成「渲染侧」和「麦克风侧」，\r\n";
    t += L"把声音渲染到它的渲染侧，就会出现在它的麦克风里——本开关就是这么做的。\r\n";
    t += L"实测：只有渲染到该虚拟麦克风的渲染侧才会进麦克风；\r\n";
    t += L"渲染到 Gaming / Media / Aux / Chat 等其它通道都不会。\r\n\r\n";

    t += L"检测到 " + std::to_wstring(virtualMics) + L" 个虚拟麦克风：\r\n";
    AppendList(&t, captures, true, false);
    if (loopbacks > 0) {
        t += L"    （标 [回环/监听] 的录的是系统正在播放的声音，也能用）\r\n";
    }
    t += L"\r\n";

    t += L"检测到的虚拟输出设备：\r\n";
    AppendList(&t, renders, true, true);
    t += L"\r\n";

    t += L"当前系统默认播放设备：" +
         (DefaultRenderName().empty() ? std::wstring(L"(未知)")
                                      : DefaultRenderName()) +
         L"\r\n";
    if (!bestMic.empty()) {
        t += L"建议使用的虚拟麦克风：" + bestMic + L"\r\n";
    }
    return t;
}

std::wstring AudioDevices::RecommendedMicName() {
    return PickBestVirtualMic(CaptureEndpoints(), RenderEndpoints());
}

bool AudioDevices::FindMicFeedEndpoint(const std::wstring& micName,
                                       std::wstring* id, std::wstring* name) {
    if (micName.empty() || !id || !name) {
        return false;
    }
    const std::wstring want = ToLower(micName);
    const std::wstring wantFamily = FamilyKey(micName);
    const auto renders = RenderEndpoints();

    // 1) 名字完全一致（虚拟驱动两侧同名，实测 Sonar 就是这种情况）
    for (const auto& r : renders) {
        if (ToLower(r.name) == want) {
            *id = r.id;
            *name = r.name;
            return true;
        }
    }
    // 2) 同家族 + 名字偏麦克风侧
    for (const auto& r : renders) {
        if (FamilyKey(r.name) == wantFamily && LooksMicSide(r.name)) {
            *id = r.id;
            *name = r.name;
            return true;
        }
    }
    return false;
}

#include "PerAppAudio.h"

#include <inspectable.h>
#include <mmdeviceapi.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/wrappers/corewrappers.h>

#pragma comment(lib, "runtimeobject.lib")

using Microsoft::WRL::Wrappers::HStringReference;

namespace {

// 未公开接口 IAudioPolicyConfigFactory（Win11 21H2 及以上的变体）。
//
// IID 与 vtable 布局与 EarTrumpet 的实现一致（其源码中
// IAudioPolicyConfigFactoryVariantFor21H2 声明为 InterfaceIsIInspectable）：
//   slot 0..5   : IInspectable（QueryInterface/AddRef/Release/GetIids/
//                 GetRuntimeClassName/GetTrustLevel）
//   slot 6..24  : 19 个与音量组/铃声/聊天上下文相关的方法（不调用，仅占位对齐）
//   slot 25     : SetPersistedDefaultAudioEndpoint
//   slot 26     : GetPersistedDefaultAudioEndpoint
//   slot 27     : ClearAllPersistedApplicationDefaultEndpoints（Win11 22H2+）
// 占位方法必须保留，否则 vtable 对不齐、会调到错误的方法上。
MIDL_INTERFACE("ab3d4648-e242-459f-b02f-541c70306324")
IAudioPolicyConfigFactory21H2 : public IInspectable {
public:
    virtual HRESULT STDMETHODCALLTYPE Stub01() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub02() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub03() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub04() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub05() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub06() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub07() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub08() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub09() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub18() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stub19() = 0;

    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        UINT32 processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        UINT32 processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// 设备 ID 需要包成"设备接口路径"形式，否则接口会返回 E_INVALIDARG。
// 形如：\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{hash}#{e6327cad-dcec-4949-ae8a-991e976a79d2}
constexpr wchar_t kMmdevapiToken[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kRenderInterface[] =
    L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
constexpr wchar_t kActivationName[] = L"Windows.Media.Internal.AudioPolicyConfig";

// 获取激活工厂；失败返回 nullptr（老系统或接口被移除时）。
IAudioPolicyConfigFactory21H2* AcquireFactory() {
    const HStringReference name(kActivationName);
    IAudioPolicyConfigFactory21H2* factory = nullptr;
    const HRESULT hr = RoGetActivationFactory(
        name.Get(), __uuidof(IAudioPolicyConfigFactory21H2),
        reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        return nullptr;
    }
    return factory;
}

// 对 eConsole 与 eMultimedia 两个角色都设置（与 EarTrumpet 一致）。
HRESULT ApplyForRoles(IAudioPolicyConfigFactory21H2* factory, DWORD pid,
                      HSTRING deviceId) {
    HRESULT last = E_FAIL;
    const ERole roles[] = {eConsole, eMultimedia};
    for (const ERole role : roles) {
        const HRESULT hr =
            factory->SetPersistedDefaultAudioEndpoint(pid, eRender, role, deviceId);
        if (FAILED(hr)) {
            last = hr;
        } else {
            last = S_OK;
        }
    }
    return last;
}

} // namespace

bool PerAppAudio::SetProcessOutputDevice(DWORD pid,
                                         const std::wstring& shortDeviceId) {
    if (shortDeviceId.empty()) {
        return false;
    }
    IAudioPolicyConfigFactory21H2* factory = AcquireFactory();
    if (!factory) {
        return false;
    }
    const std::wstring full =
        std::wstring(kMmdevapiToken) + shortDeviceId + kRenderInterface;

    HSTRING hstring = nullptr;
    if (FAILED(WindowsCreateString(full.c_str(),
                                   static_cast<UINT32>(full.size()), &hstring)) ||
        !hstring) {
        factory->Release();
        return false;
    }

    const HRESULT hr = ApplyForRoles(factory, pid, hstring);
    WindowsDeleteString(hstring);
    factory->Release();
    return SUCCEEDED(hr);
}

bool PerAppAudio::ClearProcessOutputDevice(DWORD pid) {
    IAudioPolicyConfigFactory21H2* factory = AcquireFactory();
    if (!factory) {
        return false;
    }
    // 传 null HSTRING = 清除该应用的设置，回到系统默认设备。
    const HRESULT hr = ApplyForRoles(factory, pid, nullptr);
    factory->Release();
    return SUCCEEDED(hr);
}

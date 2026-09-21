// Diagnostic probe: repeat a full Media Foundation load/release cycle in-process
// and report memory each round. Isolates the MF usage pattern from the app's
// UI / single-instance / IPC layers, so a leak can be attributed precisely.
//
// usage: mf_load_cycle_probe.exe <file> <cycles> [--no-clear-current]
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <psapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

// Minimal event drain, mirroring what the app does (EndGetEvent + re-BeginGetEvent).
// NOTE: the session pointer is atomic and is cleared before the session is torn
// down -- otherwise a callback that is still in flight would touch a freed
// session and crash (this was the cause of the old --mode=restart segfault).
class DrainCallback : public IMFAsyncCallback {
public:
    std::atomic<ULONG> ref{1};
    std::atomic<long> drained{0};
    std::atomic<IMFMediaSession*> session{nullptr};
    HANDLE hStopped = nullptr;   // signalled on MESessionStopped / MESessionEnded
    HANDLE hCleared = nullptr;   // signalled on MESessionTopologiesCleared

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --ref;
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
        if (flags) *flags = 0;
        if (queue) *queue = MFASYNC_CALLBACK_QUEUE_STANDARD;
        return S_OK;
    }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        IMFMediaSession* s = session.load();
        if (!s) {
            return S_OK; // 会话已拆除，直接返回，不要再碰它
        }
        ComPtr<IMFMediaEvent> ev;
        if (SUCCEEDED(s->EndGetEvent(result, &ev))) {
            ++drained;
            MediaEventType type = MEUnknown;
            if (ev && SUCCEEDED(ev->GetType(&type))) {
                if (type == MESessionStopped || type == MESessionEnded) {
                    if (hStopped) SetEvent(hStopped);
                } else if (type == MESessionTopologiesCleared) {
                    if (hCleared) SetEvent(hCleared);
                }
            }
        }
        s = session.load();
        if (s) {
            s->BeginGetEvent(this, nullptr);
        }
        return S_OK;
    }
};

double PrivateMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) {
        return -1.0;
    }
    return pmc.PrivateUsage / 1048576.0;
}

double WorkingSetMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) {
        return -1.0;
    }
    return pmc.WorkingSetSize / 1048576.0;
}

DWORD HandleCount() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

HRESULT Resolve(const std::wstring& path, IMFMediaSource** ppSource) {
    ComPtr<IMFSourceResolver> pResolver;
    HRESULT hr = MFCreateSourceResolver(&pResolver);
    if (FAILED(hr)) {
        return hr;
    }
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> pUnk;
    hr = pResolver->CreateObjectFromURL(path.c_str(), MF_RESOLUTION_MEDIASOURCE,
                                        nullptr, &type, &pUnk);
    if (FAILED(hr)) {
        return hr;
    }
    if (type != MF_OBJECT_MEDIASOURCE) {
        return MF_E_INVALIDMEDIATYPE;
    }
    return pUnk->QueryInterface(IID_PPV_ARGS(ppSource));
}

HRESULT BuildTopology(IMFMediaSource* pSource, IMFTopology** ppTopology) {
    ComPtr<IMFPresentationDescriptor> pPD;
    HRESULT hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) {
        return hr;
    }
    DWORD streamCount = 0;
    pPD->GetStreamDescriptorCount(&streamCount);
    DWORD audioIndex = 0;
    bool found = false;
    for (DWORD i = 0; i < streamCount; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> pSD;
        if (FAILED(pPD->GetStreamDescriptorByIndex(i, &selected, &pSD))) {
            continue;
        }
        ComPtr<IMFMediaTypeHandler> pHandler;
        if (FAILED(pSD->GetMediaTypeHandler(&pHandler))) {
            continue;
        }
        GUID major = GUID_NULL;
        pHandler->GetMajorType(&major);
        if (major == MFMediaType_Audio) {
            audioIndex = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return MF_E_INVALIDMEDIATYPE;
    }
    for (DWORD i = 0; i < streamCount; ++i) {
        if (i == audioIndex) {
            pPD->SelectStream(i);
        } else {
            pPD->DeselectStream(i);
        }
    }
    ComPtr<IMFStreamDescriptor> pStreamDesc;
    BOOL sel = FALSE;
    hr = pPD->GetStreamDescriptorByIndex(audioIndex, &sel, &pStreamDesc);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFTopologyNode> pSourceNode;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSourceNode);
    if (FAILED(hr)) {
        return hr;
    }
    pSourceNode->SetUnknown(MF_TOPONODE_SOURCE, pSource);
    pSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD.Get());
    pSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pStreamDesc.Get());
    pSourceNode->SetUINT32(MF_TOPONODE_STREAMID, audioIndex);

    ComPtr<IMFActivate> pRenderer;
    hr = MFCreateAudioRendererActivate(&pRenderer);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFTopologyNode> pOutNode;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutNode);
    if (FAILED(hr)) {
        return hr;
    }
    pOutNode->SetObject(pRenderer.Get());
    hr = pSourceNode->ConnectOutput(0, pOutNode.Get(), 0);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFTopology> pTopo;
    hr = MFCreateTopology(&pTopo);
    if (FAILED(hr)) {
        return hr;
    }
    pTopo->AddNode(pSourceNode.Get());
    pTopo->AddNode(pOutNode.Get());
    *ppTopology = pTopo.Detach();
    return S_OK;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: mf_load_cycle_probe.exe <file> <cycles> "
                L"[--mode=resolve|topology|session] [--no-clear-current]\n");
        return 1;
    }
    const std::wstring file = argv[1];
    const int cycles = _wtoi(argv[2]);
    bool clearCurrent = true;
    bool drain = false;
    bool waitEvents = false;
    std::wstring mode = L"session";
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--no-clear-current") == 0) {
            clearCurrent = false;
        } else if (wcscmp(argv[i], L"--drain") == 0) {
            drain = true;
        } else if (wcscmp(argv[i], L"--wait") == 0) {
            drain = true;
            waitEvents = true;
        } else if (wcsncmp(argv[i], L"--mode=", 7) == 0) {
            mode = argv[i] + 7;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    setvbuf(stdout, nullptr, _IONBF, 0); // 崩溃时也能看到已经打印的内容
    if (FAILED(MFStartup(MF_VERSION))) {
        wprintf(L"MFStartup failed\n");
        return 1;
    }

    // 只有走会话的模式才需要创建 IMFMediaSession。
    // （旧版只在 mode==session 时创建，restart 模式下 pSession 为空，
    //   pSession->SetTopology 直接空指针解引用 —— 这就是段错误的根因。）
    const bool needsSession = (mode == L"session" || mode == L"restart" ||
                               mode == L"sessiononly");
    ComPtr<IMFMediaSession> pSession;
    if (needsSession && FAILED(MFCreateMediaSession(nullptr, &pSession))) {
        wprintf(L"MFCreateMediaSession failed\n");
        return 1;
    }

    DrainCallback* pDrain = nullptr;
    if (drain && pSession) {
        pDrain = new DrainCallback();
        pDrain->session = pSession.Get();
        if (waitEvents) {
            pDrain->hStopped = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            pDrain->hCleared = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        pSession->BeginGetEvent(pDrain, nullptr);
    }
    wprintf(L"file=%ls  mode=%ls  clearCurrent=%d  drain=%d  wait=%d\n",
            file.c_str(), mode.c_str(), clearCurrent ? 1 : 0, drain ? 1 : 0,
            waitEvents ? 1 : 0);
    wprintf(L"%6s | %9s | %9s | %7s | %s\n", L"cycle", L"WS(MB)", L"PrivateMB",
            L"handles", L"note");
    wprintf(L"-------+-----------+-----------+---------+------\n");
    wprintf(L"%6d | %9.2f | %9.2f | %7lu | start\n", 0, WorkingSetMB(),
            PrivateMB(), HandleCount());

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFTopology> topo; // restart 模式下跨轮次保留
    for (int c = 1; c <= cycles; ++c) {
        wchar_t note[128] = L"";

        if (mode == L"sessiononly") {
            // 只做「创建会话 / 销毁会话」，不加载任何媒体：
            // 用于判断句柄增长是不是会话重建本身带来的。
            if (pSession) {
                pSession->Shutdown();
                pSession.Reset();
            }
            ComPtr<IMFMediaSession> pNew;
            const HRESULT hrNew = MFCreateMediaSession(nullptr, &pNew);
            pSession = pNew;
            Sleep(40);
            wprintf(L"%6d | %9.2f | %9.2f | %7lu | session create/destroy hr=0x%08lX\n",
                    c, WorkingSetMB(), PrivateMB(), HandleCount(),
                    static_cast<unsigned long>(hrNew));
            continue;
        }

        if (mode == L"session") {
            if (waitEvents && pDrain && pDrain->hStopped) {
                ResetEvent(pDrain->hStopped);
            }
            pSession->Stop();
            if (waitEvents && pDrain && pDrain->hStopped) {
                WaitForSingleObject(pDrain->hStopped, 3000);
            } else {
                Sleep(60);
            }
            if (clearCurrent) {
                if (waitEvents && pDrain && pDrain->hCleared) {
                    ResetEvent(pDrain->hCleared);
                }
                const HRESULT hrClear =
                    pSession->SetTopology(MFSESSION_SETTOPOLOGY_CLEAR_CURRENT, nullptr);
                swprintf_s(note, L"clearCurrent hr=0x%08lX",
                           static_cast<unsigned long>(hrClear));
                if (waitEvents && pDrain && pDrain->hCleared) {
                    WaitForSingleObject(pDrain->hCleared, 3000);
                }
            }
            pSession->ClearTopologies();
        }

        if (mode == L"restart" && topo) {
            // 同一拓扑反复 Start/Stop：区分“每次呈现泄漏”还是“每次换拓扑泄漏”。
            PROPVARIANT var;
            PropVariantInit(&var);
            var.vt = VT_I8;
            var.hVal.QuadPart = 0;
            HRESULT hr2 = pSession->Start(&GUID_NULL, &var);
            PropVariantClear(&var);
            if (FAILED(hr2)) {
                wprintf(L"Start failed 0x%08lX\n", static_cast<unsigned long>(hr2));
                break;
            }
            Sleep(300);
            if (waitEvents && pDrain && pDrain->hStopped) {
                ResetEvent(pDrain->hStopped);
            }
            pSession->Stop();
            if (waitEvents && pDrain && pDrain->hStopped) {
                WaitForSingleObject(pDrain->hStopped, 3000);
            } else {
                Sleep(80);
            }
            wprintf(L"%6d | %9.2f | %9.2f | %7lu | restart same topology\n", c,
                    WorkingSetMB(), PrivateMB(), HandleCount());
            continue;
        }

        source.Reset();
        topo.Reset();

        HRESULT hr = Resolve(file, &source);
        if (FAILED(hr)) {
            wprintf(L"resolve failed 0x%08lX\n", static_cast<unsigned long>(hr));
            break;
        }

        if (mode == L"resolve") {
            source.Reset();
            Sleep(60);
            wprintf(L"%6d | %9.2f | %9.2f | %7lu | resolve+release\n", c,
                    WorkingSetMB(), PrivateMB(), HandleCount());
            continue;
        }

        hr = BuildTopology(source.Get(), &topo);
        if (FAILED(hr)) {
            wprintf(L"topology failed 0x%08lX\n", static_cast<unsigned long>(hr));
            break;
        }

        if (mode == L"topology") {
            topo.Reset();
            source.Reset();
            Sleep(60);
            wprintf(L"%6d | %9.2f | %9.2f | %7lu | topology+release\n", c,
                    WorkingSetMB(), PrivateMB(), HandleCount());
            continue;
        }

        hr = pSession->SetTopology(MFSESSION_SETTOPOLOGY_IMMEDIATE, topo.Get());
        if (FAILED(hr)) {
            wprintf(L"SetTopology failed 0x%08lX\n", static_cast<unsigned long>(hr));
            break;
        }
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = 0;
        hr = pSession->Start(&GUID_NULL, &var);
        PropVariantClear(&var);
        if (FAILED(hr)) {
            wprintf(L"Start failed 0x%08lX\n", static_cast<unsigned long>(hr));
            break;
        }
        Sleep(300); // 播放一小段

        wprintf(L"%6d | %9.2f | %9.2f | %7lu | %ls\n", c, WorkingSetMB(), PrivateMB(),
                HandleCount(), note);
    }

    // 收尾
    if (needsSession) {
        pSession->Stop();
        Sleep(100);
        if (clearCurrent) {
            pSession->SetTopology(MFSESSION_SETTOPOLOGY_CLEAR_CURRENT, nullptr);
        }
        pSession->ClearTopologies();
    }
    source.Reset();
    // 先断开回调里的会话指针，再拆会话：否则在途的 Invoke 会访问已释放的会话
    // （这正是旧版 --mode=restart 段错误的原因）。回调对象本身不释放，进程退出时回收。
    if (pDrain) {
        pDrain->session = nullptr;
    }
    if (pSession) {
        pSession->Shutdown();
        Sleep(50);
        pSession.Reset();
    }
    Sleep(50);
    wprintf(L"%6s | %9.2f | %9.2f | %7lu | after shutdown\n", L"end",
            WorkingSetMB(), PrivateMB(), HandleCount());

    MFShutdown();
    CoUninitialize();
    return 0;
}

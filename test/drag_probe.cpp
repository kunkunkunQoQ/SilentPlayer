// Diagnostic probe: can a real OLE drag-drop be driven programmatically, and
// which receive mechanism actually fires?
//
//   sink A: WS_EX_ACCEPTFILES only  -> 期待 WM_DROPFILES（shell 兼容通道）
//   sink B: RegisterDragDrop 注册的 IDropTarget
//
// usage: drag_probe.exe <file>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objidl.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

HWND g_sinkA = nullptr; // WS_EX_ACCEPTFILES
HWND g_sinkB = nullptr; // IDropTarget
int g_aCount = 0;
int g_bCount = 0;

LRESULT CALLBACK SinkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DROPFILES) {
        ++g_aCount;
        HDROP hDrop = reinterpret_cast<HDROP>(wp);
        const UINT len = DragQueryFileW(hDrop, 0, nullptr, 0);
        wchar_t buf[MAX_PATH] = {};
        if (len > 0 && len < MAX_PATH) {
            DragQueryFileW(hDrop, 0, buf, MAX_PATH);
        }
        wprintf(L"[sink A] WM_DROPFILES  ->  path='%ls'\n", buf);
        DragFinish(hDrop);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

class SinkTarget : public IDropTarget {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP DragEnter(IDataObject* pdo, DWORD, POINTL, DWORD* effect) override {
        wprintf(L"[sink B] DragEnter (CF_HDROP=%ls)\n",
                HasDrop(pdo) ? L"yes" : L"no");
        if (effect) *effect = HasDrop(pdo) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        wprintf(L"[sink B] DragOver keys=0x%lX pt=(%ld,%ld)\n", keys, pt.x, pt.y);
        if (effect) *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        wprintf(L"[sink B] DragLeave\n");
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* pdo, DWORD, POINTL, DWORD* effect) override {
        ++g_bCount;
        wprintf(L"[sink B] Drop (CF_HDROP=%ls)\n", HasDrop(pdo) ? L"yes" : L"no");
        if (effect) *effect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    static bool HasDrop(IDataObject* pdo) {
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return pdo && pdo->QueryGetData(&fmt) == S_OK;
    }
};

bool MakeDataObject(const std::wstring& file, IDataObject** out) {
    PIDLIST_ABSOLUTE pidlFile = nullptr;
    if (FAILED(SHParseDisplayName(file.c_str(), nullptr, &pidlFile, 0, nullptr)) ||
        !pidlFile) {
        wprintf(L"SHParseDisplayName failed (err=%lu)\n", GetLastError());
        return false;
    }
    // SHCreateDataObject 需要「父目录 pidl + 相对于父目录的子项 pidl」，
    // 传 pidlFolder=nullptr 会当成桌面根，导致数据对象里没有 CF_HDROP。
    PIDLIST_ABSOLUTE pidlFolder = ILClone(pidlFile);
    ILRemoveLastID(pidlFolder);
    PCUITEMID_CHILD child = ILFindLastID(pidlFile);
    const HRESULT hr = SHCreateDataObject(pidlFolder, 1, &child, nullptr,
                                          IID_PPV_ARGS(out));
    CoTaskMemFree(pidlFolder);
    CoTaskMemFree(pidlFile);
    if (FAILED(hr)) {
        wprintf(L"SHCreateDataObject failed 0x%08lX\n", (unsigned long)hr);
        return false;
    }
    // 确认数据对象里确实有 CF_HDROP
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    wprintf(L"data object QueryGetData(CF_HDROP) = %ls\n",
            (*out)->QueryGetData(&fmt) == S_OK ? L"S_OK" : L"NOT available");
    return true;
}

// 最小拖放源：左键松开即投放，Esc 取消。
class SinkSource : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override {
        wprintf(L"    [source] QueryContinueDrag escape=%d keys=0x%lX MK_LBUTTON=%d\n",
                escape ? 1 : 0, keys, (keys & MK_LBUTTON) ? 1 : 0);
        if (escape) {
            return DRAGDROP_S_CANCEL;
        }
        if ((keys & MK_LBUTTON) == 0) {
            wprintf(L"    [source] -> DRAGDROP_S_DROP\n");
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD effect) override {
        wprintf(L"    [source] GiveFeedback effect=%lu\n", effect);
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

// 把光标移到窗口中心并真跑一次 DoDragDrop。
// DoDragDrop 要求鼠标左键处于按下状态，所以这里合成一次真实按键：
// 主线程按住左键并进入 DoDragDrop 的模态循环，辅助线程稍后移动并松开。
struct DragCtx {
    POINT origin{};
    POINT center{};
    HWND target = nullptr;
    IDataObject* pdo = nullptr;
    HRESULT hr = E_FAIL;
    DWORD effect = DROPEFFECT_NONE;
};

DWORD WINAPI ReleaseThread(LPVOID param) {
    auto* ctx = static_cast<DragCtx*>(param);
    Sleep(700);
    // 不移动鼠标（移动可能把光标带出窗口，OLE 会判成 DragLeave），直接松开左键。
    (void)ctx;
    INPUT up = {};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &up, sizeof(INPUT));
    return 0;
}

void TryDrag(HWND target, const wchar_t* label, IDataObject* pdo, bool synthetic) {
    RECT rc{};
    GetWindowRect(target, &rc);
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    wprintf(L"--- DoDragDrop onto %ls (hwnd=%p) at (%d,%d) synthetic=%d ---\n",
            label, target, cx, cy, synthetic ? 1 : 0);

    POINT origin{};
    GetCursorPos(&origin);
    SetForegroundWindow(target);
    SetCursorPos(cx, cy);
    Sleep(250);

    DragCtx ctx;
    ctx.origin = origin;
    ctx.center = POINT{cx, cy};
    ctx.target = target;
    ctx.pdo = pdo;

    HANDLE hThread = nullptr;
    if (synthetic) {
        INPUT down = {};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &down, sizeof(INPUT));
        hThread = CreateThread(nullptr, 0, ReleaseThread, &ctx, 0, nullptr);
    }

    static SinkSource source;
    ctx.hr = DoDragDrop(pdo, &source, DROPEFFECT_COPY, &ctx.effect);

    if (synthetic) {
        if (hThread) {
            WaitForSingleObject(hThread, 3000);
            CloseHandle(hThread);
        }
        // 兜底：无论如何都要把左键松开，避免留下“按住”的状态
        INPUT up = {};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &up, sizeof(INPUT));
    }
    SetCursorPos(origin.x, origin.y);
    wprintf(L"DoDragDrop hr=0x%08lX effect=%lu\n", (unsigned long)ctx.hr,
            ctx.effect);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        wprintf(L"usage: drag_probe.exe <file> [--synthetic]\n");
        return 1;
    }
    std::wstring file = argv[1];
    for (wchar_t& ch : file) {
        if (ch == L'/') {
            ch = L'\\'; // 兼容从 bash 传入时被转成正斜杠的情况
        }
    }
    wprintf(L"input path = '%ls'\n", file.c_str());

    OleInitialize(nullptr);

    // --apponly：不创建任何干扰窗口，只对 SilentPlayer 主窗口做一次合成拖放。
    // 先把窗口挪到固定位置，并校验光标确实落到了窗口中心，排除“光标没落准”的干扰。
    if (argc > 2 && wcscmp(argv[2], L"--apponly") == 0) {
        HWND app = FindWindowW(L"SilentPlayerMainWindow", nullptr);
        if (!app) {
            wprintf(L"SilentPlayer window not found\n");
            return 1;
        }
        SetWindowPos(app, HWND_TOP, 100, 100, 0, 0,
                     SWP_NOSIZE | SWP_SHOWWINDOW);
        Sleep(400);
        RECT rc{};
        GetWindowRect(app, &rc);
        wprintf(L"app window moved to (%ld,%ld)-(%ld,%ld), visible=%d\n", rc.left,
                rc.top, rc.right, rc.bottom, IsWindowVisible(app) ? 1 : 0);

        IDataObject* d = nullptr;
        if (!MakeDataObject(file, &d)) {
            return 1;
        }
        const int cx = (rc.left + rc.right) / 2;
        const int cy = (rc.top + rc.bottom) / 2;
        SetForegroundWindow(app);
        SetCursorPos(cx, cy);
        Sleep(300);
        POINT now{};
        GetCursorPos(&now);
        wprintf(L"cursor target=(%d,%d) actual=(%ld,%ld) %ls\n", cx, cy, now.x, now.y,
                (now.x == cx && now.y == cy) ? L"(OK)" : L"(MISMATCH!)");
        wprintf(L"window under cursor = %p (app=%p)\n", WindowFromPoint(now), app);

        static SinkSource source;
        INPUT down = {};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &down, sizeof(INPUT));
        Sleep(100);
        HANDLE hThread = CreateThread(nullptr, 0, ReleaseThread, nullptr, 0, nullptr);
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT hr = DoDragDrop(d, &source, DROPEFFECT_COPY, &effect);
        if (hThread) {
            WaitForSingleObject(hThread, 3000);
            CloseHandle(hThread);
        }
        INPUT up = {};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &up, sizeof(INPUT));
        wprintf(L"DoDragDrop hr=0x%08lX effect=%lu\n", (unsigned long)hr, effect);
        d->Release();
        Sleep(1200);
        OleUninitialize();
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SinkProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DragProbeSink";
    RegisterClassExW(&wc);

    g_sinkA = CreateWindowExW(0, L"DragProbeSink", L"sinkA", WS_OVERLAPPEDWINDOW,
                              80, 80, 320, 160, nullptr, nullptr, wc.hInstance,
                              nullptr);
    g_sinkB = CreateWindowExW(0, L"DragProbeSink", L"sinkB", WS_OVERLAPPEDWINDOW,
                              440, 80, 320, 160, nullptr, nullptr, wc.hInstance,
                              nullptr);
    if (!g_sinkA || !g_sinkB) {
        wprintf(L"CreateWindow failed\n");
        return 1;
    }
    ShowWindow(g_sinkA, SW_SHOW);
    ShowWindow(g_sinkB, SW_SHOW);
    UpdateWindow(g_sinkA);
    UpdateWindow(g_sinkB);
    DragAcceptFiles(g_sinkA, TRUE);
    static SinkTarget targetB;
    const HRESULT hrReg = RegisterDragDrop(g_sinkB, &targetB);
    wprintf(L"sinkA(hwnd=%p) WS_EX_ACCEPTFILES=%ls\n", g_sinkA,
            (GetWindowLongPtrW(g_sinkA, GWL_EXSTYLE) & WS_EX_ACCEPTFILES)
                ? L"set"
                : L"NOT set");
    wprintf(L"sinkB(hwnd=%p) RegisterDragDrop=0x%08lX\n", g_sinkB,
            (unsigned long)hrReg);

    // 直接对 SilentPlayer 主窗口做一次合成拖放（跨进程，最接近真实场景）
    if (argc > 2 && wcscmp(argv[2], L"--app") == 0) {
        HWND app = FindWindowW(L"SilentPlayerMainWindow", nullptr);
        if (!app) {
            wprintf(L"SilentPlayer window not found\n");
            return 1;
        }
        IDataObject* d = nullptr;
        if (!MakeDataObject(file, &d)) {
            return 1;
        }
        wprintf(L"\n=== synthetic drag onto SilentPlayer window (hwnd=%p) ===\n", app);
        TryDrag(app, L"SilentPlayer", d, true);
        d->Release();
        Sleep(800);
        wprintf(L"done\n");
        RevokeDragDrop(g_sinkB);
        OleUninitialize();
        return 0;
    }

    IDataObject* pdo = nullptr;
    if (!MakeDataObject(file, &pdo)) {
        return 1;
    }
    wprintf(L"data object created for '%ls'\n\n", file.c_str());

    const bool synthetic = (argc > 2 && wcscmp(argv[2], L"--synthetic") == 0);
    TryDrag(g_sinkA, L"sink A (WM_DROPFILES)", pdo, synthetic);
    Sleep(400);
    TryDrag(g_sinkB, L"sink B (IDropTarget)", pdo, synthetic);
    Sleep(400);

    wprintf(L"\nsummary: sinkA WM_DROPFILES=%d   sinkB IDropTarget=%d\n", g_aCount,
            g_bCount);
    if (pdo) {
        pdo->Release();
    }
    RevokeDragDrop(g_sinkB);
    OleUninitialize();
    return 0;
}

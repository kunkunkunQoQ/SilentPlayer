// Diagnostic probe for drag-and-drop.
//
//   drop_probe.exe drop <file>   -> 构造 HDROP 并给 SilentPlayer 主窗口发 WM_DROPFILES，
//                                   用于端到端验证“拖文件到窗口就播放”
//   drop_probe.exe traycheck     -> 检查通知区域（托盘）窗口是否注册了 OLE 投放目标，
//                                   用于说明“拖到托盘图标”在系统层面是否可行
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>   // DROPFILES
#include <objidl.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

// 把一份 DROPFILES 块构造到调用进程的堆上。
std::vector<BYTE> MakeDropBlock(const std::wstring& path) {
    const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    std::vector<BYTE> buf(bytes, 0);
    auto* df = reinterpret_cast<DROPFILES*>(buf.data());
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(buf.data() + sizeof(DROPFILES));
    std::memcpy(dst, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    dst[path.size() + 1] = L'\0'; // 文件名列表以双 null 结尾
    return buf;
}

// 把 DROPFILES 块写进目标进程的地址空间，返回远端地址（0 表示失败）。
// 真实拖放时 shell 也是这样把 HDROP 放进目标进程的；直接跨进程 SendMessage
// 系统不会整理 HDROP，目标进程会读不到文件名。
void* WriteBlockIntoProcess(DWORD pid, const std::vector<BYTE>& block,
                            HANDLE* outProc) {
    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE,
        pid);
    if (!hProc) {
        wprintf(L"OpenProcess(pid=%lu) failed, err=%lu\n", pid, GetLastError());
        return nullptr;
    }
    void* remote = VirtualAllocEx(hProc, nullptr, block.size(), MEM_COMMIT,
                                  PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed, err=%lu\n", GetLastError());
        CloseHandle(hProc);
        return nullptr;
    }
    if (!WriteProcessMemory(hProc, remote, block.data(), block.size(), nullptr)) {
        wprintf(L"WriteProcessMemory failed, err=%lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return nullptr;
    }
    *outProc = hProc;
    return remote;
}

int DoDrop(const std::wstring& file) {
    HWND hwnd = FindWindowW(L"SilentPlayerMainWindow", nullptr);
    if (!hwnd) {
        wprintf(L"SilentPlayer main window not found\n");
        return 1;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    wprintf(L"target hwnd=%p pid=%lu  WS_EX_ACCEPTFILES=%ls\n", hwnd, pid,
            (ex & WS_EX_ACCEPTFILES) ? L"set" : L"NOT set");

    const std::vector<BYTE> block = MakeDropBlock(file);
    HANDLE hProc = nullptr;
    void* remote = WriteBlockIntoProcess(pid, block, &hProc);
    if (!remote) {
        wprintf(L"-> cannot place HDROP in target process\n");
        return 1;
    }
    wprintf(L"HDROP placed at remote address %p (%zu bytes), posting WM_DROPFILES...\n",
            remote, block.size());
    PostMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(remote), 0);
    Sleep(1200);
    wprintf(L"process alive=%ls\n",
            FindWindowW(L"SilentPlayerMainWindow", nullptr) ? L"yes" : L"NO");
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return 0;
}

// 自检：同样的 HDROP 块发给自己进程里一个注册过拖放的窗口，
// 用来区分「HDROP 格式不对」和「跨进程没整理」。
LRESULT CALLBACK SinkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DROPFILES) {
        HDROP hDrop = reinterpret_cast<HDROP>(wp);
        const UINT len = DragQueryFileW(hDrop, 0, nullptr, 0);
        wprintf(L"[selftest] WM_DROPFILES received, name length=%u chars\n", len);
        if (len > 0) {
            wchar_t buf[MAX_PATH] = {};
            DragQueryFileW(hDrop, 0, buf, MAX_PATH);
            wprintf(L"[selftest] parsed path = '%ls'\n", buf);
        }
        DragFinish(hDrop);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int SelfTest(const std::wstring& file) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SinkProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DropProbeSink";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, L"DropProbeSink", L"sink", WS_OVERLAPPED, 0, 0,
                             100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) {
        wprintf(L"CreateWindow failed\n");
        return 1;
    }
    DragAcceptFiles(h, TRUE);
    wprintf(L"[selftest] sink hwnd=%p WS_EX_ACCEPTFILES=%ls\n", h,
            (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) ? L"set"
                                                                   : L"NOT set");
    const std::vector<BYTE> block = MakeDropBlock(file);
    // 同进程内，直接传本进程地址即可。
    SendMessageW(h, WM_DROPFILES,
                 reinterpret_cast<WPARAM>(const_cast<BYTE*>(block.data())), 0);
    DestroyWindow(h);
    UnregisterClassW(L"DropProbeSink", wc.hInstance);
    return 0;
}

// 托盘区域窗口链：Shell_TrayWnd -> TrayNotifyWnd -> SysPager -> ToolbarWindow32
void WalkTrayWindows() {
    HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    wprintf(L"Shell_TrayWnd      = %p\n", shell);
    if (!shell) {
        return;
    }
    HWND notify = FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr);
    wprintf(L"TrayNotifyWnd      = %p\n", notify);
    if (!notify) {
        return;
    }
    HWND pager = FindWindowExW(notify, nullptr, L"SysPager", nullptr);
    wprintf(L"SysPager           = %p\n", pager);
    HWND toolbar = nullptr;
    if (pager) {
        toolbar = FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr);
    }
    if (!toolbar) {
        toolbar = FindWindowExW(notify, nullptr, L"ToolbarWindow32", nullptr);
    }
    wprintf(L"ToolbarWindow32    = %p\n",
            toolbar);
}

// 一个什么都不做的投放目标，只为探测“该窗口是否已被别人注册过”。
class ProbeTarget : public IDropTarget {
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
    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override {
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD*) override { return S_OK; }
    STDMETHODIMP DragLeave() override { return S_OK; }
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL, DWORD*) override {
        return S_OK;
    }
};

int TrayCheck() {
    OleInitialize(nullptr);
    WalkTrayWindows();

    HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notify = shell ? FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr)
                        : nullptr;
    HWND pager = notify ? FindWindowExW(notify, nullptr, L"SysPager", nullptr)
                        : nullptr;
    HWND toolbar = pager ? FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr)
                         : nullptr;

    static ProbeTarget target;
    HWND candidates[2] = {toolbar, notify};
    for (HWND h : candidates) {
        if (!h) {
            continue;
        }
        const HRESULT hr = RegisterDragDrop(h, &target);
        wprintf(L"RegisterDragDrop(%p) = 0x%08lX", h,
                static_cast<unsigned long>(hr));
        if (hr == DRAGDROP_E_ALREADYREGISTERED) {
            wprintf(L"  -> ALREADY registered (owner has a drop target)\n");
        } else if (SUCCEEDED(hr)) {
            wprintf(L"  -> NOT registered (no drop target on this window)\n");
            RevokeDragDrop(h);
        } else {
            wprintf(L"  -> other\n");
        }
    }
    wprintf(L"\nOLE drag-drop picks the target by the window under the cursor.\n"
            L"A 3rd-party tray icon is just a picture drawn by explorer; the window under\n"
            L"the cursor belongs to explorer, so the app can never receive that drop.\n");
    OleUninitialize();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        wprintf(L"usage: drop_probe.exe drop <file> | selftest <file> | traycheck\n");
        return 1;
    }
    if (wcscmp(argv[1], L"drop") == 0 && argc >= 3) {
        return DoDrop(argv[2]);
    }
    if (wcscmp(argv[1], L"checktarget") == 0) {
        // 若目标窗口已经注册了 OLE 投放目标，RegisterDragDrop 会返回
        // DRAGDROP_E_ALREADYREGISTERED。用这一点验证应用确实挂上了 IDropTarget。
        HWND h = FindWindowW(L"SilentPlayerMainWindow", nullptr);
        if (!h) {
            wprintf(L"SilentPlayer window not found\n");
            return 1;
        }
        OleInitialize(nullptr);
        static ProbeTarget t;
        const HRESULT hr = RegisterDragDrop(h, &t);
        wprintf(L"RegisterDragDrop(SilentPlayer hwnd=%p) = 0x%08lX\n", h,
                (unsigned long)hr);
        if (hr == DRAGDROP_E_ALREADYREGISTERED) {
            wprintf(L"  -> 该窗口【已注册】OLE 投放目标 => 应用的文件拖放已生效\n");
        } else if (SUCCEEDED(hr)) {
            wprintf(L"  -> 该窗口【没有】投放目标（应用未注册成功）\n");
            RevokeDragDrop(h);
        } else {
            wprintf(L"  -> 其它结果\n");
        }
        OleUninitialize();
        return 0;
    }
    if (wcscmp(argv[1], L"rawdrop") == 0) {
        // 只发一个空的 WM_DROPFILES，用于判断消息本身是否会被投递
        HWND h = FindWindowW(L"SilentPlayerMainWindow", nullptr);
        if (!h) {
            wprintf(L"window not found\n");
            return 1;
        }
        const BOOL ok = PostMessageW(h, WM_DROPFILES, 0, 0);
        wprintf(L"PostMessage(WM_DROPFILES, 0, 0) -> %d err=%lu\n", ok,
                GetLastError());
        Sleep(600);
        return 0;
    }
    if (wcscmp(argv[1], L"selftest") == 0 && argc >= 3) {
        return SelfTest(argv[2]);
    }
    if (wcscmp(argv[1], L"traycheck") == 0) {
        return TrayCheck();
    }
    wprintf(L"unknown command\n");
    return 1;
}

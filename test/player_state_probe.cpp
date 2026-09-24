// Diagnostic probe: sample SilentPlayer's UI state and its real audio peak over time.
//
// Purpose: verify that opening a new file really restarts the presentation
// (position goes back to 0 at the moment of the switch) and that audio is
// actually being rendered afterwards -- not just that the file name label
// changed. Read-only: it only reads window/control state and WASAPI peaks.
//
// usage: player_state_probe.exe [samples] [intervalMs] [--toggle=i,j,k]
//        player_state_probe.exe 24 500 --toggle=8,13
//        --toggle=i sends the play/pause command at sample i, so pause/resume
//        can be exercised without a human clicking the button.
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT kTrayCallbackMsg = WM_APP + 1; // App.h: WM_APP_TRAY_MSG
constexpr int kProgressMax = 1000;            // PlayerWindow.h: kProgressMax

enum : UINT {
    IDC_FILE = 1001,
    IDC_PROGRESS = 1002,
    IDC_TIME = 1003,
    IDC_PLAYPAUSE = 1004,
    IDC_STOP = 1005,
    IDC_SUBTITLE = 1008,
    IDC_DURATION = 1009,
    IDC_VOLUME_LABEL = 1010,
    IDC_VOLUME = 1006,
    IDC_VOLUME_PCT = 1007,
    IDC_DESTROY = 1011,
};

// 托盘菜单：打开后枚举条目，用于验证「销毁」确实存在且顺序正确。
// 菜单是 TrackPopupMenu 创建的临时弹出窗口，类名 #32768。
void DumpTrayMenu(HWND mainHwnd) {
    PostMessageW(mainHwnd, kTrayCallbackMsg, 0, WM_RBUTTONUP);
    HWND menu = nullptr;
    for (int i = 0; i < 60 && !menu; ++i) {
        Sleep(20);
        menu = FindWindowW(L"#32768", nullptr);
    }
    if (!menu) {
        wprintf(L"tray menu did not open\n");
        return;
    }
    Sleep(150);
    // 菜单窗口不是普通窗口，取 HMENU 要用 MN_GETHMENU（GetMenu 拿不到）。
    HMENU hMenu = reinterpret_cast<HMENU>(
        SendMessageW(menu, MN_GETHMENU, 0, 0));
    if (!hMenu) {
        wprintf(L"tray menu handle not available\n");
    } else {
        const int n = GetMenuItemCount(hMenu);
        wprintf(L"tray menu items = %d\n", n);
        for (int i = 0; i < n; ++i) {
            wchar_t text[128] = {};
            GetMenuStringW(hMenu, i, text, 128, MF_BYPOSITION);
            const UINT state = GetMenuState(hMenu, i, MF_BYPOSITION);
            if (state == static_cast<UINT>(-1)) {
                continue;
            }
            if ((state & MF_SEPARATOR) != 0) {
                wprintf(L"  [%d] ---- separator ----\n", i);
            } else {
                wprintf(L"  [%d] id=%u text='%ls'%ls\n", i, GetMenuItemID(hMenu, i),
                        text, (state & MF_CHECKED) ? L"  [CHECKED]" : L"");
            }
        }
    }
    // 关闭菜单
    PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
    PostMessageW(menu, WM_KEYUP, VK_ESCAPE, 0);
    Sleep(200);
}

bool InvokeTrayItem(HWND mainHwnd, int itemIndex); // 前置声明

// 触发托盘「麦克风输出提示」并读出弹窗内容，然后关掉它。
// 这样"检测结果与指引文本"可以被自动化核对，不用靠肉眼看截图。
void DumpMicHintDialog(HWND mainHwnd) {
    // 菜单里的「输出到麦克风」位置（0 起，含分隔符/禁用项，见 --traydump）。
    // 有文件时：0=打开文件… 1=sep 2=文件名 3=sep 4=播放/暂停 5=停止 6=销毁 7=输出到麦克风 …
    if (!InvokeTrayItem(mainHwnd, 7)) {
        return;
    }
    HWND dlg = nullptr;
    for (int i = 0; i < 60 && !dlg; ++i) {
        Sleep(50);
        dlg = FindWindowW(L"#32770", nullptr);
    }
    if (!dlg) {
        wprintf(L"mic hint dialog did not appear\n");
        return;
    }
    wchar_t title[256] = {};
    GetWindowTextW(dlg, title, 256);
    wprintf(L"dialog title = '%ls'\n", title);

    // 逐个静态控件取文本（MessageBox 的正文是一个 Static 子窗口）。
    for (HWND child = GetWindow(dlg, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64] = {};
        GetClassNameW(child, cls, 64);
        if (_wcsicmp(cls, L"Static") != 0) {
            continue;
        }
        const int len = GetWindowTextLengthW(child);
        if (len <= 0) {
            continue;
        }
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(child, &text[0], len + 1);
        text.resize(static_cast<size_t>(len));
        wprintf(L"---- dialog text ----\n%ls\n---------------------\n", text.c_str());
        // 同时以 UTF-8 落盘：控制台代码页显示不了中文，落盘后才能核对文本内容
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"E:////kunkun////slientPlayer////build////_michint.txt",
                      L"w, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"%ls", text.c_str());
            fclose(f);
        }
    }
    PostMessageW(dlg, WM_CLOSE, 0, 0);
    Sleep(300);
}

// 模拟点击托盘菜单里的第 itemIndex 个可选项（0 起，仅计数非分隔项）。
bool InvokeTrayItem(HWND mainHwnd, int itemIndex) {
    // 先把光标挪到屏幕偏中间：菜单在光标处弹出，贴屏幕边缘时会被重新摆放。
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    SetCursorPos(sw / 3, sh / 3);
    Sleep(120);

    PostMessageW(mainHwnd, kTrayCallbackMsg, 0, WM_RBUTTONUP);
    HWND menu = nullptr;
    for (int i = 0; i < 60 && !menu; ++i) {
        Sleep(50);
        menu = FindWindowW(L"#32768", nullptr);
    }
    if (!menu) {
        return false;
    }
    Sleep(150);

    HMENU hMenu = reinterpret_cast<HMENU>(SendMessageW(menu, MN_GETHMENU, 0, 0));
    if (!hMenu) {
        PostMessageW(menu, WM_CLOSE, 0, 0);
        return false;
    }
    // 按"菜单位置"（0 起，含分隔符与禁用项，见 --traydump）直接点该条目的中心。
    // 比键盘下移计数确定：不受分隔符/禁用项是否可导航的影响。
    RECT rc{};
    if (!GetMenuItemRect(menu, hMenu, itemIndex, &rc)) {
        wprintf(L"menu position %d not found\n", itemIndex);
        PostMessageW(menu, WM_CLOSE, 0, 0);
        return false;
    }
    const LPARAM lp =
        MAKELPARAM((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
    PostMessageW(menu, WM_MOUSEMOVE, 0, lp);
    Sleep(80);
    PostMessageW(menu, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(80);
    PostMessageW(menu, WM_LBUTTONUP, 0, lp);
    Sleep(450);
    return true;
}

// ASCII-safe state token: the UI labels are Chinese, which the console codepage
// mangles, so map them to stable ASCII tokens instead.
// Escapes are used so this file stays readable regardless of source encoding.
const char* StatusToken(const std::wstring& subtitle) {
    if (subtitle == L"\u6B63\u5728\u64AD\u653E") return "PLAYING";   // 正在播放
    if (subtitle == L"\u5DF2\u6682\u505C") return "PAUSED";          // 已暂停
    if (subtitle == L"\u5DF2\u505C\u6B62") return "STOPPED";         // 已停止
    if (subtitle == L"\u5DF2\u64AD\u653E\u5B8C") return "ENDED";     // 已播放完
    if (subtitle == L"\u672A\u6253\u5F00\u6587\u4EF6") return "NOFILE"; // 未打开文件
    if (subtitle == L"\u64AD\u653E\u51FA\u9519") return "ERROR";     // 播放出错
    if (subtitle == L"\u65E0\u6CD5\u64AD\u653E\u8BE5\u6587\u4EF6") return "BADFILE"; // 无法播放该文件
    return "UNKNOWN";
}

std::wstring GetTextOf(HWND hwnd) {
    if (!hwnd) {
        return L"<none>";
    }
    wchar_t buf[512] = {};
    // WM_GETTEXT is marshalled for standard controls across processes.
    SendMessageW(hwnd, WM_GETTEXT, 511, reinterpret_cast<LPARAM>(buf));
    return buf;
}

// Max peak across all active render endpoints for the given pid, plus the
// actual WASAPI session volume (0..1, or -1 if unavailable).
// Sonar-like virtual devices may route the app away from the default endpoint,
// so every active endpoint is checked.
float PeakForPid(DWORD pid, float* outVolume = nullptr) {
    if (outVolume) {
        *outVolume = -1.0f;
    }
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pEnum)))) {
        return -1.0f;
    }
    IMMDeviceCollection* pColl = nullptr;
    if (FAILED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl))) {
        pEnum->Release();
        return -1.0f;
    }
    UINT n = 0;
    pColl->GetCount(&n);
    float best = 0.0f;
    for (UINT i = 0; i < n; ++i) {
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
                        IAudioMeterInformation* pMeter = nullptr;
                        if (SUCCEEDED(pCtl->QueryInterface(IID_PPV_ARGS(&pMeter)))) {
                            float peak = 0.0f;
                            if (SUCCEEDED(pMeter->GetPeakValue(&peak)) && peak > best) {
                                best = peak;
                            }
                            pMeter->Release();
                        }
                        if (outVolume) {
                            ISimpleAudioVolume* pVol = nullptr;
                            if (SUCCEEDED(pCtl->QueryInterface(IID_PPV_ARGS(&pVol)))) {
                                float v = 0.0f;
                                if (SUCCEEDED(pVol->GetMasterVolume(&v))) {
                                    *outVolume = v;
                                }
                                pVol->Release();
                            }
                        }
                    }
                    if (pCtl2) pCtl2->Release();
                    pCtl->Release();
                }
                pSessions->Release();
            }
            pMgr->Release();
        }
        pDev->Release();
    }
    pColl->Release();
    pEnum->Release();
    return best;
}

} // namespace

// --dump: print client size, every child control's rect, and flag overlaps.
// Used to check the layout is not cramped without a human looking at it.
namespace {

struct DumpItem {
    UINT id;
    RECT rc;
};

struct DumpCtx {
    DumpItem items[24];
    int count;
};

BOOL CALLBACK DumpChildProc(HWND child, LPARAM param) {
    auto* ctx = reinterpret_cast<DumpCtx*>(param);
    if (ctx->count >= 24) {
        return TRUE;
    }
    wchar_t cls[128] = {};
    GetClassNameW(child, cls, 128);
    RECT rc{};
    GetWindowRect(child, &rc);
    POINT tl{rc.left, rc.top};
    ScreenToClient(GetParent(child), &tl);
    RECT rel{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
    ctx->items[ctx->count].id = GetDlgCtrlID(child);
    ctx->items[ctx->count].rc = rel;
    ++ctx->count;
    const std::wstring txt = GetTextOf(child);
    // 同时以 UTF-8 落盘：控制台代码页显示不了中文，落盘后才能核对控件文本
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"E:////kunkun////slientPlayer////build////_controls.txt",
                  L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"  id=%u %s | x=%ld y=%ld w=%ld h=%ld | '%ls'\n",
                 GetDlgCtrlID(child), cls, rel.left, rel.top,
                 rel.right - rel.left, rel.bottom - rel.top, txt.c_str());
        fclose(f);
    }
    wprintf(L"  id=%-4u %-12s x=%-4ld y=%-4ld w=%-4ld h=%-4ld text='%ls'\n",
            GetDlgCtrlID(child), cls, rel.left, rel.top, rel.right - rel.left,
            rel.bottom - rel.top, txt.c_str());
    return TRUE;
}

int DumpLayout(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    wprintf(L"client = %ld x %ld\n", client.right - client.left,
            client.bottom - client.top);

    DumpCtx ctx{};
    EnumChildWindows(hwnd, DumpChildProc, reinterpret_cast<LPARAM>(&ctx));

    int overlaps = 0;
    for (int i = 0; i < ctx.count; ++i) {
        for (int j = i + 1; j < ctx.count; ++j) {
            RECT r{};
            if (IntersectRect(&r, &ctx.items[i].rc, &ctx.items[j].rc)) {
                wprintf(L"  !! OVERLAP id=%u with id=%u\n", ctx.items[i].id,
                        ctx.items[j].id);
                ++overlaps;
            }
        }
    }
    wprintf(L"controls=%d overlaps=%d\n", ctx.count, overlaps);
    return overlaps == 0 ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Must be DPI aware, otherwise window coordinates are virtualized to 96 DPI
    // and the geometry dump would not match the real layout.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int samples = 20;
    DWORD intervalMs = 500;
    int toggleAt[16] = {};
    int toggleCount = 0;
    int seekAt = -1;   // --seek=<sampleIndex>:<percent>
    int seekPct = 0;
    int volAt = -1;    // --volume=<sampleIndex>:<0..100>
    int volVal = 0;
    int closeAt = -1;  // --close=<sampleIndex>
    int destroyAt = -1; // --destroy=<sampleIndex>  点击 UI「销毁」按钮
    int trayItemAt = -1; // --trayitem=<sampleIndex>:<n>  点击托盘菜单第 n 个可选项
    int trayItemIndex = 0;
    bool dumpLayout = false;
    bool dumpTray = false;
    bool dumpMicHint = false;
    int wheelNotches = 0;
    std::wstring keyName;
    int clickProgressPct = -1;
    int clickVolumePct = -1;
    int wheelChildNotches = 0;
    bool openFileDialog = false;
    bool tooltipCheck = false;
    bool trayLeft = false;
    bool hotkeyCheck = false;
    int hotkeyId = 0;
    if (argc > 1) samples = _wtoi(argv[1]);
    if (argc > 2) intervalMs = static_cast<DWORD>(_wtoi(argv[2]));
    for (int a = 1; a < argc; ++a) {
        if (wcscmp(argv[a], L"--dump") == 0) {
            dumpLayout = true;
        } else if (wcscmp(argv[a], L"--traydump") == 0) {
            dumpTray = true;
        } else if (wcscmp(argv[a], L"--michint") == 0) {
            dumpMicHint = true;
        } else if (wcsncmp(argv[a], L"--wheel=", 8) == 0) {
            wheelNotches = _wtoi(argv[a] + 8);
        } else if (wcsncmp(argv[a], L"--key=", 6) == 0) {
            keyName = argv[a] + 6;
        } else if (wcsncmp(argv[a], L"--clickprogress=", 16) == 0) {
            clickProgressPct = _wtoi(argv[a] + 16);
        } else if (wcsncmp(argv[a], L"--clickvolume=", 14) == 0) {
            clickVolumePct = _wtoi(argv[a] + 14);
        } else if (wcsncmp(argv[a], L"--wheelchild=", 13) == 0) {
            wheelChildNotches = _wtoi(argv[a] + 13);
        } else if (wcscmp(argv[a], L"--openfile") == 0) {
            openFileDialog = true;
        } else if (wcscmp(argv[a], L"--tooltipcheck") == 0) {
            tooltipCheck = true;
        } else if (wcscmp(argv[a], L"--trayleft") == 0) {
            trayLeft = true;
        } else if (wcscmp(argv[a], L"--hotkeycheck") == 0) {
            hotkeyCheck = true;
        } else if (wcsncmp(argv[a], L"--hotkey=", 9) == 0) {
            hotkeyId = _wtoi(argv[a] + 9);
        } else if (wcsncmp(argv[a], L"--toggle=", 9) == 0) {
            wchar_t* ctx = nullptr;
            for (wchar_t* tok = wcstok_s(argv[a] + 9, L",", &ctx);
                 tok && toggleCount < 16; tok = wcstok_s(nullptr, L",", &ctx)) {
                toggleAt[toggleCount++] = _wtoi(tok);
            }
        } else if (wcsncmp(argv[a], L"--seek=", 7) == 0) {
            wchar_t* ctx = nullptr;
            const wchar_t* first = wcstok_s(argv[a] + 7, L":", &ctx);
            const wchar_t* second = wcstok_s(nullptr, L":", &ctx);
            if (first && second) {
                seekAt = _wtoi(first);
                seekPct = _wtoi(second);
            }
        } else if (wcsncmp(argv[a], L"--volume=", 9) == 0) {
            wchar_t* ctx = nullptr;
            const wchar_t* first = wcstok_s(argv[a] + 9, L":", &ctx);
            const wchar_t* second = wcstok_s(nullptr, L":", &ctx);
            if (first && second) {
                volAt = _wtoi(first);
                volVal = _wtoi(second);
            }
        } else if (wcsncmp(argv[a], L"--close=", 8) == 0) {
            closeAt = _wtoi(argv[a] + 8);
        } else if (wcsncmp(argv[a], L"--destroy=", 10) == 0) {
            destroyAt = _wtoi(argv[a] + 10);
        } else if (wcsncmp(argv[a], L"--trayitem=", 11) == 0) {
            wchar_t* ctx = nullptr;
            const wchar_t* first = wcstok_s(argv[a] + 11, L":", &ctx);
            const wchar_t* second = wcstok_s(nullptr, L":", &ctx);
            if (first && second) {
                trayItemAt = _wtoi(first);
                trayItemIndex = _wtoi(second);
            }
        }
    }

    const HWND hwnd = FindWindowW(L"SilentPlayerMainWindow", nullptr);
    if (!hwnd) {
        wprintf(L"SilentPlayer main window not found.\n");
        return 1;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    if (dumpLayout) {
        return DumpLayout(hwnd);
    }
    if (dumpTray) {
        DumpTrayMenu(hwnd);
        return 0;
    }
    if (dumpMicHint) {
        DumpMicHintDialog(hwnd);
        return 0;
    }
    if (tooltipCheck) {
        // 检查本进程是否创建了 tooltip 控件。
        // 注意：tooltip 是 WS_POPUP（主窗口只是它的属主，不是父窗口），
        // 所以不能用 FindWindowEx(hwnd,...) 搜子窗口，要按"同进程 + 类名"找。
        struct Ctx {
            DWORD pid;
            HWND found;
        } ctx{GetWindowThreadProcessId(hwnd, nullptr), nullptr};
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        ctx.pid = pid;
        EnumWindows(
            [](HWND w, LPARAM lp) -> BOOL {
                Ctx* c = reinterpret_cast<Ctx*>(lp);
                DWORD wp = 0;
                GetWindowThreadProcessId(w, &wp);
                if (wp == c->pid) {
                    wchar_t cls[64] = {};
                    GetClassNameW(w, cls, 64);
                    if (_wcsicmp(cls, L"tooltips_class32") == 0) {
                        c->found = w;
                        return FALSE;
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        wprintf(L"tooltip window = %ls\n", ctx.found ? L"exists" : L"MISSING");
        return ctx.found ? 0 : 1;
    }
    if (clickVolumePct >= 0) {
        // 在音量条轨道上按百分比位置点一下（验证"点哪跳哪"）
        HWND bar = GetDlgItem(hwnd, 1006);
        if (!bar) {
            wprintf(L"volume bar not found\n");
            return 1;
        }
        RECT rc{};
        GetClientRect(bar, &rc);
        const LPARAM lp = MAKELPARAM(rc.left + (rc.right - rc.left) * clickVolumePct / 100,
                                     (rc.top + rc.bottom) / 2);
        PostMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageW(bar, WM_LBUTTONUP, 0, lp);
        Sleep(600);
        wprintf(L"clicked volume at %d%%\n", clickVolumePct);
        return 0;
    }
    if (wheelChildNotches != 0) {
        // 把滚轮投给"光标下的子控件"（进度条）：验证滚轮是否仍统一调音量、且不改播放位置
        HWND bar = GetDlgItem(hwnd, 1002);
        if (!bar) {
            wprintf(L"progress bar not found\n");
            return 1;
        }
        const int delta = wheelChildNotches > 0 ? 120 : -120;
        const int n = wheelChildNotches > 0 ? wheelChildNotches : -wheelChildNotches;
        for (int i = 0; i < n; ++i) {
            PostMessageW(bar, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), 0);
            Sleep(150);
        }
        Sleep(400);
        wprintf(L"sent %d wheel notch(es) to progress bar\n", wheelChildNotches);
        return 0;
    }
    if (openFileDialog) {
        // 触发托盘「打开文件…」（位置 0），看是否弹出系统文件对话框，然后关掉它
        if (!InvokeTrayItem(hwnd, 0)) {
            return 1;
        }
        HWND dlg = nullptr;
        for (int i = 0; i < 60 && !dlg; ++i) {
            Sleep(80);
            dlg = FindWindowW(nullptr, L"打开音频文件");
        }
        wprintf(L"file dialog appeared = %ls\n", dlg ? L"yes" : L"NO");
        if (dlg) {
            PostMessageW(dlg, WM_CLOSE, 0, 0);
            Sleep(300);
        }
        return 0;
    }
    if (trayLeft) {
        // 模拟单击托盘图标（托盘回调消息 + WM_LBUTTONUP）
        PostMessageW(hwnd, kTrayCallbackMsg, 0, WM_LBUTTONUP);
        Sleep(500);
        wprintf(L"sent tray left click, visible=%d\n", IsWindowVisible(hwnd) ? 1 : 0);
        return 0;
    }
    if (hotkeyCheck) {
        // 若本程序已注册该媒体键，别人再注册会失败并返回 ERROR_HOTKEY_ALREADY_REGISTERED。
        const BOOL ok = RegisterHotKey(nullptr, 0x7F01, MOD_NOREPEAT,
                                       VK_MEDIA_PLAY_PAUSE);
        wprintf(L"RegisterHotKey(VK_MEDIA_PLAY_PAUSE) by probe = %d, err=%lu %ls\n", ok,
                GetLastError(),
                ok ? L"(说明应用没注册上!)" : L"(说明应用已占用该键)");
        if (ok) {
            UnregisterHotKey(nullptr, 0x7F01);
        }
        return 0;
    }
    if (hotkeyId != 0) {
        // 直接投递 WM_HOTKEY，验证处理路径（注册是否成功另由 --hotkeycheck 验证）
        PostMessageW(hwnd, WM_HOTKEY, static_cast<WPARAM>(hotkeyId), 0);
        Sleep(600);
        wprintf(L"sent WM_HOTKEY id=%d\n", hotkeyId);
        return 0;
    }
    if (clickProgressPct >= 0) {
        // 在进度条轨道上按百分比位置点一下，用于验证"点击跳转"是否生效。
        HWND bar = GetDlgItem(hwnd, 1002);
        if (!bar) {
            wprintf(L"progress bar not found\n");
            return 1;
        }
        RECT rc{};
        GetClientRect(bar, &rc);
        const int x = rc.left + (rc.right - rc.left) * clickProgressPct / 100;
        const int y = (rc.top + rc.bottom) / 2;
        const LPARAM lp = MAKELPARAM(x, y);
        PostMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageW(bar, WM_LBUTTONUP, 0, lp);
        Sleep(700);
        wprintf(L"clicked progress at %d%%\n", clickProgressPct);
        return 0;
    }
    if (wheelNotches != 0) {
        // 每个刻度 120；正数 = 滚轮向上（调大音量）
        const int delta = wheelNotches > 0 ? 120 : -120;
        for (int i = 0; i < (wheelNotches > 0 ? wheelNotches : -wheelNotches); ++i) {
            PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), 0);
            Sleep(120);
        }
        Sleep(400);
        wprintf(L"sent %d wheel notch(es)\n", wheelNotches);
        return 0;
    }
    if (!keyName.empty()) {
        WPARAM vk = 0;
        if (keyName == L"space") {
            vk = VK_SPACE;
        } else if (keyName == L"left") {
            vk = VK_LEFT;
        } else if (keyName == L"right") {
            vk = VK_RIGHT;
        } else if (keyName == L"esc") {
            vk = VK_ESCAPE;
        }
        if (!vk) {
            wprintf(L"unknown key '%ls'\n", keyName.c_str());
            return 1;
        }
        // 走窗口消息队列：应用的快捷键是在消息循环里统一拦截的。
        PostMessageW(hwnd, WM_KEYDOWN, vk, 0);
        Sleep(600);
        wprintf(L"sent key '%ls', visible=%d\n", keyName.c_str(),
                IsWindowVisible(hwnd) ? 1 : 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Show the player so its 500ms progress timer actually refreshes.
    PostMessageW(hwnd, kTrayCallbackMsg, 0, WM_LBUTTONUP);
    Sleep(300);

    wprintf(L"pid=%lu hwnd=%p samples=%d interval=%lums\n", pid, hwnd, samples,
            intervalMs);
    wprintf(L"%8s | %-14s | %-7s | %-7s | %-7s | %-11s | %-6s | %-4s | %-5s | %-3s | %s\n",
            L"t(ms)", L"file", L"state", L"cur", L"total", L"pos", L"peak", L"vol",
            L"svol", L"vis", L"note");
    wprintf(L"---------+----------------+---------+---------+---------+"
            L"-------------+--------+------+-------+-----+------\n");

    for (int i = 0; i < samples; ++i) {
        const char* note = "";
        const HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
        const HWND hVol = GetDlgItem(hwnd, IDC_VOLUME);

        for (int t = 0; t < toggleCount; ++t) {
            if (toggleAt[t] == i) {
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PLAYPAUSE, BN_CLICKED),
                             reinterpret_cast<LPARAM>(GetDlgItem(hwnd, IDC_PLAYPAUSE)));
                note = "<-- toggle play/pause";
                Sleep(250); // let the command take effect before sampling
                break;
            }
        }
        if (i == seekAt && hProg) {
            // Mirror a real drag: move the thumb, then THUMBTRACK ... ENDTRACK.
            const LPARAM target = (seekPct * kProgressMax) / 100;
            SendMessageW(hProg, TBM_SETPOS, TRUE, target);
            PostMessageW(hwnd, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, target),
                         reinterpret_cast<LPARAM>(hProg));
            Sleep(250);
            PostMessageW(hwnd, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, target),
                         reinterpret_cast<LPARAM>(hProg));
            note = "<-- drag progress to seek";
            Sleep(250);
        }
        if (i == volAt && hVol) {
            SendMessageW(hVol, TBM_SETPOS, TRUE, volVal);
            PostMessageW(hwnd, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, volVal),
                         reinterpret_cast<LPARAM>(hVol));
            note = "<-- volume slider";
            Sleep(250);
        }
        if (i == closeAt) {
            // WM_CLOSE must only hide the window and keep playing.
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            note = "<-- WM_CLOSE (hide)";
            Sleep(400);
        }
        if (i == destroyAt) {
            // 模拟点击 UI「销毁」按钮
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_DESTROY, BN_CLICKED),
                         reinterpret_cast<LPARAM>(GetDlgItem(hwnd, IDC_DESTROY)));
            note = "<-- UI DESTROY button";
            Sleep(500);
        }
        if (i == trayItemAt) {
            // 模拟点击托盘菜单里的条目
            InvokeTrayItem(hwnd, trayItemIndex);
            note = "<-- tray menu item";
            Sleep(300);
        }

        const HWND hFile = GetDlgItem(hwnd, IDC_FILE);
        const HWND hSub = GetDlgItem(hwnd, IDC_SUBTITLE);
        const HWND hTime = GetDlgItem(hwnd, IDC_TIME);
        const HWND hDur = GetDlgItem(hwnd, IDC_DURATION);
        const HWND hVolPct = GetDlgItem(hwnd, IDC_VOLUME_PCT);
        const LRESULT pos = hProg ? SendMessageW(hProg, TBM_GETPOS, 0, 0) : -1;
        float sessionVol = -1.0f;
        const float peak = PeakForPid(pid, &sessionVol);
        wprintf(L"%8llu | %-14s | %-7hs | %-7s | %-7s | %4ld/%-6d | %.4f | %-4s | %-5.2f | %-3d | %hs\n",
                GetTickCount64() % 1000000, GetTextOf(hFile).c_str(),
                StatusToken(GetTextOf(hSub)), GetTextOf(hTime).c_str(),
                GetTextOf(hDur).c_str(), static_cast<long>(pos), kProgressMax,
                peak, GetTextOf(hVolPct).c_str(), sessionVol,
                IsWindowVisible(hwnd) ? 1 : 0, note);
        fflush(stdout);
        Sleep(intervalMs);
    }

    CoUninitialize();
    return 0;
}

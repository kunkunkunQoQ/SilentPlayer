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
    if (!InvokeTrayItem(mainHwnd, 3)) {
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
    // 先把光标挪到屏幕偏中间的位置：菜单在光标处弹出，若光标贴着屏幕边缘，
    // 系统会重新摆放菜单，导致“光标下的条目”不确定、键盘下移次数也就不可预测。
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    SetCursorPos(sw / 3, sh / 3);
    Sleep(120);

    PostMessageW(mainHwnd, kTrayCallbackMsg, 0, WM_RBUTTONUP);
    HWND menu = nullptr;
    for (int i = 0; i < 60 && !menu; ++i) {
        Sleep(20);
        menu = FindWindowW(L"#32768", nullptr);
    }
    if (!menu) {
        wprintf(L"tray menu did not open\n");
        return false;
    }
    Sleep(200);
    // 菜单刚打开时没有选中项：第一次 VK_DOWN 会落到第 0 项，
    // 所以要激活第 N 个可选项需要按 N+1 次。
    for (int i = 0; i <= itemIndex; ++i) {
        PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYUP, VK_DOWN, 0);
        Sleep(90);
    }
    Sleep(80);
    PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageW(menu, WM_KEYUP, VK_RETURN, 0);
    Sleep(400);
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
    wprintf(L"  id=%-4u %-12s x=%-4ld y=%-4ld w=%-4ld h=%-4ld text='%ls'\n",
            GetDlgCtrlID(child), cls, rel.left, rel.top, rel.right - rel.left,
            rel.bottom - rel.top, GetTextOf(child).c_str());
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
    if (argc > 1) samples = _wtoi(argv[1]);
    if (argc > 2) intervalMs = static_cast<DWORD>(_wtoi(argv[2]));
    for (int a = 1; a < argc; ++a) {
        if (wcscmp(argv[a], L"--dump") == 0) {
            dumpLayout = true;
        } else if (wcscmp(argv[a], L"--traydump") == 0) {
            dumpTray = true;
        } else if (wcscmp(argv[a], L"--michint") == 0) {
            dumpMicHint = true;
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

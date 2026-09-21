#include "PlayerWindow.h"

#include "App.h"
#include "SingleInstance.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <oleidl.h>
#include <cwchar>

namespace {

// 客户端基准尺寸（96 DPI）；实际会按显示器 DPI 缩放。
// 宁可窗口大一点，也不要让控件挤在一起。
constexpr int kClientWidth = 420;
constexpr int kClientHeight = 222;

constexpr int kMargin = 18;      // 左右留白
constexpr int kTitleW = 120;     // 单个时间标签宽度

// 拖入文件时只关心 CF_HDROP。
FORMATETC HDropFormat() {
    FORMATETC fmt = {};
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    return fmt;
}

} // namespace

int PlayerWindow::Scale(int value) const {
    return MulDiv(value, m_dpi > 0 ? m_dpi : 96, 96);
}

bool PlayerWindow::Create(HINSTANCE hInstance, App* app) {
    m_app = app;
    m_dpi = GetDpiForSystem();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PlayerWindow::WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSilentPlayerWindowClass;
    RegisterClassExW(&wc);

    RECT rc = {0, 0, Scale(kClientWidth), Scale(kClientHeight)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    // 居中于主工作区。
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left - w) / 2);
    const int y = work.top + ((work.bottom - work.top - h) / 2);

    m_hwnd = CreateWindowExW(0, kSilentPlayerWindowClass, L"SilentPlayer", style,
                             x, y, w, h, nullptr, nullptr, hInstance, this);
    if (!m_hwnd) {
        return false;
    }
    // 注册为文件拖放目标：把文件拖到窗口上即可直接播放。
    RegisterFileDrop();
    return true;
}

void PlayerWindow::Destroy() {
    if (m_hwnd) {
        UnregisterFileDrop();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool PlayerWindow::IsVisible() const {
    return m_hwnd && IsWindowVisible(m_hwnd);
}

void PlayerWindow::Show() {
    if (!m_hwnd) {
        return;
    }
    if (IsIconic(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    }
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    StartTimerIfVisible();
}

void PlayerWindow::Hide() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_HIDE);
        KillTimer(m_hwnd, kTimerId);
    }
}

void PlayerWindow::StartTimerIfVisible() {
    if (IsVisible() && !m_draggingProgress) {
        SetTimer(m_hwnd, kTimerId, kTimerIntervalMs, nullptr);
    }
}

// --- UI 更新 -------------------------------------------------------------

void PlayerWindow::SetFileName(const std::wstring& fileName) {
    if (m_hFile) {
        SetWindowTextW(m_hFile, fileName.c_str());
    }
    if (m_hwnd) {
        const std::wstring title =
            fileName.empty() ? L"SilentPlayer" : (L"SilentPlayer — " + fileName);
        SetWindowTextW(m_hwnd, title.c_str());
    }
}

void PlayerWindow::SetStatus(const std::wstring& text) {
    if (m_hSubtitle) {
        SetWindowTextW(m_hSubtitle, text.c_str());
    }
}

void PlayerWindow::ClearMedia() {
    // 清掉上一首的一切痕迹，回到“未打开文件”的空闲状态。
    // 音量是全局设置，不随媒体清理。
    m_draggingProgress = false;
    SetFileName(std::wstring());
    SetStatus(L"未打开文件");
    RefreshPlayback(0.0, 0.0, 0.0);
}

void PlayerWindow::SetPlayState(bool playing) {
    m_playing = playing;
    if (m_hPlayPause) {
        SetWindowTextW(m_hPlayPause, playing ? L"暂停" : L"播放");
    }
}

void PlayerWindow::RefreshPlayback(double fraction, double positionSec,
                                   double durationSec) {
    if (m_draggingProgress) {
        return; // 用户正在拖动进度条，不覆盖。
    }
    m_positionSec = positionSec;
    m_durationSec = durationSec;
    if (m_hProgress) {
        int pos = static_cast<int>(fraction * kProgressMax);
        if (pos < 0) {
            pos = 0;
        }
        if (pos > kProgressMax) {
            pos = kProgressMax;
        }
        SendMessageW(m_hProgress, TBM_SETPOS, TRUE, pos);
    }
    UpdateTimeLabel();
}

void PlayerWindow::SetVolumeUI(float volume) {
    m_volume = volume;
    if (m_hVolume) {
        SendMessageW(m_hVolume, TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(volume * 100.0f + 0.5f));
    }
    if (m_hVolumePct) {
        wchar_t buf[16];
        swprintf_s(buf, L"%d%%", static_cast<int>(volume * 100.0f + 0.5f));
        SetWindowTextW(m_hVolumePct, buf);
    }
}

void PlayerWindow::SetControlsEnabled(bool enabled) {
    if (m_hProgress) {
        EnableWindow(m_hProgress, enabled ? TRUE : FALSE);
    }
    if (m_hPlayPause) {
        EnableWindow(m_hPlayPause, enabled ? TRUE : FALSE);
    }
    if (m_hStop) {
        EnableWindow(m_hStop, enabled ? TRUE : FALSE);
    }
    if (m_hDestroy) {
        EnableWindow(m_hDestroy, enabled ? TRUE : FALSE);
    }
}

// --- OLE 拖放目标：把文件拖到窗口上直接播放 --------------------------------
// 用 IDropTarget 而不是旧的 WM_DROPFILES：
//   * 现代 shell 的文件拖放走 OLE，任何拖放源都会投递到已注册 IDropTarget 的窗口；
//   * WM_DROPFILES 只有 explorer 会发（User32 会校验 HDROP 句柄，
//     第三方进程连构造测试消息都会被 ERROR_INVALID_HANDLE 拒绝），
//     既不可靠也无法自动化验证。
// 取到路径后走与命令行 / 单实例 IPC 完全相同的入口 App::LoadFile()。
class PlayerWindow::DropTarget : public IDropTarget {
public:
    explicit DropTarget(PlayerWindow* window) : m_window(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) {
            delete this;
        }
        return static_cast<ULONG>(n);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD /*keys*/,
                                        POINTL /*pt*/, DWORD* effect) override {
        const bool has = HasFiles(data);
        if (effect) {
            *effect = has ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*keys*/, POINTL /*pt*/,
                                       DWORD* effect) override {
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD /*keys*/,
                                   POINTL /*pt*/, DWORD* effect) override {
        if (effect) {
            *effect = DROPEFFECT_NONE;
        }
        std::wstring path;
        if (!TakeFirstFile(data, &path) || path.empty()) {
            return S_OK;
        }
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        if (m_window && m_window->m_app) {
            m_window->m_app->LoadFile(path);
        }
        return S_OK;
    }

private:
    static bool HasFiles(IDataObject* data) {
        if (!data) {
            return false;
        }
        FORMATETC fmt = HDropFormat();
        return data->QueryGetData(&fmt) == S_OK;
    }

    // 只取第一个文件：SilentPlayer 是单文件播放器，多余的忽略。
    static bool TakeFirstFile(IDataObject* data, std::wstring* path) {
        if (!data || !path) {
            return false;
        }
        FORMATETC fmt = HDropFormat();
        STGMEDIUM stg = {};
        if (FAILED(data->GetData(&fmt, &stg))) {
            return false;
        }
        bool ok = false;
        if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal) {
            const HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
            if (hDrop) {
                const UINT len = DragQueryFileW(hDrop, 0, nullptr, 0);
                if (len > 0) {
                    path->assign(static_cast<size_t>(len) + 1, L'\0');
                    const UINT copied =
                        DragQueryFileW(hDrop, 0, path->data(), len + 1);
                    path->resize(copied);
                    ok = !path->empty();
                }
                GlobalUnlock(stg.hGlobal);
            }
        }
        ReleaseStgMedium(&stg);
        return ok;
    }

    LONG m_ref = 1;
    PlayerWindow* m_window = nullptr;
};

// 注册/注销拖放目标。定义放在 DropTarget 之后，Create()/Destroy() 通过头文件里的
// 声明调用即可，不需要把整段 COM 实现搬到文件前面。
void PlayerWindow::RegisterFileDrop() {
    if (!m_hwnd || m_dropTarget) {
        return;
    }
    auto* target = new DropTarget(this);
    // RegisterDragDrop 需要先 OleInitialize（App::Run 里已完成）。
    if (FAILED(RegisterDragDrop(m_hwnd, target))) {
        target->Release();
        return; // 注册失败不影响其它功能，只是不能拖放
    }
    m_dropTarget = target;
}

void PlayerWindow::UnregisterFileDrop() {
    if (!m_dropTarget) {
        return;
    }
    if (m_hwnd) {
        RevokeDragDrop(m_hwnd);
    }
    m_dropTarget->Release();
    m_dropTarget = nullptr;
}


void PlayerWindow::UpdateTimeLabel() {
    // 当前时间与总时长分成两个标签：左边当前、右边总时长，不再挤进进度条里。
    const auto fmt = [](double sec, wchar_t* buf, size_t n) {
        long s = sec < 0 ? 0 : static_cast<long>(sec);
        swprintf_s(buf, n, L"%02ld:%02ld", s / 60, s % 60);
    };
    wchar_t cur[16] = {};
    wchar_t tot[16] = {};
    fmt(m_positionSec, cur, 16);
    fmt(m_durationSec, tot, 16);
    if (m_hTime) {
        SetWindowTextW(m_hTime, cur);
    }
    if (m_hDuration) {
        SetWindowTextW(m_hDuration, tot);
    }
}

void PlayerWindow::OnProgressThumbTrack() {
    m_draggingProgress = true;
    KillTimer(m_hwnd, kTimerId);
    const int pos = static_cast<int>(SendMessageW(m_hProgress, TBM_GETPOS, 0, 0));
    if (m_durationSec > 0.0) {
        m_positionSec = (pos / static_cast<double>(kProgressMax)) * m_durationSec;
    }
    UpdateTimeLabel();
}

void PlayerWindow::OnProgressEndTrack() {
    const int pos = static_cast<int>(SendMessageW(m_hProgress, TBM_GETPOS, 0, 0));
    m_draggingProgress = false;
    m_app->SeekToFraction(pos / static_cast<double>(kProgressMax));
    StartTimerIfVisible();
}

// --- 控件创建 ------------------------------------------------------------

void PlayerWindow::CreateControls(HWND hwnd) {
    m_dpi = GetDpiForWindow(hwnd);
    if (m_dpi == 0) {
        m_dpi = GetDpiForSystem();
    }
    m_fontTitle = CreateFontW(-MulDiv(10, m_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE,
                              FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    m_font = CreateFontW(-MulDiv(9, m_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                         FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
    m_fontSmall = CreateFontW(-MulDiv(8, m_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                              FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");

    const int M = Scale(kMargin);
    const int clientW = Scale(kClientWidth);
    const int contentW = clientW - M * 2;

    const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD extra,
                          int x, int y, int w, int h, HMENU id,
                          HFONT font) -> HWND {
        HWND ctl = CreateWindowExW(0, cls, text,
                                   WS_CHILD | WS_VISIBLE | extra, x, y, w, h,
                                   hwnd, id, GetModuleHandleW(nullptr), nullptr);
        if (ctl && font) {
            SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return ctl;
    };

    // 第 1 行：文件名，单独占一行，超长省略，不会撑破窗口。
    m_hFile = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, M,
                   Scale(16), contentW, Scale(22),
                   reinterpret_cast<HMENU>(IDC_FILE), m_fontTitle);

    // 第 2 行：状态副标题（小字灰色，与文件名拉开层次）。
    m_hSubtitle = make(L"STATIC", L"未打开文件", SS_LEFT | SS_NOPREFIX, M,
                       Scale(42), contentW, Scale(16),
                       reinterpret_cast<HMENU>(IDC_SUBTITLE), m_fontSmall);

    // 第 3 行：进度条，单独一行，左右各留 18px。
    m_hProgress = make(TRACKBAR_CLASS, L"", TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
                       M, Scale(70), contentW, Scale(26),
                       reinterpret_cast<HMENU>(IDC_PROGRESS), m_font);
    if (m_hProgress) {
        SendMessageW(m_hProgress, TBM_SETRANGE, TRUE, MAKELPARAM(0, kProgressMax));
        SendMessageW(m_hProgress, TBM_SETLINESIZE, 0, 20);
        SendMessageW(m_hProgress, TBM_SETPAGESIZE, 0, 100);
        SendMessageW(m_hProgress, TBM_SETPOS, TRUE, 0);
        EnableWindow(m_hProgress, FALSE);
    }

    // 第 4 行：左「当前时间」右「总时长」，两端对齐。
    const int timeW = Scale(kTitleW);
    m_hTime = make(L"STATIC", L"00:00", SS_LEFT | SS_NOPREFIX, M, Scale(100), timeW,
                   Scale(18), reinterpret_cast<HMENU>(IDC_TIME), m_font);
    m_hDuration = make(L"STATIC", L"00:00", SS_RIGHT | SS_NOPREFIX,
                       M + contentW - timeW, Scale(100), timeW, Scale(18),
                       reinterpret_cast<HMENU>(IDC_DURATION), m_font);

    // 第 5 行：播放/暂停、停止、销毁，居中一行，间距明显。
    // 三个按钮总宽 = 3*84 + 2*20 = 292，仍在内容区 384 之内，无需加宽窗口。
    const int btnW = Scale(84);
    const int btnH = Scale(32);
    const int btnGap = Scale(20);
    const int btnX = (clientW - (btnW * 3 + btnGap * 2)) / 2;
    m_hPlayPause = make(L"BUTTON", L"播放", BS_PUSHBUTTON, btnX, Scale(128), btnW,
                        btnH, reinterpret_cast<HMENU>(IDC_PLAYPAUSE), m_font);
    m_hStop = make(L"BUTTON", L"停止", BS_PUSHBUTTON, btnX + btnW + btnGap,
                   Scale(128), btnW, btnH, reinterpret_cast<HMENU>(IDC_STOP),
                   m_font);
    m_hDestroy = make(L"BUTTON", L"销毁", BS_PUSHBUTTON,
                      btnX + (btnW + btnGap) * 2, Scale(128), btnW, btnH,
                      reinterpret_cast<HMENU>(IDC_DESTROY), m_font);
    EnableWindow(m_hPlayPause, FALSE);
    EnableWindow(m_hStop, FALSE);
    EnableWindow(m_hDestroy, FALSE);

    // 第 6 行：音量，单独一行：标签 + 滑块 + 百分比，互不挤压。
    const int pctW = Scale(64);
    const int volLabelW = Scale(40);
    const int volX = M + volLabelW + Scale(8);
    const int volW = contentW - volLabelW - Scale(8) - pctW;
    make(L"STATIC", L"音量", SS_LEFT | SS_NOPREFIX, M, Scale(180), volLabelW,
         Scale(18), reinterpret_cast<HMENU>(IDC_VOLUME_LABEL), m_font);
    m_hVolume = make(TRACKBAR_CLASS, L"", TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
                     volX, Scale(176), volW, Scale(26),
                     reinterpret_cast<HMENU>(IDC_VOLUME), m_font);
    if (m_hVolume) {
        SendMessageW(m_hVolume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(m_hVolume, TBM_SETLINESIZE, 0, 5);
        SendMessageW(m_hVolume, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(m_hVolume, TBM_SETPOS, TRUE, 80);
    }
    m_hVolumePct = make(L"STATIC", L"80%", SS_RIGHT | SS_NOPREFIX,
                        M + contentW - pctW, Scale(180), pctW, Scale(18),
                        reinterpret_cast<HMENU>(IDC_VOLUME_PCT), m_font);

    SetVolumeUI(m_volume);
}

// --- 托盘右键菜单 ----------------------------------------------------------

void PlayerWindow::ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, kCmdPlayPause, m_playing ? L"暂停" : L"播放");
    AppendMenuW(hMenu, MF_STRING, kCmdStop, L"停止");
    AppendMenuW(hMenu, MF_STRING, kCmdDestroy, L"销毁");
    // 输出到麦克风：勾选后播放器把声音送到虚拟麦克风，取消则切回原设备继续正常听。
    AppendMenuW(hMenu, MF_STRING | (m_app->MicOutputEnabled() ? MF_CHECKED : 0),
                kCmdMicOutput, L"输出到麦克风");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, kCmdShow, L"显示播放器");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, kCmdExit, L"退出");

    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    switch (cmd) {
    case kCmdPlayPause:
        m_app->TogglePlayPause();
        break;
    case kCmdStop:
        m_app->Stop();
        break;
    case kCmdDestroy:
        m_app->DestroyMedia(); // 与 UI「销毁」按钮同一个入口
        break;
    case kCmdMicOutput:
        m_app->ToggleMicOutput();
        break;
    case kCmdShow:
        m_app->ShowPlayerWindow();
        break;
    case kCmdExit:
        m_app->ExitApp();
        break;
    default:
        break;
    }
}

// --- 窗口过程 --------------------------------------------------------------

LRESULT CALLBACK PlayerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<PlayerWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PlayerWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;

    case WM_COMMAND: {
        const UINT id = LOWORD(wp);
        if (id == IDC_PLAYPAUSE) {
            m_app->TogglePlayPause();
        } else if (id == IDC_STOP) {
            m_app->Stop();
        } else if (id == IDC_DESTROY) {
            m_app->DestroyMedia();
        }
        return 0;
    }

    case WM_HSCROLL: {
        if (reinterpret_cast<HWND>(lp) == m_hProgress) {
            switch (LOWORD(wp)) {
            case TB_THUMBTRACK:
                OnProgressThumbTrack();
                break;
            case TB_ENDTRACK:
                OnProgressEndTrack();
                break;
            case TB_LINEUP:
            case TB_LINEDOWN:
            case TB_PAGEUP:
            case TB_PAGEDOWN:
            case TB_TOP:
            case TB_BOTTOM: {
                const int pos =
                    static_cast<int>(SendMessageW(m_hProgress, TBM_GETPOS, 0, 0));
                m_app->SeekToFraction(pos / static_cast<double>(kProgressMax));
                break;
            }
            default:
                break;
            }
        } else if (reinterpret_cast<HWND>(lp) == m_hVolume) {
            const int pos =
                static_cast<int>(SendMessageW(m_hVolume, TBM_GETPOS, 0, 0));
            m_app->SetVolume(pos / 100.0f);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kTimerId) {
            m_app->OnProgressTick();
        }
        return 0;

    case WM_APP_TRAY_MSG:
        switch (lp) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            m_app->ShowPlayerWindow();
            break;
        case WM_RBUTTONUP:
            ShowTrayMenu(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_APP_AUDIO_EVENT:
        // 高 16 位是媒体代号，用于丢弃属于旧媒体的迟到事件。
        m_app->OnAudioEvent(static_cast<AudioEvent>(LOWORD(wp)),
                            static_cast<HRESULT>(lp), HIWORD(wp));
        return 0;

    case WM_COPYDATA: {
        const COPYDATASTRUCT* pcds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (pcds && pcds->dwData == kSilentPlayerCopyDataMagic && pcds->lpData &&
            pcds->cbData >= sizeof(wchar_t)) {
            const std::wstring path(static_cast<const wchar_t*>(pcds->lpData));
            m_app->LoadFile(path);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        // 副标题/时间/百分比用次要灰色，文件名保持正文色，层次更清楚。
        HDC hdc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetBkMode(hdc, TRANSPARENT);
        if (id == IDC_SUBTITLE || id == IDC_TIME || id == IDC_DURATION ||
            id == IDC_VOLUME_PCT) {
            SetTextColor(hdc, RGB(110, 110, 110));
        } else {
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CLOSE:
        // 关闭窗口 = 隐藏，继续后台播放、驻留托盘。
        Hide();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        if (m_font) {
            DeleteObject(m_font);
            m_font = nullptr;
        }
        if (m_fontTitle) {
            DeleteObject(m_fontTitle);
            m_fontTitle = nullptr;
        }
        if (m_fontSmall) {
            DeleteObject(m_fontSmall);
            m_fontSmall = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

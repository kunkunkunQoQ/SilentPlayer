#include "App.h"

#include "AudioDevices.h"

#include <shobjidl.h> // IFileOpenDialog（选择音频文件）
#include "PerAppAudio.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <sstream>
#include <iomanip>

namespace {

std::wstring FileNameFromPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

// 把常见错误码翻译成人话（用户看不懂十六进制）。
std::wstring ExplainError(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
    case 0xC00D36C4u:
        return L"（格式不受支持，或文件已损坏）";
    case 0xC00D36B4u:
        return L"（文件内容与扩展名不符，或编码方式不支持）";
    case 0xC00D36B3u:
        return L"（文件不完整或已损坏）";
    case 0x80070002u:
        return L"（找不到该文件）";
    case 0x80070005u:
        return L"（没有访问权限）";
    case 0x80070020u:
        return L"（文件正被其它程序占用）";
    default:
        return L"";
    }
}

std::wstring HexError(HRESULT hr) {
    std::wostringstream oss;
    oss << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return oss.str();
}

} // namespace

int App::Run(HINSTANCE hInstance, const std::wstring& initialFile,
             const std::wstring& initialCommand) {
    m_hInstance = hInstance;

    // 单实例：已有实例则把文件路径或控制命令转发过去，然后本进程直接退出。
    if (!m_singleInstance.Acquire()) {
        if (!initialCommand.empty()) {
            m_singleInstance.ForwardCommand(initialCommand);
        } else {
            m_singleInstance.ForwardFileToExisting(initialFile);
        }
        return 0;
    }

    // 公共控件（进度条/音量条）。
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // COM 用单线程套间：Media Foundation 需要，窗口的文件拖放（OLE IDropTarget）也需要。
    const HRESULT comHr = OleInitialize(nullptr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"初始化 COM 失败。", L"SilentPlayer", MB_ICONERROR);
        return 1;
    }

    // 创建播放器窗口（隐藏）与托盘。
    if (!m_window.Create(hInstance, this)) {
        MessageBoxW(nullptr, L"创建主窗口失败。", L"SilentPlayer", MB_ICONERROR);
        OleUninitialize();
        return 1;
    }
    m_player.SetEventSink(m_window.Handle(), WM_APP_AUDIO_EVENT);
    m_tray.Initialize(m_window.Handle(), WM_APP_TRAY_MSG);

    // 音频初始化失败只在真正出错时提示（保持静默优先）。
    if (FAILED(m_player.Initialize())) {
        MessageBoxW(m_window.Handle(), L"初始化音频组件失败。", L"SilentPlayer",
                    MB_ICONERROR);
        m_tray.Destroy();
        m_window.Destroy();
        OleUninitialize();
        return 1;
    }

    // 先清除"本应用"的按应用音频路由设置，再延迟加载首个文件。
    // 背景：SteelSeries Sonar 之类的软件会用同一套机制把本播放器路由到虚拟麦克风；
    // 若上次是"输出到麦克风"状态被强制结束（崩溃/任务管理器结束），设置会残留在系统里，
    // 下次启动就会听不到声音。
    // 注意：清除是**异步生效**的，所以不能"清完立刻加载"（来不及，会仍走麦克风），
    // 也不适合"加载后再重载"（重载与首次加载挨太近会偶发失败并报 BADFILE）。
    // 启动时先清一次（大多数情况下这样就够了），并立刻加载首个文件让用户尽快听到声音。
    PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
    if (!initialFile.empty()) {
        LoadFile(initialFile);
        // 再安排一次"修正"：SteelSeries Sonar 这类软件会在**本进程启动时**重新应用它自己的
        // 路由设置（晚于我们上面的清除），所以首次加载后再清一次并重载才能落到系统默认设备。
        // 这里与托盘开关的关闭路径完全一致（清设置 + ReloadMedia），只是延迟到首次加载稳定之后，
        // 避免与首次加载挨太近导致重载失败。
        SetTimer(m_window.Handle(), kOutputFixTimerId, 900, nullptr);
    }

    // 「N 秒无操作自动退出」默认开启：启动即开始 1 秒心跳计时。
    // （取消勾选会 KillTimer 并清零，之后完全不再运行。）
    if (m_autoExitEnabled) {
        SetTimer(m_window.Handle(), kAutoExitTimerId, 1000, nullptr);
    }

    // 消息循环。
    // 键盘快捷键在这里统一拦截：无论焦点在窗口还是某个子控件上都生效
    // （本程序没有文本输入，不存在与输入冲突的问题）。
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && HandleShortcut(msg.wParam)) {
            continue;
        }
        if (msg.message == WM_MOUSEWHEEL) {
            // 滚轮统一调音量：无论光标落在窗口的哪个控件上（进度条 / 音量条 / 按钮）都一样。
            // 顺带避免 trackbar 在进度条上把滚轮当成"挪滑块"，从而意外改掉播放位置。
            const int delta = GET_WHEEL_DELTA_WPARAM(msg.wParam);
            if (delta != 0) {
                AdjustVolume(delta > 0 ? 0.05f : -0.05f);
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    m_tray.Destroy();
    m_window.Destroy();
    m_player.Shutdown();
    m_micPlayer.Shutdown(); // 第二路（没启用过也安全：Shutdown 是幂等的）
    if (comHr == S_OK) {
        OleUninitialize();
    }
    return static_cast<int>(msg.wParam);
}

// --- 命令入口 -------------------------------------------------------------

void App::LoadFile(const std::wstring& path) {
    if (path.empty()) {
        ShowPlayerWindow();
        return;
    }
    if (m_exiting) {
        return;
    }

    const HRESULT hr = m_player.OpenFile(path);
    if (FAILED(hr)) {
        // 打不开的格式：释放失败过程中可能已建立的资源、UI 回到空闲并标注错误，
        // 不影响后续打开其它文件。
        m_player.CloseMedia();
        ResetToIdle();
        SetStatusText(L"无法播放该文件");
        std::wstring msg =
            L"无法打开文件：\n" + path + L"\n\n错误码：" + HexError(hr) + L" " +
            ExplainError(hr);
        MessageBoxW(m_window.Handle(), msg.c_str(), L"SilentPlayer", MB_ICONERROR);
        return;
    }

    m_currentFile = path;
    m_hasFile = true;
    m_window.SetFileName(FileNameFromPath(path));
    SetStatusText(L"正在播放");
    m_window.SetControlsEnabled(true);
    // 自动播放，最终状态以 Started 事件为准。
    m_window.SetPlayState(true);
    m_window.RefreshPlayback(0.0, 0.0, m_player.Duration());
    // 第二路同步打开同一文件（换歌时必须跟上，否则麦克风里还是上一首）。
    if (m_micMode == MicMode::MicAndSpeakers) {
        SyncSecondOutput();
    }
}

void App::TogglePlayPause() {
    if (!m_hasFile || m_exiting) {
        return;
    }
    if (m_player.State() == PlayState::Playing) {
        m_player.Pause();
    } else {
        m_player.Play();
    }
    MirrorToSecondOutput();
}

void App::Stop() {
    if (!m_hasFile || m_exiting) {
        return;
    }
    m_player.Stop();
    MirrorToSecondOutput();
}

// 销毁当前媒体：停止 → 释放 Media Foundation 媒体 → 回到“未打开文件”的空闲状态。
// 与「停止」的区别：停止只停播放、保留媒体与界面信息；销毁会释放媒体并清空界面。
void App::DestroyMedia() {
    if (m_exiting || !m_player.HasFile()) {
        return; // 本来就没有媒体，无事可做
    }
    // CloseMedia：StopSessionSync → 释放 source/拓扑/音量服务 → 重建会话 → 递增媒体代号。
    // 代号递增后，旧媒体的迟到事件（Started/Paused/Stopped/Ended/Error）都会被丢弃。
    m_player.CloseMedia();
    // 第二路也要一起释放，否则它会继续播放/占着虚拟麦克风。
    if (m_micMode == MicMode::MicAndSpeakers) {
        m_micPlayer.CloseMedia();
    }
    ResetToIdle();
}

// 回到空闲状态：UI 与状态清理集中在这里，避免多条路径各写一份而逐渐产生差异。
void App::ResetToIdle() {
    m_hasFile = false;
    m_currentFile.clear();
    m_window.SetPlayState(false);
    m_window.SetControlsEnabled(false);
    m_window.ClearMedia(); // 文件名/状态/进度/时间归零；音量与窗口尺寸不变
}

void App::FixupOutputRouting() {
    // 清除本应用的路由设置并重载，让声音回到系统默认设备（"正常听歌"状态）。
    PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
    ReloadMedia();
}

// 在当前音量上增减（滚轮用），钳制到 0~1。
void App::AdjustVolume(float delta) {
    float v = m_player.GetVolume() + delta;
    v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
    SetVolume(v);
}

// 托盘菜单「打开文件…」：用系统文件对话框选一个音频文件并播放。
void App::OpenFileDialog() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"音频文件", L"*.mp3;*.wav;*.mp4;*.m4a;*.aac;*.flac;*.wma"},
        {L"所有文件", L"*.*"},
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"打开音频文件");
    // 以主窗口为属主；窗口隐藏时也能正常弹出。
    if (FAILED(dialog->Show(m_window.Handle()))) {
        return; // 用户取消
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return;
    }
    const std::wstring file = path;
    CoTaskMemFree(path);
    LoadFile(file);
}

void App::ShowPlayerWindow() {
    m_window.Show();
}

// 重新加载当前媒体，让"按应用音频路由"的新设置生效，并尽量保持"继续听歌"的体验：
// 记住当前位置与播放意图，重开媒体后续上（换输出设备必然有一次短暂断音）。
void App::ReloadMedia() {
    if (!m_hasFile || m_currentFile.empty()) {
        return; // 没有媒体：下次打开时自然生效
    }
    const double pos = m_player.Position();
    const double dur = m_player.Duration();
    // 用"播放意图"判断，而不是事件驱动的 State()：刚重载时 Started 事件还没到，
    // State() 会短暂是 Stopped，用它会把本该继续播放的媒体误判成需要暂停。
    const bool wasPlaying = m_player.WantPlaying();
    const std::wstring path = m_currentFile;

    if (FAILED(m_player.OpenFile(path))) {
        DestroyMedia();
        SetStatusText(L"无法播放该文件");
        return;
    }
    m_hasFile = true;
    m_currentFile = path;
    if (dur > 0.0 && pos > 0.0) {
        m_player.SeekToFraction(pos / dur); // 续上原来的位置
    }
    if (!wasPlaying) {
        m_player.Pause();
    }
    // 主路被重开了一次，第二路也要重新对齐（否则两边会错位/停在旧会话上）。
    SyncSecondOutput();
}

// 「输出到麦克风」开关（**仅麦克风**：你自己听不到，朋友能听到）。
//   开启：把本应用的输出设备切到虚拟麦克风设备——实测只有渲染到该虚拟设备的
//         "渲染侧"，声音才会出现在它的麦克风里（渲染到 Gaming/Media/Aux/Chat 都不会）。
//   关闭：切回系统默认播放设备，恢复正常听歌。
// 与「麦克风 + 播放设备同时输出」互斥：开这个会先关掉那个。
void App::ToggleMicOutput() {
    // 已经是"仅麦克风"→ 取消勾选，回到默认。
    if (m_micMode == MicMode::MicOnly) {
        SwitchToMicMode(MicMode::Default);
        return;
    }
    // 无论当前是默认还是"麦克风+播放设备"，都切到"仅麦克风"（自动关掉另一个）。
    SwitchToMicMode(MicMode::MicOnly);
}
// 统一的输出模式切换入口：三种状态互斥，且**先彻底停掉旧模式**再启用新模式。
// 把互斥逻辑集中在这里，避免两个开关各写一份而逐渐不一致。
bool App::SwitchToMicMode(MicMode mode, bool quiet) {
    if (m_micMode == mode) {
        return true;
    }

    // 1) 停掉旧模式的一切痕迹。
    //    - 第二路解码会话：任何模式下只要还开着就先关掉。
    if (m_micMode == MicMode::MicAndSpeakers) {
        m_micPlayer.CloseMedia();
    }
    //    - 仅麦克风模式留下的"按应用路由"设置：切走时必须清掉，
    //      否则它会继续把本进程的输出钉在麦克风上。
    if (m_micMode == MicMode::MicOnly) {
        PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
        m_micFeedEndpointName.clear();
    }

    m_micMode = mode;

    // 2) 启用新模式。
    if (mode == MicMode::MicOnly) {
        const std::wstring mic = AudioDevices::RecommendedMicName();
        std::wstring id;
        std::wstring name;
        if (!AudioDevices::FindMicFeedEndpoint(mic, &id, &name)) {
            m_micMode = MicMode::Default;
            if (!quiet) {
                MessageBoxW(m_window.Handle(),
                            L"没有找到可用的虚拟麦克风，无法把声音送到麦克风。\n\n"
                            L"需要先安装一个虚拟音频设备（如 VB-Cable / VoiceMeeter）。",
                            L"SilentPlayer · 输出到麦克风", MB_OK | MB_ICONWARNING);
            }
            return false;
        }
        // 用"按应用音频路由"把本进程（主路）的输出改到虚拟麦克风设备。
        // （SteelSeries Sonar 之类的软件用的也是这套机制。）
        if (!PerAppAudio::SetProcessOutputDevice(GetCurrentProcessId(), id)) {
            m_micMode = MicMode::Default;
            if (!quiet) {
                MessageBoxW(m_window.Handle(),
                            L"无法设置本应用的输出设备（当前系统可能不支持该接口）。\n\n"
                            L"可以改为手动把 Windows 默认播放设备切换成虚拟麦克风设备。",
                            L"SilentPlayer · 输出到麦克风", MB_OK | MB_ICONWARNING);
            }
            return false;
        }
        m_micFeedEndpointName = name;
        ReloadMedia(); // 让新的路由设置生效（重建音频会话）
        SetStatusText(m_statusBase); // 副标题加上模式后缀，避免用户以为"没声音了"

        if (!quiet) {
            const std::wstring advice = AudioDevices::BuildMicRoutingAdvice(name);
            MessageBoxW(m_window.Handle(), advice.c_str(),
                        L"SilentPlayer · 输出到麦克风", MB_OK | MB_ICONINFORMATION);
        }
        return true;
    }

    if (mode == MicMode::MicAndSpeakers) {
        // 主路**不动**：仍然是"本应用默认端点"→ 走系统默认播放设备，你自己能听到。
        // 第二路显式钉到虚拟麦克风的渲染侧 → 朋友能听到。
        const std::wstring mic = AudioDevices::RecommendedMicName();
        std::wstring id;
        std::wstring name;
        if (!AudioDevices::FindMicFeedEndpoint(mic, &id, &name)) {
            m_micMode = MicMode::Default;
            if (!quiet) {
                MessageBoxW(m_window.Handle(),
                            L"没有找到可用的虚拟麦克风，无法把声音同时送到麦克风。\n\n"
                            L"需要先安装一个虚拟音频设备（如 VB-Cable / VoiceMeeter）。",
                            L"SilentPlayer · 麦克风 + 播放设备", MB_OK | MB_ICONWARNING);
            }
            return false;
        }
        if (FAILED(m_micPlayer.Initialize())) {
            m_micMode = MicMode::Default;
            if (!quiet) {
                MessageBoxW(m_window.Handle(), L"初始化第二路音频输出失败。",
                            L"SilentPlayer · 麦克风 + 播放设备", MB_OK | MB_ICONERROR);
            }
            return false;
        }
        // 钉死到虚拟麦克风的渲染侧；主路不受影响，继续走默认播放设备。
        m_micPlayer.SetOutputEndpointId(id);
        m_micFeedEndpointName = name;

        // 主路保持默认设备（若之前是"仅麦克风"，上面已清掉路由设置）。
        ReloadMedia(); // 主路按默认端点重开
        // 第二路跟上：打开同一文件、同一位置、同一音量。
        SyncSecondOutput();
        SetStatusText(m_statusBase);

        if (!quiet) {
            const std::wstring advice = AudioDevices::BuildMicRoutingAdvice(name) +
                L"\n\n你现在**自己也听得到**（本模式与「输出到麦克风」不同），"
                L"朋友同样能听到。";
            MessageBoxW(m_window.Handle(), advice.c_str(),
                        L"SilentPlayer · 麦克风 + 播放设备", MB_OK | MB_ICONINFORMATION);
        }
        return true;
    }

    // MicMode::Default：清掉路由设置并重载，回到"正常听歌"。
    PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
    m_micFeedEndpointName.clear();
    ReloadMedia();
    SetStatusText(m_statusBase);
    return true;
}

// 主路的播放控制之后，把同样的动作同步给第二路。
// 两路是**独立解码的两个 MF 会话**，不会有系统级的自动同步，所以每一个控制点都要镜像一次。
// 不在该模式时直接返回，零开销。
void App::MirrorToSecondOutput() {
    if (m_micMode != MicMode::MicAndSpeakers) {
        return;
    }
    // 用"播放意图"而不是事件驱动的 State()：刚 Play/Pause 时事件还没到，
    // State() 会是上一态，镜像就会慢一拍甚至反向（本项目已知坑，见 AI_CONTEXT #14）。
    if (m_player.WantPlaying()) {
        m_micPlayer.Play();
    } else {
        m_micPlayer.Pause();
    }
}

// 让第二路（进麦克风那一路）跟上主路的媒体、位置与音量。
// 主路是"用户听的那一路"，位置/播放状态都以它为准。
void App::SyncSecondOutput() {
    if (m_micMode != MicMode::MicAndSpeakers || !m_hasFile ||
        m_currentFile.empty()) {
        return;
    }
    if (FAILED(m_micPlayer.OpenFile(m_currentFile))) {
        return; // 第二路开不起来不影响主路：用户至少还能听到
    }
    // 音量与位置跟随主路（换设备后必然要重新设一次）。
    m_micPlayer.SetVolume(m_player.GetVolume());
    const double pos = m_player.Position();
    const double dur = m_player.Duration();
    if (pos > 0.0 && dur > 0.0) {
        m_micPlayer.SeekToFraction(pos / dur);
    }
    if (!m_player.WantPlaying()) {
        m_micPlayer.Pause();
    }
}

// 「麦克风 + 播放设备同时输出」开关（你自己听得到，朋友也听得到）。
// 与「输出到麦克风」互斥：开这个会自动关掉那个，反之亦然。
void App::ToggleMicAndSpeakers() {
    if (m_micMode == MicMode::MicAndSpeakers) {
        SwitchToMicMode(MicMode::Default);
        return;
    }
    SwitchToMicMode(MicMode::MicAndSpeakers);
}

// 「360 秒无操作自动退出」开关（托盘菜单可勾选）。
//   开启：启动 1 秒心跳定时器，累计"没有正在播放"的时长，达到阈值就正常退出进程。
//   关闭：停掉定时器并把计数清零 —— 功能完全不运行（默认关闭，不写盘）。
// 判定口径：只看"当前是否正在播放"。暂停 / 停止 / 播放结束回到空闲 / 未打开文件
// 都算"无操作"，累计 360 秒后退出。正在播放时计时立即清零。
void App::ToggleAutoExit() {
    if (m_autoExitEnabled) {
        KillTimer(m_window.Handle(), kAutoExitTimerId);
        m_autoExitEnabled = false;
        m_autoExitIdleSince = 0;
        return;
    }
    m_autoExitEnabled = true;
    m_autoExitIdleSince = 0; // 从下一次心跳开始重新计时
    SetTimer(m_window.Handle(), kAutoExitTimerId, 1000, nullptr);
}

// 自动退出心跳（每 1 秒）。只有功能开启时定时器才存在，因此这里也再确认一次。
void App::OnAutoExitTick() {
    if (!m_autoExitEnabled || m_exiting) {
        return;
    }

    // 用播放意图 + 实际状态共同判定，避免重载瞬间的 Stopped 被误当成"在播"。
    const bool playing =
        m_player.WantPlaying() && m_player.State() == PlayState::Playing;
    if (playing) {
        m_autoExitIdleSince = 0; // 正在播放：清零，重新等下一次空闲
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (m_autoExitIdleSince == 0) {
        m_autoExitIdleSince = now; // 刚进入空闲：开始计时
        return;
    }
    if (now - m_autoExitIdleSince >=
        static_cast<ULONGLONG>(kAutoExitIdleSeconds) * 1000ULL) {
        ExitApp(); // 与托盘「退出」走同一条正常退出路径
    }
}

void App::ExitApp() {
    if (m_exiting) {
        return;
    }
    m_exiting = true;
    // 清掉"按应用音频路由"设置：开着「输出到麦克风」退出时不在系统里留下持久化设置。
    // （"麦克风 + 播放设备"模式用的是显式端点 + NOPERSIST，不写系统设置，无需清理。）
    PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
    if (m_micMode == MicMode::MicAndSpeakers) {
        m_micPlayer.CloseMedia(); // 先停第二路，别让它在退出过程中还出声
    }
    m_tray.Destroy();
    m_window.Hide();
    DestroyWindow(m_window.Handle()); // WM_DESTROY → PostQuitMessage
}

void App::SeekToFraction(double fraction) {
    if (!m_hasFile || m_exiting) {
        return;
    }
    m_player.SeekToFraction(fraction);
    if (m_micMode == MicMode::MicAndSpeakers) {
        m_micPlayer.SeekToFraction(fraction); // 两路一起跳，避免错位
    }
}

void App::SetVolume(float volume) {
    if (m_exiting) {
        return;
    }
    m_player.SetVolume(volume);
    m_window.SetVolumeUI(volume);
    if (m_micMode == MicMode::MicAndSpeakers) {
        m_micPlayer.SetVolume(volume); // 音量也同步（否则两路响度不一致）
    }
}

void App::OnProgressTick() {
    if (m_window.IsVisible()) {
        RefreshProgress();
    }
}

// 状态文本统一入口：副标题带输出模式后缀，托盘提示带文件名与状态。
void App::SetStatusText(const std::wstring& base) {
    m_statusBase = base;
    std::wstring subtitle = base;
    // 只有真正在播放某个文件时才加模式后缀：空闲时"未打开文件 · 输出到麦克风"读起来别扭。
    if (m_hasFile) {
        if (m_micMode == MicMode::MicOnly) {
            subtitle += L" · 输出到麦克风";
        } else if (m_micMode == MicMode::MicAndSpeakers) {
            subtitle += L" · 麦克风 + 播放设备";
        }
    }
    m_window.SetStatus(subtitle);

    std::wstring tip = L"SilentPlayer";
    if (m_hasFile && !m_currentFile.empty()) {
        tip += L" — " + FileNameFromPath(m_currentFile) + L"（" + base + L"）";
    }
    m_tray.SetTooltip(tip);
}

// 相对当前位置快退/快进（秒），钳制在 [0, 时长]。
void App::SeekBy(double deltaSeconds) {
    if (!m_hasFile) {
        return;
    }
    const double duration = m_player.Duration();
    if (duration <= 0.0) {
        return;
    }
    double pos = m_player.Position();
    if (pos < 0.0) {
        pos = 0.0;
    }
    pos += deltaSeconds;
    if (pos < 0.0) {
        pos = 0.0;
    }
    if (pos > duration) {
        pos = duration;
    }
    SeekToFraction(pos / duration);
}

// 键盘快捷键：空格 = 播放/暂停，←/→ = 快退/快进 5 秒，Esc = 隐藏窗口。
bool App::HandleShortcut(WPARAM vk) {
    switch (vk) {
    case VK_SPACE:
        TogglePlayPause();
        return true;
    case VK_LEFT:
        SeekBy(-5.0);
        return true;
    case VK_RIGHT:
        SeekBy(5.0);
        return true;
    case VK_ESCAPE:
        m_window.Hide(); // 与点关闭按钮一样：只是隐藏，继续后台播放
        return true;
    default:
        return false;
    }
}

// 命令行 / IPC 控制命令。便于脚本、快捷键工具、Stream Deck 等外部触发。
// 注意：`micmode=` 是**仅用于诊断/自动化**的入口 —— 输出模式原本只有托盘菜单能切，
// 而菜单自动化必须注入鼠标（探针的 --trayitem 会 SetCursorPos + 合成点击），
// 会抢占用户鼠标。有了这条命令，所有输出模式的验证都不需要碰鼠标。
void App::RunCommand(const std::wstring& command) {
    if (command == L"toggle") {
        TogglePlayPause();
    } else if (command == L"stop") {
        Stop();
    } else if (command == L"show") {
        ShowPlayerWindow();
    } else if (command == L"exit") {
        ExitApp();
    } else if (command.rfind(L"micmode=", 0) == 0) {
        // off = 默认（不送麦克风）/ mic = 仅麦克风 / dual = 麦克风 + 播放设备
        const std::wstring v = command.substr(8);
        if (v == L"off") {
            SwitchToMicMode(MicMode::Default, /*quiet=*/true);
        } else if (v == L"mic") {
            SwitchToMicMode(MicMode::MicOnly, /*quiet=*/true);
        } else if (v == L"dual") {
            SwitchToMicMode(MicMode::MicAndSpeakers, /*quiet=*/true);
        }
        // 非法值忽略（与 volume= 的处理一致，不静默改状态）
    } else if (command.rfind(L"volume=", 0) == 0) {
        // 非法值（如 --volume=abc）忽略，而不是被 _wtoi 当成 0 静音。
        wchar_t* end = nullptr;
        const long percent = wcstol(command.c_str() + 7, &end, 10);
        if (end && end != command.c_str() + 7 && *end == L'\0') {
            const long clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            SetVolume(static_cast<float>(clamped) / 100.0f);
        }
    }
}

// 关闭窗口后的一次性提示：避免用户以为程序已退出（仅本次运行提示一次）。
void App::NotifyHiddenToTray() {
    if (m_closeHintShown) {
        return;
    }
    m_closeHintShown = true;
    m_tray.ShowBalloon(L"SilentPlayer", L"仍在后台播放，左键托盘图标可再次打开。");
}

void App::RefreshProgress() {
    if (!m_hasFile) {
        return;
    }
    const double pos = m_player.Position();
    const double dur = m_player.Duration();
    if (pos < 0.0 || dur <= 0.0) {
        return;
    }
    m_window.RefreshPlayback(pos / dur, pos, dur);
}

// --- 音频事件 -------------------------------------------------------------

void App::OnAudioEvent(AudioEvent ev, HRESULT status, unsigned generation) {
    // 丢弃属于旧媒体的迟到事件：例如 A 的结束事件在 B 已经开始播放后才被处理，
    // 若照单执行会把 B 停掉、把 UI 清空。
    if (generation != static_cast<unsigned>(m_player.MediaGeneration() & 0xFFFFu)) {
        return;
    }

    switch (ev) {
    case AudioEvent::Loaded:
        // 新媒体就绪；时长/音量已由 AudioPlayer 应用。
        m_hasFile = true;
        m_window.SetControlsEnabled(true);
        m_window.RefreshPlayback(0.0, 0.0, m_player.Duration());
        break;

    case AudioEvent::Started:
        m_window.SetPlayState(true);
        SetStatusText(L"正在播放");
        RefreshProgress();
        break;

    case AudioEvent::Paused:
        m_window.SetPlayState(false);
        SetStatusText(L"已暂停");
        break;

    case AudioEvent::Stopped:
        m_window.SetPlayState(false);
        SetStatusText(L"已停止");
        m_window.RefreshPlayback(0.0, 0.0, m_player.Duration());
        break;

    case AudioEvent::Ended:
        // 播放自然结束：与用户点击「销毁」走同一条清理路径，
        // 回到“未打开文件”的空闲状态，但进程继续驻留托盘，不自动播放下一首。
        DestroyMedia();
        break;

    case AudioEvent::Error:
        // 出错同样释放资源并回到空闲状态，保证之后还能正常打开别的文件。
        DestroyMedia();
        SetStatusText(L"播放出错");
        {
            std::wstring msg = L"播放发生错误。\n\n错误码：" + HexError(status) +
                               L" " + ExplainError(status);
            MessageBoxW(m_window.Handle(), msg.c_str(), L"SilentPlayer",
                        MB_ICONERROR);
        }
        break;
    }
}

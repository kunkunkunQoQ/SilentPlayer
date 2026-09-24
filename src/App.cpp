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
}

void App::Stop() {
    if (!m_hasFile || m_exiting) {
        return;
    }
    m_player.Stop();
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
}

// 「输出到麦克风」开关。
//   开启：把播放器输出切到虚拟麦克风设备——实测只有渲染到该虚拟设备的"渲染侧"，
//         声音才会出现在它的麦克风里（渲染到 Gaming/Media/Aux/Chat 都不会）。
//   关闭：切回系统默认播放设备，恢复正常听歌。
void App::ToggleMicOutput() {
    if (m_micOutputEnabled) {
        // 取消勾选：清除"本应用"的输出设备设置 → 回到系统默认设备，继续正常听歌。
        PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
        ReloadMedia();
        m_micOutputEnabled = false;
        m_micFeedEndpointName.clear();
        SetStatusText(m_statusBase); // 去掉副标题里的模式后缀
        return;
    }

    const std::wstring mic = AudioDevices::RecommendedMicName();
    std::wstring id;
    std::wstring name;
    if (!AudioDevices::FindMicFeedEndpoint(mic, &id, &name)) {
        MessageBoxW(m_window.Handle(),
                    L"没有找到可用的虚拟麦克风，无法把声音送到麦克风。\n\n"
                    L"需要先安装一个虚拟音频设备（如 VB-Cable / VoiceMeeter）。",
                    L"SilentPlayer · 输出到麦克风", MB_OK | MB_ICONWARNING);
        return;
    }

    // 用"按应用音频路由"把本进程的输出改到虚拟麦克风设备：
    // 只影响本播放器，不动系统默认设备，也不影响其它应用。
    // （SteelSeries Sonar 之类的软件用的也是这套机制，所以这能覆盖它对我们的设置。）
    if (!PerAppAudio::SetProcessOutputDevice(GetCurrentProcessId(), id)) {
        MessageBoxW(m_window.Handle(),
                    L"无法设置本应用的输出设备（当前系统可能不支持该接口）。\n\n"
                    L"可以改为手动把 Windows 默认播放设备切换成虚拟麦克风设备。",
                    L"SilentPlayer · 输出到麦克风", MB_OK | MB_ICONWARNING);
        return;
    }
    ReloadMedia(); // 让新的路由设置生效（重建音频会话）
    m_micOutputEnabled = true;
    m_micFeedEndpointName = name;
    SetStatusText(m_statusBase); // 副标题加上模式后缀，避免用户以为"没声音了"

    const std::wstring advice = AudioDevices::BuildMicRoutingAdvice(name);
    MessageBoxW(m_window.Handle(), advice.c_str(), L"SilentPlayer · 输出到麦克风",
                MB_OK | MB_ICONINFORMATION);
}

void App::ExitApp() {
    if (m_exiting) {
        return;
    }
    m_exiting = true;
    // 清掉"按应用音频路由"设置：开着「输出到麦克风」退出时不在系统里留下持久化设置。
    PerAppAudio::ClearProcessOutputDevice(GetCurrentProcessId());
    m_tray.Destroy();
    m_window.Hide();
    DestroyWindow(m_window.Handle()); // WM_DESTROY → PostQuitMessage
}

void App::SeekToFraction(double fraction) {
    if (!m_hasFile || m_exiting) {
        return;
    }
    m_player.SeekToFraction(fraction);
}

void App::SetVolume(float volume) {
    if (m_exiting) {
        return;
    }
    m_player.SetVolume(volume);
    m_window.SetVolumeUI(volume);
}

void App::OnProgressTick() {
    if (m_window.IsVisible()) {
        RefreshProgress();
    }
}

// 状态文本统一入口：副标题带"输出到麦克风"模式后缀，托盘提示带文件名与状态。
void App::SetStatusText(const std::wstring& base) {
    m_statusBase = base;
    std::wstring subtitle = base;
    // 只有真正在播放某个文件时才加模式后缀：空闲时"未打开文件 · 输出到麦克风"读起来别扭。
    if (m_micOutputEnabled && m_hasFile) {
        subtitle += L" · 输出到麦克风";
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
void App::RunCommand(const std::wstring& command) {
    if (command == L"toggle") {
        TogglePlayPause();
    } else if (command == L"stop") {
        Stop();
    } else if (command == L"show") {
        ShowPlayerWindow();
    } else if (command == L"exit") {
        ExitApp();
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

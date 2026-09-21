#include "App.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <sstream>
#include <iomanip>

namespace {

std::wstring FileNameFromPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring HexError(HRESULT hr) {
    std::wostringstream oss;
    oss << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return oss.str();
}

} // namespace

int App::Run(HINSTANCE hInstance, const std::wstring& initialFile) {
    m_hInstance = hInstance;

    // 单实例：已有实例则转发文件路径后直接退出。
    if (!m_singleInstance.Acquire()) {
        m_singleInstance.ForwardFileToExisting(initialFile);
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

    // 命令行携带文件则自动加载播放。
    if (!initialFile.empty()) {
        LoadFile(initialFile);
    }

    // 消息循环。
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
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
        m_window.SetStatus(L"无法播放该文件");
        std::wstring msg =
            L"无法打开文件：\n" + path + L"\n\n错误码：" + HexError(hr);
        MessageBoxW(m_window.Handle(), msg.c_str(), L"SilentPlayer", MB_ICONERROR);
        return;
    }

    m_currentFile = path;
    m_hasFile = true;
    m_window.SetFileName(FileNameFromPath(path));
    m_window.SetStatus(L"正在播放");
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

void App::ShowPlayerWindow() {
    m_window.Show();
}

void App::ExitApp() {
    if (m_exiting) {
        return;
    }
    m_exiting = true;
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
        m_window.SetStatus(L"正在播放");
        RefreshProgress();
        break;

    case AudioEvent::Paused:
        m_window.SetPlayState(false);
        m_window.SetStatus(L"已暂停");
        break;

    case AudioEvent::Stopped:
        m_window.SetPlayState(false);
        m_window.SetStatus(L"已停止");
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
        m_window.SetStatus(L"播放出错");
        {
            std::wstring msg = L"播放发生错误。\n\n错误码：" + HexError(status);
            MessageBoxW(m_window.Handle(), msg.c_str(), L"SilentPlayer",
                        MB_ICONERROR);
        }
        break;
    }
}

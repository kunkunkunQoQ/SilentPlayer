#pragma once

#include <windows.h>
#include <string>

#include "AudioPlayer.h"
#include "PlayerWindow.h"
#include "SingleInstance.h"
#include "TrayIcon.h"

// 自定义消息。
constexpr UINT WM_APP_TRAY_MSG = WM_APP + 1;        // 托盘回调消息
constexpr UINT WM_APP_AUDIO_EVENT = WM_APP + 2;     // MF 事件 → UI 线程

// 应用控制器：单实例、窗口、托盘、播放器的组装层。
class App {
public:
    App() = default;
    ~App() = default;

    // 入口：单实例检查 → 初始化 → 消息循环。
    int Run(HINSTANCE hInstance, const std::wstring& initialFile);

    // 由 PlayerWindow 调用的命令入口。
    void LoadFile(const std::wstring& path); // 空路径 = 仅显示窗口
    void TogglePlayPause();
    void Stop();
    // 销毁：停止并彻底释放当前媒体，回到“未打开文件”的空闲状态。
    // 自然播放结束、播放出错、UI「销毁」按钮、托盘「销毁」都走这一个入口。
    // 销毁 ≠ 退出：进程与托盘继续运行。
    void DestroyMedia();
    void ShowPlayerWindow();
    void ExitApp();
    void SeekToFraction(double fraction);
    void SetVolume(float volume);
    void OnProgressTick(); // 定时器：刷新进度

    // MF 事件（UI 线程）。generation 为事件所属媒体代号，用于丢弃旧媒体的迟到事件。
    void OnAudioEvent(AudioEvent ev, HRESULT status, unsigned generation);

private:
    void RefreshProgress();
    // 回到“未打开文件”的空闲状态（清文件引用、清 UI、禁用控件；音量不变）。
    // 释放媒体由调用方负责，销毁/播放结束/出错/打开失败共用这一个重置逻辑。
    void ResetToIdle();

    HINSTANCE m_hInstance = nullptr;
    SingleInstance m_singleInstance;
    AudioPlayer m_player;
    PlayerWindow m_window;
    TrayIcon m_tray;

    std::wstring m_currentFile;
    bool m_hasFile = false;
    bool m_exiting = false;
};

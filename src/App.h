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
    // initialFile：要播放的文件（空 = 只驻留托盘）。
    // initialCommand：控制命令（如 "toggle"），仅在已有实例运行时转发，随后本进程退出。
    int Run(HINSTANCE hInstance, const std::wstring& initialFile,
            const std::wstring& initialCommand = std::wstring());

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
    // 「输出到麦克风」开关：
    //   开启 → 把播放器的输出设备切到虚拟麦克风设备（实测只有渲染到它才会进麦克风），
    //          这样语音里的朋友能听到你放的歌；
    //   关闭 → 切回原来的播放设备（系统默认），你可以继续正常听歌。
    void ToggleMicOutput();
    bool MicOutputEnabled() const { return m_micOutputEnabled; }
    // 打开"选择音频文件"对话框（托盘菜单「打开文件…」）。
    void OpenFileDialog();
    // 启动后修正输出路由：清除"按应用路由"设置并重载，确保默认走系统默认设备。
    void FixupOutputRouting();
    // 启动修正用的定时器 id。
    static constexpr UINT_PTR kOutputFixTimerId = 2;
    // 执行一条来自命令行/IPC 的控制命令：toggle / stop / show / exit / volume=<0-100>。
    void RunCommand(const std::wstring& command);
    // 窗口被隐藏到托盘时的一次性提示（仅本次运行提示一次）。
    void NotifyHiddenToTray();
    void SeekToFraction(double fraction);
    void SetVolume(float volume);
    void OnProgressTick(); // 定时器：刷新进度

    // MF 事件（UI 线程）。generation 为事件所属媒体代号，用于丢弃旧媒体的迟到事件。
    void OnAudioEvent(AudioEvent ev, HRESULT status, unsigned generation);

private:
    void RefreshProgress();
    // 状态副标题 + 托盘提示统一在这里生成（顺带带上"输出到麦克风"模式）。
    void SetStatusText(const std::wstring& base);
    // 在当前音量上增减（滚轮用），自动钳制到 0~1。
    void AdjustVolume(float delta);
    // 键盘快捷键（空格/左右箭头/Esc）。返回 true 表示已处理。
    bool HandleShortcut(WPARAM vk);
    // 相对当前位置快退/快进若干秒（复用已验证的 SeekToFraction）。
    void SeekBy(double deltaSeconds);
    // 回到“未打开文件”的空闲状态（清文件引用、清 UI、禁用控件；音量不变）。
    // 释放媒体由调用方负责，销毁/播放结束/出错/打开失败共用这一个重置逻辑。
    void ResetToIdle();

    HINSTANCE m_hInstance = nullptr;
    SingleInstance m_singleInstance;
    AudioPlayer m_player;
    PlayerWindow m_window;
    TrayIcon m_tray;

    std::wstring m_currentFile;
    // 当前状态基文本（不含"输出到麦克风"后缀），供切换模式时重新拼装。
    std::wstring m_statusBase;
    // 本次运行是否已经提示过"关闭窗口后仍在后台播放"。
    bool m_closeHintShown = false;
    bool m_hasFile = false;
    bool m_exiting = false;
    // 「输出到麦克风」是否已开启（仅本次运行有效，不写盘）。
    bool m_micOutputEnabled = false;
    // 进麦克风的输出端点（开启时记录，供界面显示）。
    std::wstring m_micFeedEndpointName;
    // 重新加载当前媒体，让"按应用音频路由"的新设置生效，并保持播放位置与播放状态。
    // （换输出设备必然要重建音频会话，所以必须重载一次。）
    void ReloadMedia();
};

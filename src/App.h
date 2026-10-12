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
    bool MicOutputEnabled() const { return m_micMode == MicMode::MicOnly; }
    // 「麦克风 + 播放设备同时输出」：两路同时出声 —— 你能听到，朋友也能听到。
    // 与「输出到麦克风」**互斥**：开这个会自动关掉那个，反之亦然。
    void ToggleMicAndSpeakers();
    bool MicAndSpeakersEnabled() const { return m_micMode == MicMode::MicAndSpeakers; }
    // 当前输出模式（互斥三选一：默认 / 仅麦克风 / 麦克风+播放设备）。
    enum class MicMode { Default, MicOnly, MicAndSpeakers };
    // 打开"选择音频文件"对话框（托盘菜单「打开文件…」）。
    void OpenFileDialog();
    // 「N 秒无操作自动退出」开关（托盘菜单可勾选）：
    //   开启 → 启动 1 秒心跳定时器；只要「当前没有正在播放」的状态累计达到
    //          kAutoExitIdleSeconds 秒，就按正常退出路径结束进程（与托盘「退出」一致）。
    //   关闭 → 停掉定时器并清零计数，功能**完全不运行**（默认关闭）。
    void ToggleAutoExit();
    bool AutoExitEnabled() const { return m_autoExitEnabled; }
    // 心跳（每 1 秒一次）。仅在本功能开启时才会被调用。
    void OnAutoExitTick();
    // 自动退出心跳定时器 id；空闲判定阈值（秒）。
    static constexpr UINT_PTR kAutoExitTimerId = 3;
    static constexpr int kAutoExitIdleSeconds = 360;
    // 启动后修正输出路由：清除"按应用路由"设置并重载，确保默认走系统默认设备。
    void FixupOutputRouting();
    // 启动修正用的定时器 id。
    static constexpr UINT_PTR kOutputFixTimerId = 2;
    // 执行一条来自命令行/IPC 的控制命令：toggle / stop / show / exit / volume=<0-100>
    // / micmode=off|mic|dual（最后一个仅用于诊断自动化，见 RunCommand 注释）。
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
    // 输出模式统一切换入口（三种状态互斥；先停旧模式再启用新模式）。
    // quiet = true 时不弹任何模态对话框，失败只改状态并返回 —— 供**自动化/诊断**使用
    // （`--micmode=...`）。模态框会跑自己的消息循环，会卡住无人的自动化测试。
    // 托盘菜单走 quiet = false，保留原来的提示与指引。
    // 返回是否真的切到了目标模式。
    bool SwitchToMicMode(MicMode mode, bool quiet = false);
    // 让第二路（进麦克风那一路）跟上主路的媒体、位置与音量。
    void SyncSecondOutput();
    // 主路播放控制后同步第二路（第二路存在时才做事）。
    void MirrorToSecondOutput();
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
    // 当前输出模式。三种状态互斥：默认 / 仅麦克风 / 麦克风+播放设备。
    // 这是"当前处于哪种输出模式"的**唯一**判据（不再另设 bool，避免两份状态漂移）。
    MicMode m_micMode = MicMode::Default;
    // 「麦克风 + 播放设备同时输出」的第二条解码会话（渲染到虚拟麦克风）。
    // 未启用该模式时不开会话，零开销。
    AudioPlayer m_micPlayer;
    // 进麦克风的输出端点（开启时记录，供界面显示）。
    std::wstring m_micFeedEndpointName;
    // 「N 秒无操作自动退出」是否已开启（仅本次运行有效，不写盘；**默认开启**）。
    bool m_autoExitEnabled = true;
    // 当前这段"没有正在播放"的起始时刻（GetTickCount64）；0 = 未在计时。
    ULONGLONG m_autoExitIdleSince = 0;
    // 重新加载当前媒体，让"按应用音频路由"的新设置生效，并保持播放位置与播放状态。
    // （换输出设备必然要重建音频会话，所以必须重载一次。）
    void ReloadMedia();
};

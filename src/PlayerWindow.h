#pragma once

#include <windows.h>
#include <string>

class App;

// 托盘右键菜单命令。
enum : UINT {
    kCmdPlayPause = 1,
    kCmdStop = 2,
    kCmdDestroy = 5, // 与 App::DestroyMedia() 同一个入口
    kCmdMicOutput = 6, // 输出到麦克风（可勾选）
    kCmdOpenFile = 7,  // 打开文件…（系统文件对话框）
    kCmdShow = 3,
    kCmdExit = 4,
};

// 播放器主窗口：同时也是隐藏的“常驻后台窗口”。
// 默认隐藏；左键托盘或“显示播放器”时显示。
class PlayerWindow {
public:
    bool Create(HINSTANCE hInstance, App* app);
    void Destroy();

    HWND Handle() const { return m_hwnd; }
    bool IsVisible() const;

    void Show();
    void Hide();

    // UI 更新入口（由 App 调用）。
    void SetFileName(const std::wstring& fileName);
    void SetPlayState(bool playing);
    // 文件名下方的状态副标题（正在播放 / 已暂停 / 已停止 / 未打开文件）。
    void SetStatus(const std::wstring& text);
    // 回到“未打开文件”的空闲状态：清文件名、清时间、进度归零。
    void ClearMedia();
    // 播放进度刷新；拖动进度条期间不覆盖用户操作。
    void RefreshPlayback(double fraction, double positionSec, double durationSec);
    void SetVolumeUI(float volume); // 0.0 ~ 1.0
    void SetControlsEnabled(bool enabled);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls(HWND hwnd);
    void ShowTrayMenu(HWND hwnd);
    void UpdateTimeLabel();
    void OnProgressThumbTrack();
    void OnProgressEndTrack();
    void StartTimerIfVisible();

    // 布局按显示器 DPI 缩放，避免高 DPI 下控件再次挤在一起。
    int Scale(int value) const;

    static constexpr UINT kTimerId = 1;
    static constexpr UINT kTimerIntervalMs = 500;
    static constexpr int kProgressMax = 1000;

    enum : UINT {
        IDC_FILE = 1001,
        IDC_PROGRESS = 1002,
        IDC_TIME = 1003,
        IDC_PLAYPAUSE = 1004,
        IDC_STOP = 1005,
        IDC_VOLUME = 1006,
        IDC_VOLUME_PCT = 1007,
        IDC_SUBTITLE = 1008,
        IDC_DURATION = 1009,
        IDC_VOLUME_LABEL = 1010,
        IDC_DESTROY = 1011,
    };

    HWND m_hwnd = nullptr;
    App* m_app = nullptr;

    HWND m_hFile = nullptr;
    HWND m_hSubtitle = nullptr;
    HWND m_hProgress = nullptr;
    HWND m_hTime = nullptr;
    HWND m_hDuration = nullptr;
    HWND m_hPlayPause = nullptr;
    HWND m_hStop = nullptr;
    HWND m_hDestroy = nullptr;

    // 接收“拖文件到窗口”的 OLE 投放目标（CF_HDROP）。见 PlayerWindow.cpp。
    class DropTarget;
    DropTarget* m_dropTarget = nullptr;
    void RegisterFileDrop();
    void UnregisterFileDrop();
    HWND m_hVolume = nullptr;
    HWND m_hVolumePct = nullptr;
    HWND m_hTip = nullptr; // 文件名悬停提示（超长省略后可看全名）
    HFONT m_font = nullptr;      // 正文
    HFONT m_fontTitle = nullptr; // 文件名
    HFONT m_fontSmall = nullptr; // 副标题

    std::wstring m_fileName; // 当前文件名（用于托盘菜单顶部与窗口标题）
    bool m_closeHintShown = false; // 是否已提示过"关闭后仍在后台播放"
    int m_dpi = 96;
    bool m_draggingProgress = false;
    bool m_playing = false;
    double m_positionSec = 0.0;
    double m_durationSec = 0.0;
    float m_volume = 0.8f;
};

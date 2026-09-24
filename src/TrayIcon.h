#pragma once

#include <windows.h>
#include <string>

// 系统托盘图标：左键消息由主窗口处理，右键菜单由主窗口弹出。
class TrayIcon {
public:
    bool Initialize(HWND hwnd, UINT callbackMsg);
    void Destroy();
    void SetTooltip(const std::wstring& text);
    // 托盘气泡提示（一次性说明用，例如"关闭窗口后仍在后台播放"）。
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

private:
    HWND m_hwnd = nullptr;
    UINT m_callbackMsg = 0;
    bool m_added = false;
    HICON m_icon = nullptr;
};

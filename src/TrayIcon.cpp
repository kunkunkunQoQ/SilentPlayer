#include "TrayIcon.h"

#include <shellapi.h>
#include "resource.h"

namespace {
constexpr UINT kTrayIconId = 1;
} // namespace

bool TrayIcon::Initialize(HWND hwnd, UINT callbackMsg) {
    m_hwnd = hwnd;
    m_callbackMsg = callbackMsg;

    m_icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    if (!m_icon) {
        m_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = callbackMsg;
    nid.hIcon = m_icon;
    wcscpy_s(nid.szTip, L"SilentPlayer");

    m_added = Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
    return m_added;
}

void TrayIcon::Destroy() {
    if (m_added) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = m_hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = false;
    }
}

void TrayIcon::SetTooltip(const std::wstring& text) {
    if (!m_added) {
        return;
    }
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!m_added) {
        return;
    }
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_NOSOUND; // 保持静默：不播提示音
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

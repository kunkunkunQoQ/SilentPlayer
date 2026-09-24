#include "SingleInstance.h"

namespace {

constexpr const wchar_t* kMutexName = L"Local\\SilentPlayer_SingleInstance_Mutex";
constexpr int kFindWindowRetries = 20;    // 等待首实例创建窗口
constexpr DWORD kFindWindowRetryDelayMs = 100;

} // namespace

SingleInstance::~SingleInstance() {
    if (m_mutex) {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
    }
}

bool SingleInstance::Acquire() {
    m_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!m_mutex) {
        // 极少数情况下无法创建互斥体：不做阻断，按单实例继续。
        return true;
    }
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

bool SingleInstance::ForwardFileToExisting(const std::wstring& path) {
    return SendToExisting(kSilentPlayerCopyDataMagic, path);
}

bool SingleInstance::ForwardCommand(const std::wstring& command) {
    return SendToExisting(kSilentPlayerCommandMagic, command);
}

bool SingleInstance::SendToExisting(DWORD magic, const std::wstring& payload) {
    HWND hwnd = nullptr;
    for (int i = 0; i < kFindWindowRetries && !hwnd; ++i) {
        hwnd = FindWindowW(kSilentPlayerWindowClass, nullptr);
        if (!hwnd) {
            Sleep(kFindWindowRetryDelayMs);
        }
    }
    if (!hwnd) {
        return false;
    }

    COPYDATASTRUCT cds = {};
    cds.dwData = magic;
    cds.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(payload.c_str());
    return SendMessageW(hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(nullptr),
                        reinterpret_cast<LPARAM>(&cds)) == TRUE;
}

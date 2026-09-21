#pragma once

#include <windows.h>
#include <string>

// WM_COPYDATA 的 dwData 魔数，供第二实例向首实例传递文件路径。
constexpr DWORD kSilentPlayerCopyDataMagic = 0x53504C52; // "SPLR"

// 主窗口类名，用于 FindWindowW 定位已有实例。
constexpr const wchar_t* kSilentPlayerWindowClass = L"SilentPlayerMainWindow";

// 单实例：Named Mutex + WM_COPYDATA。
class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // 返回 true 表示当前是首个实例；false 表示已有实例在运行。
    bool Acquire();

    // 仅在 Acquire() 返回 false 时调用：
    // 把文件路径转发给正在运行的实例；path 为空时仅唤起已有实例显示窗口。
    bool ForwardFileToExisting(const std::wstring& path);

private:
    HANDLE m_mutex = nullptr;
};

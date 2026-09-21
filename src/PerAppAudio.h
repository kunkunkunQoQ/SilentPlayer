#pragma once

#include <windows.h>
#include <string>

// 按应用音频路由（Per-App Audio Routing）。
//
// 作用：把**指定进程**的音频输出重定向到某个设备，而**不改系统默认设备**、
// 不影响其它应用。Windows 的「设置 → 音量合成器 → 每个应用的输出设备」用的就是这套机制，
// SteelSeries Sonar 之类的软件也是用它把特定应用路由到虚拟设备的。
//
// 注意：底层是**未公开**的 COM 接口 `IAudioPolicyConfigFactory`
// （WinRT 激活名 `Windows.Media.Internal.AudioPolicyConfig`），
// 实现细节参考 EarTrumpet（IID 与 vtable 槽位一致）。失败时返回 false，调用方应优雅降级。
class PerAppAudio {
public:
    // 把 pid 的输出设备设为 shortDeviceId（MMDevice 的短 ID，如 {0.0.0.00000000}.{hash}）。
    // 同时设置 eConsole 与 eMultimedia 两个角色。
    static bool SetProcessOutputDevice(DWORD pid, const std::wstring& shortDeviceId);

    // 清除 pid 的输出设备设置，回到系统默认设备。
    static bool ClearProcessOutputDevice(DWORD pid);
};

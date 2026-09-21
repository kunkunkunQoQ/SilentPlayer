#pragma once

#include <string>
#include <vector>

// 音频端点检测（**只读**，不改变任何音频路由）。
//
// 背景：普通用户态程序无法把音频写进麦克风——WASAPI 的采集端点只能被读取
// （没有 IAudioRenderClient），虚拟麦克风必须由内核态音频驱动提供。
// 所以这里只做两件事：
//   1) 枚举系统里已有的输出端点 / 麦克风端点，并标出像虚拟设备的那几个；
//   2) 生成给用户的操作指引（该在语音软件里把麦克风选成哪个）。
struct AudioEndpointInfo {
    std::wstring id;
    std::wstring name;
    bool likelyVirtual = false;  // 名字像虚拟音频设备（由驱动提供）
    bool likelyLoopback = false; // 立体声混音 / What U Hear 之类
};

class AudioDevices {
public:
    // 当前活动的渲染（输出）端点。
    static std::vector<AudioEndpointInfo> RenderEndpoints();
    // 当前活动的采集（麦克风）端点。
    static std::vector<AudioEndpointInfo> CaptureEndpoints();
    // 默认渲染端点的友好名；失败返回空串。
    static std::wstring DefaultRenderName();
    // 组装给用户看的中文指引文本（含检测结果）。
    // activeOutputName 为「输出到麦克风」开启后实际使用的虚拟麦克风设备名。
    static std::wstring BuildMicRoutingAdvice(const std::wstring& activeOutputName);

    // 推荐的虚拟麦克风（与虚拟输出设备同家族的那个；没有则返回第一个虚拟麦克风）。
    static std::wstring RecommendedMicName();

    // 找到"能把声音送进该虚拟麦克风"的输出端点。
    // 实测：虚拟驱动会把同一设备同时暴露在渲染侧和采集侧，**渲染到它的渲染侧**
    // 才会真正出现在该麦克风里（渲染到其它通道都不会）。找到返回 true 并填 id/name。
    static bool FindMicFeedEndpoint(const std::wstring& micName, std::wstring* id,
                                    std::wstring* name);
};

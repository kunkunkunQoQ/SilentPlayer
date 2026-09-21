#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <propidl.h>
#include <wrl/client.h>
#include <string>
#include <atomic>

using Microsoft::WRL::ComPtr;

enum class PlayState { Stopped, Playing, Paused };

// 由 MF 回调线程发往 UI 线程的播放事件。
enum class AudioEvent {
    Loaded = 0, // 新媒体已就绪（时长已知、音量已应用）
    Started,
    Paused,
    Stopped,
    Ended,      // 播放自然结束
    Error,
};

// 音频播放：Windows Media Foundation 媒体会话 + 流式音频渲染器(SAR)。
// 仅使用 Windows 自带解码能力，不引入任何第三方库。
class AudioPlayer : public IMFAsyncCallback {
public:
    AudioPlayer();
    ~AudioPlayer();

    // MFStartup + 创建媒体会话。启动时调用一次。
    HRESULT Initialize();

    // 释放会话并 MFShutdown。退出时调用一次。
    void Shutdown();

    // 打开音频文件并自动播放；会停止当前播放并切换。
    HRESULT OpenFile(const std::wstring& path);

    // 释放当前媒体资源并回到“未打开文件”的空闲状态（保留会话与事件环）。
    // 播放自然结束后由 App 调用，避免继续持有上一首的大型媒体对象。
    void CloseMedia();

    // 当前媒体的代号：每次加载新媒体或关闭媒体时递增。
    // 事件会带上该代号，UI 据此丢弃属于旧媒体的迟到事件。
    unsigned MediaGeneration() const { return m_mediaGen.load(); }

    HRESULT Play();
    HRESULT Pause();
    HRESULT Stop();

    // fraction: 0.0 ~ 1.0，相对总时长。
    HRESULT SeekToFraction(double fraction);

    // volume: 0.0 ~ 1.0。
    HRESULT SetVolume(float volume);
    float GetVolume() const { return m_volume; }

    // 当前位置（秒）；不可用时返回 -1。
    double Position() const;
    double Duration() const { return m_durationSec; }
    PlayState State() const { return m_state; }
    bool HasFile() const { return m_hasFile; }

    // 事件将通过 PostMessage(msg) 发送到 hwnd，wParam = (WPARAM)AudioEvent。
    void SetEventSink(HWND hwnd, UINT msg);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFAsyncCallback
    STDMETHODIMP GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) override;
    STDMETHODIMP Invoke(IMFAsyncResult* pAsyncResult) override;

private:
    HRESULT ResolveMediaSource(const std::wstring& path, IMFMediaSource** ppSource);
    HRESULT BuildTopology(IMFMediaSource* pSource, IMFTopology** ppTopology);
    HRESULT ApplyVolume();

    // 同步停止：调用 IMFMediaSession::Stop 后等待 MESessionStopped。
    // MF 的 Stop 是异步的，不等它完成就 SetTopology，新拓扑会被“排队”，
    // 直到当前播放结束才生效（表现为：UI 已换歌，声音仍是上一首）。
    HRESULT StopSessionSync();

    // 释放媒体对象并清空媒体状态（不含停止）。OpenFile 与 CloseMedia 共用。
    void ReleaseMedia();

    // 媒体会话的创建/销毁。会话会随每次呈现累积内部对象（实测约 4.8MB、
    // 十几个句柄，直到 Shutdown 才释放），因此每次释放媒体时重建一次，
    // 把长期占用封顶；MFStartup/MFShutdown 仍是进程级、只做一次。
    HRESULT CreateSession();
    void DestroySession();

    // 会话时钟当前值（100ns）；不可用时返回 -1。
    // 注意：该时钟是会话级时间线，不会在新呈现开始时自动归零，
    // 因此 Position() 必须减去本次呈现起点的基准值。
    MFTIME SessionClockNow() const;
    void HandleEvent(IMFMediaEvent* pEvent);
    void PostEvent(AudioEvent ev, HRESULT status = S_OK);
    void SetState(PlayState s);

    ComPtr<IMFMediaSession> m_session;
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFSimpleAudioVolume> m_audioVolume;

    // 由 MF 回调线程在 MESessionStopped / MESessionEnded 时置位，
    // UI 线程在切换文件前等待它，确保旧播放真正停止。
    HANDLE m_stopEvent = nullptr;

    std::wstring m_path;
    double m_durationSec = 0.0;
    float m_volume = 0.8f; // 默认音量 80%（仅内存，不持久化）
    PlayState m_state = PlayState::Stopped;
    bool m_hasFile = false;
    bool m_loading = false; // OpenFile 进行中，忽略旧会话的中间事件
    bool m_shutdown = false;

    // 播放位置基准：Position() = 会话时钟 - m_clockBase。
    // 新呈现开始时（MESessionStarted）重新记录，保证换歌后进度从 0 开始。
    std::atomic<MFTIME> m_clockBase{0};
    std::atomic<bool> m_baseValid{false};
    std::atomic<bool> m_captureBase{false};

    // 媒体代号：加载新媒体 / 释放媒体时递增，用于识别迟到的旧媒体事件。
    std::atomic<unsigned> m_mediaGen{0};

    HWND m_sinkHwnd = nullptr;
    UINT m_sinkMsg = 0;

    std::atomic<ULONG> m_refCount{1};
};

#include "AudioPlayer.h"

#include <propvarutil.h>
#include <cstring>

namespace {

constexpr double kHnsPerSecond = 10000000.0;

// 等待 MESessionStopped 的上限。正常停止在数十毫秒内完成，
// 这里只作为兜底，避免极端情况下卡住 UI 线程。
constexpr DWORD kStopWaitTimeoutMs = 2000;

// --- 按文件内容识别媒体类型 ----------------------------------------------
// MF 的字节流处理器按「文件扩展名或 MIME 类型」注册（MSDN: Scheme Handlers and
// Byte-Stream Handlers），扩展名写错时按扩展名解析必然失败。所以这里读文件头判断
// 真实容器，把 MIME 类型通过 MF_BYTESTREAM_CONTENT_TYPE 交给 MF，由内容说了算；
// 扩展名只作为嗅探失败时的兜底。
//
// MIME 字符串必须与 HKLM\SOFTWARE\Microsoft\Windows Media Foundation\
// ByteStreamHandlers 下注册的键一致（本机实测的可用值见 docs/ARCHITECTURE.md）。

bool MatchAt(const unsigned char* buf, DWORD got, const char* sig, int len,
             int offset) {
    if (offset < 0 || len <= 0 || static_cast<DWORD>(offset + len) > got) {
        return false;
    }
    return std::memcmp(buf + offset, sig, static_cast<size_t>(len)) == 0;
}

// 读文件头；成功返回 true（got 为实际读到的字节数）。
bool ReadHeader(const std::wstring& path, unsigned char* buf, DWORD want,
                DWORD* got) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, want, &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    *got = read;
    return true;
}

// 嗅探容器类型；识别不出返回 false（此时回退到按扩展名解析）。
bool SniffMimeType(const std::wstring& path, std::wstring* mime) {
    unsigned char buf[64] = {};
    DWORD got = 0;
    if (!ReadHeader(path, buf, sizeof(buf), &got) || got < 12) {
        return false;
    }

    // ISO-BMFF：MP4 / M4A / MOV / 3GP 都是 [4 字节 box 长度] + "ftyp"
    if (MatchAt(buf, got, "ftyp", 4, 4)) {
        *mime = L"video/mp4";
        return true;
    }
    // Matroska / WebM
    if (buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3) {
        *mime = L"audio/x-matroska";
        return true;
    }
    // RIFF / RF64 容器，且类型为 WAVE
    if ((MatchAt(buf, got, "RIFF", 4, 0) || MatchAt(buf, got, "RF64", 4, 0)) &&
        MatchAt(buf, got, "WAVE", 4, 8)) {
        *mime = L"audio/wav";
        return true;
    }
    // FLAC
    if (MatchAt(buf, got, "fLaC", 4, 0)) {
        *mime = L"audio/flac";
        return true;
    }
    // ASF（WMA / WMV）的固定 16 字节 GUID
    static const unsigned char kAsfGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66,
                                               0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA,
                                               0x00, 0x62, 0xCE, 0x6C};
    if (got >= 16 && std::memcmp(buf, kAsfGuid, 16) == 0) {
        *mime = L"video/x-ms-asf";
        return true;
    }
    // ADTS 裸 AAC：同步字 0xFFF，且 layer 位为 00（MPEG 音频该两位非 0）
    if (buf[0] == 0xFF && (buf[1] & 0xF6) == 0xF0) {
        *mime = L"audio/vnd.dlna.adts";
        return true;
    }
    // ID3v2 标签开头的 MP3
    if (MatchAt(buf, got, "ID3", 3, 0)) {
        *mime = L"audio/mpeg";
        return true;
    }
    // MPEG 音频同步字（0xFFE0 掩码）
    if (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) {
        *mime = L"audio/mpeg";
        return true;
    }
    // AC-3 / E-AC-3
    if (buf[0] == 0x0B && buf[1] == 0x77) {
        *mime = L"audio/eac3";
        return true;
    }
    return false;
}

} // namespace

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    Shutdown();
}

HRESULT AudioPlayer::CreateSession() {
    m_session.Reset();
    HRESULT hr = MFCreateMediaSession(nullptr, &m_session);
    if (FAILED(hr)) {
        return hr;
    }
    hr = m_session->BeginGetEvent(this, nullptr);
    if (FAILED(hr)) {
        m_session->Shutdown();
        m_session.Reset();
        return hr;
    }
    return S_OK;
}

void AudioPlayer::DestroySession() {
    if (m_session) {
        m_session->Shutdown();
        m_session.Reset();
    }
}

HRESULT AudioPlayer::Initialize() {
    if (m_session) {
        return S_OK;
    }
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        return hr;
    }
    m_stopEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = CreateSession();
    if (FAILED(hr)) {
        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
        MFShutdown();
        return hr;
    }
    return S_OK;
}

void AudioPlayer::Shutdown() {
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;
    DestroySession();
    m_source.Reset();
    m_audioVolume.Reset();
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    MFShutdown();
}

void AudioPlayer::SetEventSink(HWND hwnd, UINT msg) {
    m_sinkHwnd = hwnd;
    m_sinkMsg = msg;
}

// --- 播放控制 ------------------------------------------------------------

// 同步停止当前会话：Stop() 是异步的，必须等到 MESessionStopped 才算真正停止。
HRESULT AudioPlayer::StopSessionSync() {
    if (!m_session) {
        return S_FALSE;
    }
    // 会话本来就没在播放时，Stop() 不会再发 MESessionStopped，
    // 直接返回，避免无谓的等待。
    if (m_state == PlayState::Stopped) {
        return S_OK;
    }
    if (m_stopEvent) {
        ResetEvent(m_stopEvent);
    }
    const HRESULT hr = m_session->Stop();
    if (FAILED(hr)) {
        return hr;
    }
    if (m_stopEvent) {
        WaitForSingleObject(m_stopEvent, kStopWaitTimeoutMs);
    }
    return S_OK;
}

// 释放媒体对象并清空媒体状态（不含停止）。OpenFile 与 CloseMedia 共用。
void AudioPlayer::ReleaseMedia() {
    if (m_session) {
        // 关键：ClearTopologies() 只清“队列里等待播放”的拓扑，**不会释放当前拓扑**
        // （MSDN 原文：This method does not clear the current topology ... To remove
        // the current topology, call SetTopology with MFSESSION_SETTOPOLOGY_CLEAR_CURRENT）。
        // 不显式清掉当前拓扑的话，旧 media source / 解码器 / 文件句柄会一直挂着，
        // 表现为每次加载/切歌都泄漏内存与句柄。
        m_session->SetTopology(MFSESSION_SETTOPOLOGY_CLEAR_CURRENT, nullptr);
        m_session->ClearTopologies();
    }
    m_source.Reset();
    m_audioVolume.Reset();
    m_durationSec = 0.0;
    m_hasFile = false;
    m_path.clear();
    m_baseValid = false;
    m_state = PlayState::Stopped;
    ++m_mediaGen; // 媒体身份已改变，之前的事件全部作废

    // 重建媒体会话：MF 的会话会随每次呈现累积内部对象（实测约 4.8MB 私有内存、
    // 十几个句柄，且只有 Shutdown 才释放），重建即可把长期占用封顶。
    // 重建失败不致命：下一次 OpenFile 会因 m_session 为空而返回错误并提示用户。
    if (!m_shutdown) {
        DestroySession();
        CreateSession();
    }
}

// 播放结束/出错后回到空闲状态：先真正停止，再释放媒体资源。
void AudioPlayer::CloseMedia() {
    if (!m_session) {
        return;
    }
    StopSessionSync();
    ReleaseMedia();
}

HRESULT AudioPlayer::OpenFile(const std::wstring& path) {
    if (m_shutdown || !m_session) {
        return MF_E_SHUTDOWN;
    }

    m_loading = true;

    // 1) 真正停止旧播放：等到 MESessionStopped 再继续。
    //    否则会话仍处于运行态，紧接着的 SetTopology 会把新拓扑“排队”，
    //    旧音频会一直播到自然结束，表现为“UI 换歌了但声音没换”。
    HRESULT hr = StopSessionSync();
    if (FAILED(hr)) {
        m_loading = false;
        return hr;
    }

    // 2) 释放旧媒体：清空旧拓扑、旧媒体源与音量服务。
    ReleaseMedia();

    // 3) 加载新文件。
    ComPtr<IMFMediaSource> pSource;
    hr = ResolveMediaSource(path, &pSource);
    if (FAILED(hr)) {
        m_loading = false;
        return hr;
    }

    ComPtr<IMFTopology> pTopology;
    hr = BuildTopology(pSource.Get(), &pTopology);
    if (FAILED(hr)) {
        m_loading = false;
        return hr;
    }

    // MFSESSION_SETTOPOLOGY_IMMEDIATE：即使会话仍被视为运行中，也立即
    // 结束当前呈现并采用新拓扑，不再排队等待旧呈现播完。
    hr = m_session->SetTopology(MFSESSION_SETTOPOLOGY_IMMEDIATE, pTopology.Get());
    if (FAILED(hr)) {
        m_loading = false;
        return hr;
    }

    m_source = pSource;
    m_path = path;
    m_hasFile = true;
    ++m_mediaGen; // 新媒体生效

    // 4) 自动播放：显式从 0 开始，保证进度归零。
    //    （VT_EMPTY 表示“从当前位置”，会沿用上一次呈现的位置。）
    m_captureBase = true;
    m_baseValid = false;
    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    varStart.vt = VT_I8;
    varStart.hVal.QuadPart = 0;
    hr = m_session->Start(&GUID_NULL, &varStart);
    PropVariantClear(&varStart);
    if (FAILED(hr)) {
        m_loading = false;
        return hr;
    }
    return S_OK;
}

HRESULT AudioPlayer::Play() {
    if (!m_session || !m_hasFile) {
        return S_FALSE;
    }
    if (m_state == PlayState::Playing) {
        return S_OK;
    }
    // 从停止态重新开始播放时，需要重新记录位置基准；
    // 暂停后继续播放则沿用原基准，进度才会接着走。
    if (m_state != PlayState::Paused) {
        m_captureBase = true;
        m_baseValid = false;
    }
    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    HRESULT hr = m_session->Start(&GUID_NULL, &varStart);
    PropVariantClear(&varStart);
    return hr;
}

HRESULT AudioPlayer::Pause() {
    if (!m_session || !m_hasFile) {
        return S_FALSE;
    }
    if (m_state == PlayState::Paused) {
        return S_OK;
    }
    return m_session->Pause();
}

HRESULT AudioPlayer::Stop() {
    if (!m_session || !m_hasFile) {
        return S_FALSE;
    }
    return m_session->Stop();
}

HRESULT AudioPlayer::SeekToFraction(double fraction) {
    if (!m_session || !m_hasFile) {
        return S_FALSE;
    }
    if (fraction < 0.0) {
        fraction = 0.0;
    }
    if (fraction > 1.0) {
        fraction = 1.0;
    }
    const MFTIME pos = static_cast<MFTIME>(fraction * m_durationSec * kHnsPerSecond);
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = pos;
    HRESULT hr = m_session->Start(&GUID_NULL, &var);
    PropVariantClear(&var);
    return hr;
}

MFTIME AudioPlayer::SessionClockNow() const {
    if (!m_session) {
        return -1;
    }
    ComPtr<IMFClock> pClock;
    if (FAILED(m_session->GetClock(&pClock))) {
        return -1;
    }
    MFTIME clockTime = 0;
    if (FAILED(pClock->GetCorrelatedTime(0, &clockTime, nullptr))) {
        return -1;
    }
    return clockTime;
}

double AudioPlayer::Position() const {
    if (!m_baseValid.load()) {
        return -1.0; // 本次呈现的基准尚未确定，先不显示进度。
    }
    const MFTIME now = SessionClockNow();
    if (now < 0) {
        return -1.0;
    }
    MFTIME delta = now - m_clockBase.load();
    if (delta < 0) {
        delta = 0;
    }
    double sec = static_cast<double>(delta) / kHnsPerSecond;
    // 播放结束后时钟仍会继续走，钳制在总时长内，避免出现 00:11 / 00:08。
    if (m_durationSec > 0.0 && sec > m_durationSec) {
        sec = m_durationSec;
    }
    return sec;
}

HRESULT AudioPlayer::SetVolume(float volume) {
    if (volume < 0.0f) {
        volume = 0.0f;
    }
    if (volume > 1.0f) {
        volume = 1.0f;
    }
    m_volume = volume;
    return ApplyVolume();
}

HRESULT AudioPlayer::ApplyVolume() {
    if (!m_session) {
        return S_FALSE;
    }
    if (!m_audioVolume) {
        // 必须用 MR_POLICY_VOLUME_SERVICE 才能拿到 IMFSimpleAudioVolume。
        // MR_AUDIO_POLICY_SERVICE 提供的是 IMFAudioPolicy（音量策略：显示名/图标/分组），
        // 用它要 IMFSimpleAudioVolume 会返回 E_NOINTERFACE(0x80004002)，音量静默失效。
        // 依据：Windows SDK 10.0.22621 的 mfidl.h —— IMFSimpleAudioVolume(8880) 紧跟
        // MR_POLICY_VOLUME_SERVICE(8997)，IMFAudioStreamVolume(9009) 紧跟
        // MR_STREAM_VOLUME_SERVICE(9145)，IMFAudioPolicy(9154) 紧跟 MR_AUDIO_POLICY_SERVICE(9299)。
        HRESULT hr = MFGetService(m_session.Get(), MR_POLICY_VOLUME_SERVICE,
                                  IID_PPV_ARGS(&m_audioVolume));
        if (FAILED(hr)) {
            return hr;
        }
    }
    return m_audioVolume->SetMasterVolume(m_volume);
}

// --- 媒体源与拓扑 --------------------------------------------------------

HRESULT AudioPlayer::ResolveMediaSource(const std::wstring& path,
                                        IMFMediaSource** ppSource) {
    ComPtr<IMFSourceResolver> pResolver;
    HRESULT hr = MFCreateSourceResolver(&pResolver);
    if (FAILED(hr)) {
        return hr;
    }

    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;

    // 1) 先按「内容」解析：把文件当字节流，用嗅探出的 MIME 类型交给 MF。
    //    CreateObjectFromByteStream 的 pwszURL 传 nullptr，解析器就不会再看
    //    （可能写错的）扩展名，而是用 MF_BYTESTREAM_CONTENT_TYPE 选处理器。
    //    这样 MP4 内容叫 .mp3、MP3 内容叫 .mp4 都能正确识别。
    std::wstring sniffedMime;
    if (SniffMimeType(path, &sniffedMime)) {
        ComPtr<IMFByteStream> pByteStream;
        hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                          MF_FILEFLAGS_NONE, path.c_str(), &pByteStream);
        if (SUCCEEDED(hr)) {
            // IMFByteStream 本身不是属性存储，但 MF 的文件字节流实现了
            // IMFAttributes，需要先 QI 再设置内容类型（实测 QI 成功）。
            ComPtr<IMFAttributes> pAttrs;
            if (SUCCEEDED(pByteStream.As(&pAttrs))) {
                pAttrs->SetString(MF_BYTESTREAM_CONTENT_TYPE,
                                  sniffedMime.c_str());
            }
            ComPtr<IUnknown> pUnk;
            hr = pResolver->CreateObjectFromByteStream(
                pByteStream.Get(), nullptr, MF_RESOLUTION_MEDIASOURCE, nullptr,
                &objectType, &pUnk);
            if (SUCCEEDED(hr) && objectType == MF_OBJECT_MEDIASOURCE) {
                if (SUCCEEDED(pUnk->QueryInterface(IID_PPV_ARGS(ppSource)))) {
                    return S_OK;
                }
            }
        }
    }

    // 2) 兜底：按扩展名 / URL 解析。嗅探识别不出的格式（以及内容确实与扩展名一致
    //    的常见情况）都靠这一步，保证原有行为不变。
    objectType = MF_OBJECT_INVALID;
    ComPtr<IUnknown> pSourceUnk;
    hr = pResolver->CreateObjectFromURL(path.c_str(), MF_RESOLUTION_MEDIASOURCE,
                                        nullptr, &objectType, &pSourceUnk);
    if (FAILED(hr)) {
        return hr;
    }
    if (objectType != MF_OBJECT_MEDIASOURCE) {
        return MF_E_INVALIDMEDIATYPE;
    }
    return pSourceUnk->QueryInterface(IID_PPV_ARGS(ppSource));
}

HRESULT AudioPlayer::BuildTopology(IMFMediaSource* pSource,
                                   IMFTopology** ppTopology) {
    ComPtr<IMFPresentationDescriptor> pPD;
    HRESULT hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) {
        return hr;
    }

    // 时长（100ns 单位）。
    UINT64 duration = 0;
    if (SUCCEEDED(pPD->GetUINT64(MF_PD_DURATION, &duration)) && duration > 0) {
        m_durationSec = static_cast<double>(duration) / kHnsPerSecond;
    }

    // 找到第一条音频流。
    DWORD streamCount = 0;
    pPD->GetStreamDescriptorCount(&streamCount);
    DWORD audioStreamIndex = 0;
    bool foundAudio = false;
    for (DWORD i = 0; i < streamCount; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> pSD;
        if (FAILED(pPD->GetStreamDescriptorByIndex(i, &selected, &pSD))) {
            continue;
        }
        ComPtr<IMFMediaTypeHandler> pHandler;
        if (FAILED(pSD->GetMediaTypeHandler(&pHandler))) {
            continue;
        }
        GUID majorType = GUID_NULL;
        pHandler->GetMajorType(&majorType);
        if (majorType == MFMediaType_Audio) {
            audioStreamIndex = i;
            foundAudio = true;
            break;
        }
    }
    if (!foundAudio) {
        return MF_E_INVALIDMEDIATYPE;
    }

    // 只保留音频流：SelectStream 不会自动取消其它流，MP4 等容器里
    // 往往同时存在视频流，必须显式取消，否则拓扑解析会把视频也算进去。
    for (DWORD i = 0; i < streamCount; ++i) {
        if (i == audioStreamIndex) {
            hr = pPD->SelectStream(i);
        } else {
            pPD->DeselectStream(i); // 某些源可能不支持，失败可忽略
            hr = S_OK;
        }
        if (FAILED(hr)) {
            return hr;
        }
    }

    // 源节点：按微软标准示例完整设置源、呈现描述符、流描述符与流 ID。
    ComPtr<IMFStreamDescriptor> pStreamDesc;
    BOOL streamSelected = FALSE;
    hr = pPD->GetStreamDescriptorByIndex(audioStreamIndex, &streamSelected,
                                         &pStreamDesc);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IMFTopologyNode> pSourceNode;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSourceNode);
    if (FAILED(hr)) {
        return hr;
    }
    hr = pSourceNode->SetUnknown(MF_TOPONODE_SOURCE, pSource);
    if (FAILED(hr)) {
        return hr;
    }
    hr = pSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD.Get());
    if (FAILED(hr)) {
        return hr;
    }
    hr = pSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pStreamDesc.Get());
    if (FAILED(hr)) {
        return hr;
    }
    hr = pSourceNode->SetUINT32(MF_TOPONODE_STREAMID, audioStreamIndex);
    if (FAILED(hr)) {
        return hr;
    }

    // 输出节点：流式音频渲染器（SAR）。
    ComPtr<IMFActivate> pRendererActivate;
    hr = MFCreateAudioRendererActivate(&pRendererActivate);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IMFTopologyNode> pOutputNode;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutputNode);
    if (FAILED(hr)) {
        return hr;
    }
    hr = pOutputNode->SetObject(pRendererActivate.Get());
    if (FAILED(hr)) {
        return hr;
    }

    // 连接源 → 输出。
    hr = pSourceNode->ConnectOutput(0, pOutputNode.Get(), 0);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IMFTopology> pTopology;
    hr = MFCreateTopology(&pTopology);
    if (FAILED(hr)) {
        return hr;
    }
    pTopology->AddNode(pSourceNode.Get());
    pTopology->AddNode(pOutputNode.Get());

    *ppTopology = pTopology.Detach();
    return S_OK;
}

// --- 媒体会话事件 --------------------------------------------------------

HRESULT AudioPlayer::Invoke(IMFAsyncResult* pAsyncResult) {
    if (!m_session) {
        return S_OK;
    }

    ComPtr<IMFMediaEvent> pEvent;
    HRESULT hr = m_session->EndGetEvent(pAsyncResult, &pEvent);
    if (FAILED(hr)) {
        return hr;
    }

    MediaEventType meType = MEUnknown;
    pEvent->GetType(&meType);
    HRESULT hrStatus = S_OK;
    pEvent->GetStatus(&hrStatus);

    HandleEvent(pEvent.Get());

    // 重新挂接事件。
    if (!m_shutdown) {
        m_session->BeginGetEvent(this, nullptr);
    }
    return S_OK;
}

void AudioPlayer::HandleEvent(IMFMediaEvent* pEvent) {
    MediaEventType meType = MEUnknown;
    pEvent->GetType(&meType);
    HRESULT hrStatus = S_OK;
    pEvent->GetStatus(&hrStatus);


    switch (meType) {
    case MESessionTopologyStatus: {
        UINT32 status = 0;
        pEvent->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &status);
        if (status == MF_TOPOSTATUS_READY) {
            m_loading = false;
            // 时长兜底：若构建拓扑时未知，这里从 PresentationDescriptor 读取。
            if (m_durationSec <= 0.0 && m_source) {
                ComPtr<IMFPresentationDescriptor> pPD;
                if (SUCCEEDED(m_source->CreatePresentationDescriptor(&pPD))) {
                    UINT64 duration = 0;
                    if (SUCCEEDED(pPD->GetUINT64(MF_PD_DURATION, &duration)) &&
                        duration > 0) {
                        m_durationSec = static_cast<double>(duration) / kHnsPerSecond;
                    }
                }
            }
            ApplyVolume(); // 新 SAR 上重新应用音量。
            PostEvent(AudioEvent::Loaded);
        }
        break;
    }
    case MESessionStarted:
        // 新呈现真正开始：记录位置基准，使进度从 0 起算。
        if (m_captureBase.load()) {
            const MFTIME now = SessionClockNow();
            if (now >= 0) {
                m_clockBase = now;
                m_baseValid = true;
            }
            m_captureBase = false;
        }
        SetState(PlayState::Playing);
        PostEvent(AudioEvent::Started);
        break;
    case MESessionPaused:
        if (m_loading) {
            break; // 切换文件期间的旧事件不上报 UI。
        }
        SetState(PlayState::Paused);
        PostEvent(AudioEvent::Paused);
        break;
    case MESessionStopped:
        // 唤醒 StopSessionSync 的等待方：旧播放到这里才真正停止。
        if (m_stopEvent) {
            SetEvent(m_stopEvent);
        }
        if (m_loading) {
            break; // 切换文件期间不上报，避免把新文件的 UI 状态冲掉。
        }
        m_baseValid = false;
        SetState(PlayState::Stopped);
        PostEvent(AudioEvent::Stopped);
        break;
    case MESessionEnded:
        // 播放结束等同于本次呈现结束，唤醒可能正在等待停止的一方。
        if (m_stopEvent) {
            SetEvent(m_stopEvent);
        }
        if (m_loading) {
            break; // 旧文件的结束事件不应影响新文件（否则会误调 Stop）。
        }
        m_baseValid = false; // 停止后时钟仍在走，不再对外报告位置。
        SetState(PlayState::Stopped);
        PostEvent(AudioEvent::Ended);
        break;
    case MEError:
        if (m_stopEvent) {
            SetEvent(m_stopEvent); // 出错也要解除等待，避免卡住切换。
        }
        m_loading = false;
        m_baseValid = false;
        SetState(PlayState::Stopped);
        PostEvent(AudioEvent::Error, hrStatus);
        break;
    default:
        break;
    }
}

void AudioPlayer::SetState(PlayState s) {
    // 加载新文件期间，旧会话的 Stopped/Paused 等中间事件会干扰 UI，
    // 只有新拓扑就绪(Loaded)之后的状态才可信。
    if (m_loading && (s == PlayState::Stopped || s == PlayState::Paused)) {
        return;
    }
    m_state = s;
}

void AudioPlayer::PostEvent(AudioEvent ev, HRESULT status) {
    if (m_sinkHwnd) {
        // 低 16 位：事件类型；高 16 位：媒体代号。
        // UI 用代号丢弃属于旧媒体的迟到事件（例如 A 的结束事件在 B 开始后才被处理）。
        const WORD gen = static_cast<WORD>(m_mediaGen.load() & 0xFFFF);
        PostMessageW(m_sinkHwnd, m_sinkMsg,
                     MAKEWPARAM(static_cast<WORD>(static_cast<int>(ev)), gen),
                     static_cast<LPARAM>(status));
    }
}

// --- COM 接口 ------------------------------------------------------------

STDMETHODIMP AudioPlayer::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(this);
    } else if (riid == IID_IMFAsyncCallback) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) AudioPlayer::AddRef() {
    return ++m_refCount;
}

STDMETHODIMP_(ULONG) AudioPlayer::Release() {
    const ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP AudioPlayer::GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) {
    if (pdwFlags) {
        *pdwFlags = 0;
    }
    if (pdwQueue) {
        *pdwQueue = MFASYNC_CALLBACK_QUEUE_STANDARD;
    }
    return S_OK;
}

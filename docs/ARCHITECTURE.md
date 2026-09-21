# SilentPlayer 架构说明

本文解释代码**为什么这样设计**，而不是只罗列类名。

## 总体结构

```
main.cpp (wWinMain)
   │  SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)
   │  CommandLineToArgvW 解析 [音频文件路径]
   ▼
App::Run
   │  1. SingleInstance::Acquire  —— 已运行则转发文件路径并退出
   │  2. InitCommonControlsEx(ICC_BAR_CLASSES)
   │  3. CoInitializeEx(STA)
   │  4. PlayerWindow::Create（隐藏窗口，逻辑 420×222，按 DPI 缩放）
   │  5. AudioPlayer::Initialize（MFStartup + MFCreateMediaSession）
   │  6. TrayIcon::Initialize（Shell_NotifyIcon NIM_ADD）
   │  7. 命令行带文件 → LoadFile → 自动播放
   ▼
   消息循环 GetMessage → 分发（托盘消息 / MF 事件 / 定时器 / 控件）
```

## 程序启动流程

1. `wWinMain` 设置每显示器 DPI 感知（保证控件清晰），解析命令行。
2. `App::Run` 先做**单实例检查**：`CreateMutexW` 命名互斥体失败说明已有实例，则用 `WM_COPYDATA` 把文件路径发给首实例并立即返回（不创建任何窗口，秒退）。
3. 初始化 COM（MF 需要 STA）、创建隐藏主窗口、初始化 MF 会话与托盘。
4. 若命令行带音频文件，立即 `LoadFile` 自动播放。
5. 进入消息循环。**主窗口创建后即隐藏**（`SW_HIDE`），全程不显示。

设计要点：窗口、会话、托盘一次性创建常驻，避免每次打开文件重复建窗/建会话，保证「再次打开新文件」时切换开销最小。

## 播放器窗口布局（PlayerWindow）

客户端**逻辑尺寸 420 × 222**（96 DPI 基准），所有坐标与尺寸都经 `Scale()` 按显示器 DPI 缩放，
所以在 150% 缩放屏幕上实际是 630 × 333 物理像素，控件与字体一起等比放大，不会再次挤在一起。

```
┌──────────────────────────────────────────┐  18px 左右留白
│  music.mp3                     （10pt 半粗）
│  正在播放                       （8pt 次要灰）
│  ━━━━━━━━━━━━━━━●━━━━━━━━━    （进度条，独占一行，576 宽）
│  01:32                            03:45   （左当前 / 右总时长）
│          暂停    停止    销毁               （居中，间距 20）
│  音量    ━━━━━━━━━━━━━━━      80%          （标签 + 滑块 + 百分比）
└──────────────────────────────────────────┘
```

- 每一类信息独占一行，**控件之间零重叠**（可用 `test/player_state_probe.exe 0 0 --dump` 验证）。
- 按钮行三个按钮：`3 × 84 + 2 × 20 = 292`，仍在内容区 384 之内，**窗口尺寸无需加大**。
- 文件名用 `SS_ENDELLIPSIS`，长文件名显示为 `a_very_long_music_file_name_tha...`，不撑破窗口。
- 状态副标题（`SetStatus`）由 `App` 在事件里更新：正在播放 / 已暂停 / 已停止 / 已播放完 / 未打开文件 / 播放出错。
- 次要信息（副标题、时间、音量百分比）用灰色（RGB 110），与文件名拉开层次；`WM_CTLCOLORSTATIC` 里按控件 ID 区分。
- 进度条拖动期间 `m_draggingProgress` 暂停 500ms 自动刷新，松手后 `SeekToFraction`。

## 音频播放流程（AudioPlayer）

```
OpenFile(path)
  ├─ StopSessionSync()                        真正停止旧播放（见下）
  │     m_session->Stop()  →  WaitForSingleObject(MESessionStopped, 2s)
  ├─ ReleaseMedia()                           释放旧媒体 + 重建会话（见下）
  │     SetTopology(CLEAR_CURRENT, nullptr)   ← 释放“当前”拓扑
  │     ClearTopologies()                     清队列中等待的拓扑
  │     释放 m_source / m_audioVolume，清时长/路径/位置基准
  │     DestroySession() + CreateSession()     把 MF 内部累积的占用封顶
  ├─ ResolveMediaSource → IMFSourceResolver::CreateObjectFromURL(MF_RESOLUTION_MEDIASOURCE)
  ├─ BuildTopology：
  │    选第一条音频流；**其余流显式 DeselectStream**（MP4 含视频轨时必须）
  │    源节点(SOURCESTREAM_NODE)
  │      SetUnknown(SOURCE)
  │      SetUnknown(PRESENTATION_DESCRIPTOR)
  │      SetUnknown(STREAM_DESCRIPTOR)
  │      SetUINT32(STREAMID)
  │      ↑ 这三项 + SelectStream 缺一不可，
  │        否则 MF_E_TOPO_MISSING_PRESENTATION_DESCRIPTOR
  │    输出节点(OUTPUT_NODE) = MFCreateAudioRendererActivate()（SAR，默认播放设备）
  │    ConnectOutput(0, 输出节点, 0)
  ├─ SetTopology(MFSESSION_SETTOPOLOGY_IMMEDIATE, topo)
  └─ Start(&GUID_NULL, &varStart)   varStart.vt = VT_I8, QuadPart = 0（显式从 0 开始）
```

- 会话事件经 `IMFAsyncCallback::Invoke` 收到后 `EndGetEvent` 再重新 `BeginGetEvent`（事件环不中断），并 `PostMessage(WM_APP_AUDIO_EVENT)` 通知 UI 线程。
- `MF_TOPOSTATUS_READY` → 读时长兜底、应用音量、通知 `Loaded`。
- `MESessionStarted / Paused / Stopped / Ended / MEError` 分别映射到 UI 状态。

## 支持的格式（全部走 Media Foundation 原生链路）

**格式判断以文件内容为准，扩展名只作兜底**：MF 的字节流处理器按「文件扩展名或 MIME 类型」
注册（MSDN: Scheme Handlers and Byte-Stream Handlers），扩展名写错时按扩展名解析必然失败
（实测 `CreateObjectFromURL` 对 `mp4content.mp3` 直接失败）。因此 `ResolveMediaSource` 分两段：

```
ResolveMediaSource(path)
  ├─ 1) 读文件头嗅探真实容器 → MIME
  │     命中？→ MFCreateFile 打开成 IMFByteStream
  │              QI IMFAttributes（实测 MF 的文件字节流支持）
  │              SetString(MF_BYTESTREAM_CONTENT_TYPE, mime)
  │              CreateObjectFromByteStream(bs, /*pwszURL=*/nullptr, ...)
  │                ↑ URL 传 nullptr，解析器就不会再看扩展名
  └─ 2) 兜底：CreateObjectFromURL(path)  按扩展名解析（嗅探识别不出时走这里）
```

嗅探规则（`SniffMimeType`，只看文件头 64 字节）：

| 文件头 | 判定 | MIME（必须是本机注册过的键） |
| ---- | ---- | ---- |
| 偏移 4 处 `ftyp` | ISO-BMFF：MP4 / M4A / MOV / 3GP | `video/mp4` |
| `1A 45 DF A3` | Matroska / WebM | `audio/x-matroska` |
| `RIFF`/`RF64` + `WAVE` | WAV | `audio/wav` |
| `fLaC` | FLAC | `audio/flac` |
| ASF 16 字节 GUID | WMA / WMV | `video/x-ms-asf` |
| `0xFFF?` 且 layer=00 | ADTS 裸 AAC | `audio/vnd.dlna.adts` |
| `ID3` | 带 ID3v2 标签的 MP3 | `audio/mpeg` |
| `0xFFE?` | MPEG 音频同步字 | `audio/mpeg` |
| `0B 77` | AC-3 / E-AC-3 | `audio/eac3` |

MIME 字符串必须与 `HKLM\SOFTWARE\Microsoft\Windows Media Foundation\ByteStreamHandlers`
下注册的键一致（本机实测可用的音频类键：`audio/mpeg`、`audio/mp3`、`audio/x-mp3`、
`audio/wav`、`audio/x-wav`、`audio/flac`、`audio/x-flac`、`audio/mp4`、`video/mp4`、
`audio/aac`、`audio/x-aac`、`audio/vnd.dlna.adts`、`audio/x-ms-wma`、`video/x-ms-asf`、
`audio/x-m4a`、`audio/x-matroska`、`audio/eac3` 等）。

> 试过但**不可行**的机制：`CreateObjectFromURL` 的 `pProps` 属性存储里放
> `MF_BYTESTREAM_CONTENT_TYPE` —— 实测仍按扩展名解析、失败（`resolve_probe` 有记录）。

实测本机（Windows 10/11 x64）MF 音频解码器：MP3、MPEG Audio、AAC(+ADTS)、FLAC、WMA/WMAPro、
ALAC、Opus、AC3/EAC3、DTS、TrueHD、AMR、GSM、G711、ADPCM。

| 格式 | 容器处理 | 音频解码 | 实测 |
| ---- | ---- | ---- | ---- |
| MP3 | MF 字节流处理程序 | MP3 Decoder MFT | ✓ |
| WAV | RIFF | PCM 直通 | ✓ |
| MP4 | MPEG-4 源 | AAC Decoder MFT（忽略视频轨） | ✓ |
| M4A | MPEG-4 源 | AAC Decoder MFT | ✓ |
| AAC | ADTS | AAC Decoder MFT | ✓ |
| FLAC | MF 原生 FLAC | FLAC Decoder MFT | ✓ |
| WMA | ASF | WMAudio Decoder MFT | ✓ |

**不需要 FFmpeg，也不需要转码**。MP4 只取音频轨：不创建视频渲染节点、不需要视频窗口、
不产生任何临时文件、不修改原文件（只读打开）。

### 为什么 MP4 必须显式取消视频流？

MSDN 的 `IMFPresentationDescriptor::SelectStream` 只做「选中该流」，**不会取消其它流**
（要取消得自己调 `DeselectStream`）。MP4 里视频流默认可能是选中的（实测 `resolve_probe`
显示 `stream[1] video selected=1`），若不显式取消，拓扑解析会把视频一起算进去。
所以 `BuildTopology` 现在遍历所有流：音频流 `SelectStream`、其余一律 `DeselectStream`。

## 打开文件的三种入口（都汇到同一个 App::LoadFile）

```
命令行参数          ─┐
单实例 IPC(WM_COPYDATA)─┼─→ App::LoadFile(path) ─→ AudioPlayer::OpenFile() ─→ 播放
拖文件到窗口(IDropTarget)┘
```

三个入口共用同一条加载与清理路径，不存在"某个入口另有一套逻辑"。

### 拖文件到窗口（OLE IDropTarget）

- `PlayerWindow::RegisterFileDrop()` 在窗口创建后 `RegisterDragDrop(hwnd, target)`，
  销毁时 `RevokeDragDrop()`；`App::Run` 用 `OleInitialize` 初始化 OLE。
- `PlayerWindow::DropTarget` 实现 `IDropTarget`：
  - `DragEnter`：用 `QueryGetData(CF_HDROP)` 判断能否接受，返回 `DROPEFFECT_COPY` / `DROPEFFECT_NONE`；
  - `Drop`：`GetData(CF_HDROP)` → `GlobalLock` → `DragQueryFileW(0)` → `ReleaseStgMedium`
    → `App::LoadFile(第一个文件)`。
- 多文件拖入只播第一个（SilentPlayer 是单文件播放器）。
- 用 IDropTarget 而不用 `WM_DROPFILES` 的原因见 CHANGELOG：
  WM_DROPFILES 只有 explorer 会发，第三方进程连构造测试消息都会被 User32 拒绝。

### 拖到托盘图标：Windows 层面不可行

通知区域（系统托盘）图标是 explorer 绘制的，光标下的窗口属于 explorer，而 OLE 拖放是按
"光标下窗口是否注册了 IDropTarget" 决定目标的。实测托盘窗口（`TrayNotifyWnd`）上
**没有任何投放目标**（`RegisterDragDrop` 返回 `S_OK`），所以投放到托盘图标上的文件
**任何应用都收不到**。官方文档中通知区域与应用之间也只有鼠标/键盘回调消息。
因此不实现该功能。



## 媒体生命周期与空闲状态

| 时机 | 动作 |
| ---- | ---- |
| 播放自然结束（`MESessionEnded`） | `App::DestroyMedia()` → 释放媒体 → UI 回到「未打开文件」 |
| **用户点击「销毁」**（UI 按钮 / 托盘菜单） | 同一条 `App::DestroyMedia()` 路径，结果与自然结束完全一致 |
| 打开新文件（A → B） | `OpenFile()`：同步停止 A → `ReleaseMedia()` 释放 A → 加载 B → 只显示 B |
| 打开失败（格式不支持） | `CloseMedia()` + `ResetToIdle()` + 状态「无法播放该文件」→ 不影响之后打开其它文件 |
| 播放出错（`MEError`） | 同上，另外弹出错误码 |
| 关闭窗口 | 只隐藏，继续播放 |
| 托盘「退出」 | `Shutdown()`：销毁会话 + `MFShutdown` |

`App::DestroyMedia()` 与 `App::ResetToIdle()`：

```
DestroyMedia()                       // 销毁：停止 + 释放媒体
  ├─ 若 m_exiting 或播放器本来就没有媒体 → 直接返回
  ├─ AudioPlayer::CloseMedia()
  │     StopSessionSync() → 释放 source/拓扑/音量服务 → 重建会话 → ++m_mediaGen
  └─ ResetToIdle()                   // UI 与状态清理只写这一份
        m_hasFile = false
        m_currentFile.clear()
        SetPlayState(false) / SetControlsEnabled(false) / PlayerWindow::ClearMedia()
```

**停止 ≠ 销毁**：`App::Stop()` 只调 `AudioPlayer::Stop()`，保留媒体对象与界面信息
（文件名、总时长仍在，状态显示「已停止」）；`DestroyMedia()` 才会释放媒体并清空界面。
四条路径（销毁 / 自然结束 / 出错 / 打开失败）共用 `ResetToIdle()`，避免清理逻辑各自漂移。

「未打开文件」状态下：文件名清空、窗口标题恢复为 `SilentPlayer`、状态显示「未打开文件」、
时间与进度归零、进度/播放/停止/销毁按钮禁用；**音量保持**、窗口尺寸不变、进程与托盘继续驻留。

## 单 EXE 双架构（x64 Windows + Windows 11 on ARM64）

**发布产物就是一个 x64 的 `SilentPlayer.exe`，不需要第二个文件、不需要用户选择架构。**

| 运行环境 | 运行方式 |
| ---- | ---- |
| Windows x64 | 原生运行 |
| Windows 11 on ARM64 | 系统内置的 **x64 模拟层**（24H2+ 为 Prism）运行 |

为什么这样可行（依据 Microsoft 文档，非推测）：

- 「Windows 11 on Arm supports emulation of both x86 and x64 apps.」（Windows 10 on Arm 只支持 x86 模拟）
- 「For x64 apps ... system binaries are compiled as **Arm64X PE files** that can be loaded into
  both x64 and Arm64 processes from the same location」——所以 x64 进程能完整访问整个 OS，
  包括 Media Foundation / WASAPI / Shell 等全部系统能力，无需特殊代码。

为什么**不用** ARM64EC / ARM64X：

- ARM64EC 的最终 PE 头显示为 `8664 (x64) (ARM64X)`，但代码是 ARM64 指令，
  **只能在 ARM64 上执行**；ARM64X 是「含 ARM64 + ARM64EC 代码、可被 x64 与 Arm64 进程加载」
  的 ARM64 家族 PE。两者都无法在 x64 Windows 上运行，
  因此不满足「一个 EXE 同时兼容 x64 与 ARM64」。
- 若将来要追求 ARM64 原生性能，必须额外产出一个 ARM64EC 版本（= 两个 EXE），
  这违背当前「单 EXE」目标，故不采用。需要时可用 VS 安装
  「MSVC v143 - C++ ARM64/ARM64EC build tools」组件后另建目标。

本项目不存在架构限制（已逐项核查）：无内联汇编、无 intrinsics、无架构条件编译、
无架构/模拟检测 API、无 `/arch:` 选项、无第三方 DLL —— 只用 Windows 系统 API。

> **ARM64 实机运行尚未验证**：开发环境无 ARM64 设备，且本机 MSVC 仅安装 x64/x86 工具链
> （无 arm64/arm64ec 库），无法产出 ARM64 产物做交叉验证。

### 为什么播放结束后要重建会话？

MF 的 `IMFMediaSession` 会随**每次呈现**累积内部对象：实测每次加载/切歌泄漏约 4.8 MB 私有内存
和十几个句柄，且**只有 `Shutdown()` 才释放**（`ClearTopologies` / `CLEAR_CURRENT` / 等待会话事件
都无效，已用隔离探针逐一排除）。因此在 `ReleaseMedia()` 里销毁并重建会话，把占用封顶；
`MFStartup` / `MFShutdown` 是进程级的，仍然只执行一次，所以重建开销很小。

隔离验证（`test/mf_load_cycle_probe.exe`，同一文件循环加载）：

| 模式 | 是否泄漏 |
| ---- | ---- |
| 只 resolve + 释放 | 否 |
| resolve + 建拓扑 + 释放（不碰会话） | 否 |
| 会话 `SetTopology + Start + Stop` | **是（约 4.8MB + 13 句柄/次）** |

### 为什么旧媒体的迟到事件要丢弃？

`MESessionEnded` 等事件是 `PostMessage` 到 UI 线程的，可能在新文件已经开始播放之后才被处理。
若照单执行，A 的结束事件会把 B 停掉并把 B 的 UI 清空。因此事件消息带上「媒体代号」
（`m_mediaGen`，每次加载/释放媒体时递增），`App::OnAudioEvent` 只处理代号匹配的事件。


### 为什么必须先同步停止，再 SetTopology？

`IMFMediaSession::Stop()` 是**异步**的：返回 S_OK 只代表请求被受理，真正停止要等 `MESessionStopped` 事件。
而按 MF 规范，如果会话**当前处于运行态**，`SetTopology` 在没有 `MFSESSION_SETTOPOLOGY_IMMEDIATE` 时会
**把新拓扑排队**，直到当前呈现播完才生效。

最初的实现是 `Stop()` 后立刻 `SetTopology(0, topo)`，于是出现：UI 文件名已换成新歌，声音却还是上一首
（要等上一首播完才切换）。修复方式是两条同时做：

1. `StopSessionSync()` 等到 `MESessionStopped` 再继续 —— 旧播放确实停止、旧拓扑与旧媒体源被释放；
2. `SetTopology` 带 `MFSESSION_SETTOPOLOGY_IMMEDIATE` —— 即使会话仍被视为运行中，也立即结束当前呈现。

等待用的是自动重置事件 `m_stopEvent`，由 MF 回调线程在 `MESessionStopped` / `MESessionEnded` / `MEError`
时 `SetEvent` 唤醒；MF 回调不依赖 UI 线程，因此不会死锁，并有 2s 超时兜底。

### 为什么 Start 要显式传位置 0？

`varStart.vt = VT_EMPTY` 的语义是“**从当前位置开始**”，换歌时会沿用上一次呈现的位置。
显式传 `VT_I8 / 0` 才能保证新文件从开头播放、进度归零。

### 为什么 Start 要传 `&GUID_NULL, &varStart`？

实测 `Start(nullptr, nullptr)` 返回 E_POINTER。规范写法是传 `&GUID_NULL` 与一个 PROPVARIANT，不能传 nullptr。

### 播放位置为什么不能直接用会话时钟？

`GetCorrelatedTime` 返回的是**会话级时间线**，不是当前文件的播放位置：

- 新呈现开始时该时钟**不会归零**，于是换歌后进度会直接显示成上一首的时长（如 `00:05 / 00:08`）；
- 播放结束后时钟**仍继续走**，于是会出现 `00:11 / 00:08`。

因此 `AudioPlayer` 维护位置基准 `m_clockBase`：在 `MESessionStarted`（本次呈现真正开始）时记录时钟值，
`Position() = 会话时钟 − m_clockBase`。基准未就绪时返回 -1，UI 不显示进度；结果再钳制到 `[0, 总时长]`。
`Stopped` / `Ended` / `Error` 之后基准失效，`Paused` 不影响基准（所以暂停/继续后进度接着走）。
从停止态重新播放（`Play()`）时会重新记录基准。

### 为什么不用 `IMFClock::GetTime`？

新 Windows SDK（10.0.22621）中 `IMFClock` 不再暴露 `GetTime`。改用 `GetCorrelatedTime(0, &clockTime, nullptr)`。

### 切换文件期间为什么要屏蔽中间事件？

`m_loading` 为 true 时不向 UI 上报旧会话的 `Stopped` / `Paused` / `Ended`：

- 旧的 `Stopped` 会把新文件的按钮状态冲成“播放”；
- 旧的 `Ended` 会让 `App` 调用 `Stop()`，反而打断刚启动的新播放。

（`Stopped` 事件本身仍会 `SetEvent` 唤醒 `StopSessionSync`，只是不再上报 UI。）

### 音量控制

- 默认 80%，仅内存。
- 通过 `MFGetService(m_session, MR_POLICY_VOLUME_SERVICE)` 拿 `IMFSimpleAudioVolume` 调 `SetMasterVolume`。
- 每次新拓扑 READY 时重新应用一次（新的 SAR 实例需要重新设置）。

#### 为什么必须是 `MR_POLICY_VOLUME_SERVICE`？

Windows SDK 的 `mfidl.h` 里三个服务 GUID 各自对应一个接口，**名字很像但用途完全不同**：

| 服务 GUID | 提供的接口 | mfidl.h 行号 |
| ---- | ---- | ---- |
| `MR_POLICY_VOLUME_SERVICE` | `IMFSimpleAudioVolume`（**会话音量**） | 8997（紧跟 8880 的 `IMFSimpleAudioVolume` 定义） |
| `MR_STREAM_VOLUME_SERVICE` | `IMFAudioStreamVolume`（各声道音量） | 9145 |
| `MR_AUDIO_POLICY_SERVICE` | `IMFAudioPolicy`（音量**策略**：显示名 / 图标 / 分组） | 9299 |

最初写成 `MFGetService(m_session, MR_AUDIO_POLICY_SERVICE, IID_PPV_ARGS(&m_audioVolume))`，
实测返回 **`E_NOINTERFACE`（0x80004002）**、指针为 null —— 也就是 `ApplyVolume()` 每次都失败，
**音量滑块只改了界面文字，实际输出没有任何变化**。这是「能编译、能运行、但静默失效」的典型问题，
只有用环回采集测真实输出幅度才能发现（`test/audio_loopback_probe.exe`）。

改成 `MR_POLICY_VOLUME_SERVICE` 后实测（源音频幅度 0.36621）：

| 滑块 | 会话实际音量 | 环回实测峰值 |
| ---- | ---- | ---- |
| 100% | 1.0000 | 0.36621 |
| 50% | 0.5000 | 0.18311 |
| 20% | 0.2000 | 0.07324 |
| 5% | 0.0500 | 0.01831 |

线性吻合，说明音量链路（滑块 → MF → 音频会话 → 实际输出）真正打通。

## 托盘实现（TrayIcon + PlayerWindow）

- `Shell_NotifyIconW(NIM_ADD)`，图标 ID = 1，工具提示「SilentPlayer」。
- 托盘回调消息 `WM_APP_TRAY_MSG` 送到隐藏主窗口：
  - **左键（WM_LBUTTONUP）**：`ShowWindow(SW_SHOW)` + `SetForegroundWindow` 显示播放器；已显示则不重复创建。
  - **右键（WM_RBUTTONUP + WM_CONTEXTMENU）**：弹出极简菜单（播放/暂停、停止、显示播放器、退出），命令经 `WM_COMMAND` 处理。
- 托盘图标随进程存活，`NIM_DELETE` 仅在真正退出时调用。

设计要点：托盘是**唯一的默认可见入口**，符合「静默 > 打扰」。

## 单实例机制（SingleInstance）

- 命名互斥体 `Local\SilentPlayer_SingleInstance_Mutex`（`CreateMutexW(NULL, TRUE, name)`）。
- 首实例持有互斥体；第二实例 `Acquire` 失败：
  1. `FindWindowW("SilentPlayerMainWindow")` 找到首实例窗口；
  2. 发送 `WM_COPYDATA`，`dwData = 0x53504C52 ("SPLR")`，`lpData` 为文件路径；
  3. 立即返回 0 退出。
- 首实例在 `WM_COPYDATA` 中校验 magic 后调用 `LoadFile`：停止旧播放 → 加载新文件 → 自动播放。

实测：第二实例约 0.15s 退出，全程只有一个进程。

## 进程显示与版本资源（resources/app.rc）

任务管理器「进程」页的显示名优先取 PE 版本资源里的 **FileDescription**，缺失时回退到映像名；
「详细信息」页则始终显示映像名（`SilentPlayer.exe`）。

本项目最初只在 `app.rc` 里放了图标，**没有任何 VERSIONINFO**，实测
`(Get-Item SilentPlayer.exe).VersionInfo` 的 `FileDescription` / `ProductName` / `OriginalFilename`
全为空、`FileVersionRaw = 0.0.0.0`，Windows 拿不到任何产品标识。现已补上标准 `VS_VERSION_INFO`：

| 字段 | 值 |
| ---- | ---- |
| FileDescription | SilentPlayer |
| ProductName | SilentPlayer |
| OriginalFilename | SilentPlayer.exe |
| InternalName | SilentPlayer |
| FileVersion | 1.0.0.0 |
| ProductVersion | 1.0.0.0 |

补充说明：程序**没有可见主窗口**（`MainWindowHandle = 0`，这是「静默优先」的设计），
所以它只会出现在任务管理器的「后台进程」区，不会出现在「应用」区 —— 这是预期行为，不是缺陷。

## 文件打开流程

```
命令行 / WM_COPYDATA / （未来）文件关联
  → App::LoadFile(path)
      → AudioPlayer::OpenFile（见上）
      → 成功：SetFileName(title = "SilentPlayer — xxx")、启用控件、置播放态、刷新进度
      → 失败：MessageBox 显示错误码（唯一允许的打扰）
```

## 新文件传递流程

```
SilentPlayer.exe music2.mp3   (已有实例运行)
  → 第二实例: Mutex 获取失败
  → FindWindow(首实例) → WM_COPYDATA(magic, "music2.mp3")
  → 首实例: 校验 magic → LoadFile("music2.mp3")
  → 停止旧文件 → 切换拓扑 → 自动播放
```

## UI 与 AudioPlayer 的关系

- `PlayerWindow` 只负责控件与窗口消息，不直接碰 MF。
- `App` 是中介：控件消息（播放/暂停/停止/进度/音量）→ 调用 `AudioPlayer`；`AudioPlayer` 事件 → `App::OnAudioEvent` → 刷新 `PlayerWindow`。
- 进度条拖动：`WM_HSCROLL` 期间置 `m_draggingProgress=true` 暂停 500ms 自动刷新，松手后 `SeekToFraction` 并恢复。
- 播放结束 `MESessionEnded`：`App` 调 `m_player.Stop()`，状态复位，窗口留在托盘。

## 主要类职责

| 类 | 职责 |
| ---- | ---- |
| App | 组装、文件加载、事件分发、退出逻辑 |
| AudioPlayer | MF 会话生命周期、拓扑构建、播放控制、音量、事件上报 |
| PlayerWindow | 隐藏主窗口、播放器控件、托盘消息与右键菜单、定时器 |
| TrayIcon | 托盘图标添加/删除/提示文本 |
| SingleInstance | 单实例互斥体 + WM_COPYDATA 转发 |

## Windows API 使用情况

| API | 用途 |
| ---- | ---- |
| MFStartup / MFCreateMediaSession / MFGetService | MF 会话 |
| IMFSourceResolver / IMFMediaSource | 媒体源解析 |
| IMFMediaSession / IMFAsyncCallback | 播放控制与事件 |
| MFCreateAudioRendererActivate (SAR) | 默认播放设备渲染 |
| Shell_NotifyIcon | 托盘 |
| CreateMutex / WM_COPYDATA | 单实例与文件转发 |
| CreateWindowEx / 控件窗口 / SetTimer | 播放器窗口与进度刷新 |
| SetProcessDpiAwarenessContext | DPI 感知 |
| CommandLineToArgvW | 命令行解析 |

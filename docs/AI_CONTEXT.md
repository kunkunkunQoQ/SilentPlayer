# SilentPlayer AI Context

> 供其他 AI 快速理解本项目。持续更新，保持简洁。

## 项目定位

极简、静默、低资源占用的 Windows 音频播放器「小工具」。打开音频文件后自动播放，默认完全不显示 UI，只在系统托盘后台运行；点击托盘图标才显示一个极简播放控制窗口。不是音乐库、不是播放列表工具。

## 当前版本

1.0.0（2026-09-19，x64 Release 单 EXE，约 260 KB）

## 技术栈

- C++17
- Win32 API（窗口、托盘、消息循环、进程通信）
- Windows Media Foundation（IMFMediaSession + IMFSourceResolver + SAR 渲染）
- Windows Core Audio / WASAPI（仅测试探针使用，主程序依赖 MF 即可）
- CMake + MSVC（Visual Studio 2022 / MSVC 14.38，NMake Makefiles 生成器）
- 无任何第三方库；静态链接 CRT（/MT）

## 目标平台

- Windows 10 2004+ / Windows 11，x64
- 最终发布：单个 `SilentPlayer.exe`，不要求安装任何运行环境

## 当前功能

- 打开音频文件并自动播放（命令行参数 / 打开方式 / **把文件拖到播放器窗口**）
- 支持格式（全部 MF 原生，无第三方库）：**MP3 / WAV / MP4（只取音频轨）/ M4A / AAC / FLAC / WMA**，
  理论上还包括 MF 原生解码的 ALAC、Opus、AC3/EAC3、DTS、AMR、GSM、G711、ADPCM 等
- 默认静默：无主窗口、无欢迎页、无 Toast；仅真正出错时弹提示
- 系统托盘：左键显示播放器窗口；右键菜单（播放/暂停、停止、销毁、显示播放器、退出）
- 极简播放器窗口（逻辑 420×222，按 DPI 缩放）：按行分区不拥挤 —— 文件名 / 状态副标题 / 进度条 / 当前时间+总时长 / 居中三按钮（播放/暂停·停止·销毁）/ 音量
- **停止 ≠ 销毁**：停止保留媒体与界面信息（状态「已停止」）；销毁释放媒体并清空界面（回到「未打开文件」）
- **播放结束后自动清理**：与「销毁」同一条路径（`App::DestroyMedia()`）：释放媒体、清空文件名、
  状态回到「未打开文件」、进度与时间归零、按钮禁用；音量保持、窗口尺寸不变、进程继续驻留托盘
- **拖文件到窗口直接播放**（OLE IDropTarget，CF_HDROP）；拖到托盘图标 Windows 层面不可行（托盘窗口属于 explorer，没有投放目标）
- **单 EXE 双架构**：一个 x64 的 SilentPlayer.exe，x64 Windows 原生运行，
  Windows 11 on ARM64 由系统 x64 模拟层运行（不使用 ARM64EC —— 那会失去 x64 兼容性）
- 单实例（Named Mutex + WM_COPYDATA），新文件切换播放
- 关闭窗口 = 隐藏并继续播放；托盘「退出」才退出
- 不保存播放列表/历史/状态
- 带标准 VERSIONINFO：任务管理器「进程」页显示 SilentPlayer，映像名 SilentPlayer.exe

## 当前架构

```
main.cpp (wWinMain, DPI aware, 解析命令行)
  └─ App (组装与消息分发)
       ├─ SingleInstance  单实例互斥体 + WM_COPYDATA 转发
       ├─ PlayerWindow    隐藏主窗口(逻辑 420×222，按 DPI 缩放) + 播放器控件 + 托盘消息处理
       ├─ TrayIcon        Shell_NotifyIcon 托盘图标
       └─ AudioPlayer     Media Foundation 会话/拓扑/播放控制
            └─ 事件: IMFAsyncCallback → PostMessage(WM_APP_AUDIO_EVENT) → App
```

关键消息：`WM_APP_TRAY_MSG`（托盘回调）、`WM_APP_AUDIO_EVENT`（MF 事件）。

## 核心代码位置

| 职责 | 位置 |
| ---- | ---- |
| 入口 / 命令行解析 | src/main.cpp |
| 应用组装 / 文件加载 / 错误提示 | src/App.cpp / App.h |
| MF 会话、拓扑、播放控制 | src/AudioPlayer.cpp / AudioPlayer.h |
| 播放器窗口与控件 | src/PlayerWindow.cpp / PlayerWindow.h |
| 托盘图标 | src/TrayIcon.cpp / TrayIcon.h |
| 单实例 + 文件转发 | src/SingleInstance.cpp / SingleInstance.h |
| 图标 / 版本资源(VERSIONINFO) | resources/ (app.ico, app.rc, resource.h, make_icon.py) |
| 构建配置 | CMakeLists.txt |
| 测试样本与诊断探针 | test/ |
| 播放状态探针（换歌/暂停/拖动/音量/关闭回归） | test/player_state_probe.cpp |
| 窗口截图探针（布局目视检查） | test/window_shot.cpp |
| 环回采集探针（验证真实输出幅度/音量） | test/audio_loopback_probe.cpp |
| 进程内存探针（WS/私有内存/句柄/线程/GDI/USER） | test/proc_mem_probe.cpp |
| MF 编解码器枚举探针 | test/mf_codec_probe.cpp |
| MF 加载/释放循环隔离探针（查泄漏） | test/mf_load_cycle_probe.cpp |
| 源解析机制探针（内容识别 vs 扩展名） | test/resolve_probe.cpp |
| 拖放探针（投放目标检测 / HDROP 自检 / 托盘取证） | test/drop_probe.cpp |
| 合成 OLE 拖放探针（驱动真实 DoDragDrop） | test/drag_probe.cpp |
| 各格式测试样本下载脚本 | test/fetch_samples.py |

## 重要设计决定

1. **窗口常驻隐藏，不销毁**：关闭 = 隐藏 + 继续播放；避免频繁创建窗口，保证托盘交互即时。
2. **单实例**：Named Mutex `Local\SilentPlayer_SingleInstance_Mutex`；第二实例通过 `WM_COPYDATA`（magic `0x53504C52`）把文件路径转发给首实例后立即退出（实测约 0.15s）。
3. **MF 拓扑必须显式选流**：源解析后需 `pPD->SelectStream(音频流)` 并给源节点设置 `MF_TOPONODE_PRESENTATION_DESCRIPTOR` / `MF_TOPONODE_STREAM_DESCRIPTOR` / `MF_TOPONODE_STREAMID`，否则报 `MF_E_TOPO_MISSING_PRESENTATION_DESCRIPTOR`（0xC00D5217）。
4. **`IMFMediaSession::Start` 参数不能传 nullptr**：必须 `Start(&GUID_NULL, &varStart)`，否则返回 E_POINTER（0x80004003）。
5. **换歌必须先同步停止**：`IMFMediaSession::Stop()` 是异步的；不等 `MESessionStopped` 就 `SetTopology`，会话仍被视为运行中，新拓扑会被**排队**到旧呈现播完，表现为“UI 换歌了、声音没换”。做法：`StopSessionSync()`（Stop + 等 `MESessionStopped`，2s 超时）+ `SetTopology(MFSESSION_SETTOPOLOGY_IMMEDIATE, topo)`。
6. **换歌必须显式从 0 开始**：`varStart.vt = VT_EMPTY` 表示“从当前位置开始”，会沿用上一首的位置；必须传 `VT_I8 / 0`，进度才会归零。
7. **播放位置不能用会话时钟原值**：会话时钟是会话级时间线，新呈现不归零、停止后仍继续走（会出现 `00:05 / 00:08`、`00:11 / 00:08`）。用 `m_clockBase`（`MESessionStarted` 时记录）做差，并钳制到 `[0, 总时长]`；`Stopped/Ended/Error` 后基准失效，`Paused` 不影响基准。
8. **时长读取**：用 `IMFClock::GetCorrelatedTime`（`GetTime` 在新 SDK 不存在）。
9. **音量服务必须是 `MR_POLICY_VOLUME_SERVICE`**：`MFGetService(m_session, MR_POLICY_VOLUME_SERVICE)` 才返回 `IMFSimpleAudioVolume`。写成 `MR_AUDIO_POLICY_SERVICE` 会拿到 `IMFAudioPolicy`（策略服务），对 `IMFSimpleAudioVolume` 返回 `E_NOINTERFACE`，**音量静默失效**（界面文字照变、声音不变）。`MF_MEDIASESSION_SERVICE` 导出在新 SDK 已移除。验证音量必须测真实输出幅度（`test/audio_loopback_probe.exe`），不能只看界面或会话峰值。
10. **进度刷新**：500ms 定时器，且**仅窗口可见时**才刷新；拖动进度条期间暂停自动刷新（`m_draggingProgress`）。
11. **播放结束**：`MESessionEnded` → 显式 `Stop()` → 保持托盘驻留，不自动下一首。
12. **切换文件期间屏蔽中间事件**：`m_loading` 为 true 时不向 UI 上报旧会话的 `Stopped/Paused/Ended`，避免冲掉新文件 UI 状态或误调 `Stop()`。
13. **音量默认 80%**，仅内存不持久化。
14. **静态 CRT /MT + 仅系统 DLL**：EXE 无 VC 运行时依赖，单文件可直接运行。
15. **测试探针**（test/）：WASAPI 会话/峰值探针（audio_session_probe）、全端点探针（audio_endpoints_probe）、窗口/对话框探针、托盘定位探针、播放状态探针（player_state_probe，含 `--dump` 布局检查与 `--toggle`）、窗口截图探针（window_shot），供自动化回归，不进主程序。
16. **UI 布局按行分区 + DPI 缩放**：客户端逻辑 420×222，所有坐标经 `Scale()` 乘 DPI 系数；每类信息独占一行，控件零重叠（`player_state_probe --dump` 可验证）。长文件名用 `SS_ENDELLIPSIS`。次要信息（副标题/时间/音量百分比）用灰色区分层次。宁可窗口大一点，也不让控件挤在一起。
17. **必须带 VERSIONINFO**：`resources/app.rc` 里除图标外必须保留 `VS_VERSION_INFO`（FileDescription / ProductName / OriginalFilename 等），否则任务管理器拿不到产品标识。
18. **拓扑里非音频流必须 `DeselectStream`**：`SelectStream` 不会自动取消其它流，MP4 含视频轨时必须显式取消，否则拓扑解析会把视频算进去。MP4 只取音频，不建视频节点、不需要转码。
19. **播放结束/出错必须释放媒体并回到空闲**：`CloseMedia()` + `ClearMedia()`，否则会一直持有上一首的媒体对象与 UI 状态。
20. **释放媒体时重建媒体会话**：MF 会话会随每次呈现累积内部对象（约 4.8MB + 13 句柄/次，只有 `Shutdown()` 才释放），重建可把长期占用封顶；`MFStartup`/`MFShutdown` 仍是进程级只做一次。
21. **事件必须带媒体代号**：事件消息高 16 位是 `m_mediaGen`，`App::OnAudioEvent` 丢弃不匹配的，避免旧媒体的迟到事件影响新播放。
22. **格式判断以内容为准**：`ResolveMediaSource` 先读文件头嗅探容器 → `MFCreateFile` + QI `IMFAttributes` + `SetString(MF_BYTESTREAM_CONTENT_TYPE, mime)` → `CreateObjectFromByteStream(bs, /*url=*/nullptr)`，绕开扩展名；嗅探不出再回退 `CreateObjectFromURL`。注意 `IMFByteStream` 本身不继承 `IMFAttributes`，必须 QI；`CreateObjectFromURL` 的 `pProps` 放 MIME **无效**（实测）。MIME 字符串必须用本机注册过的键。
23. **销毁与播放结束必须共用一条清理路径**：`App::DestroyMedia()`（= `AudioPlayer::CloseMedia()` + `App::ResetToIdle()`）。UI「销毁」按钮与托盘「销毁」都调它，不要写第二套。`ResetToIdle()` 是唯一负责清文件引用 + 清 UI + 禁用控件的地方，打开失败/播放出错也复用它。
24. **发布产物必须是 x64**：x64 Windows 原生 + Windows 11 on ARM64 由系统 x64 模拟层运行，这样一个 EXE 覆盖两者。**ARM64EC / ARM64X 只能跑在 ARM64 上**，会破坏 x64 兼容，禁止用它们做发布产物。本项目无内联汇编/intrinsics/架构条件编译/`/arch:` 选项/第三方 DLL，因此 x64 产物在模拟层下功能完整。
25. **文件拖放用 OLE IDropTarget，不要用 WM_DROPFILES**：WM_DROPFILES 只有 explorer 会发，第三方进程 `PostMessage` 会被 User32 以 ERROR_INVALID_HANDLE 拒绝（实测），无法验证；IDropTarget 是 shell 拖放的实际通道，注册后任何拖放源都能投递。需要 `OleInitialize`（已替代 `CoInitializeEx`），销毁时 `RevokeDragDrop`。
26. **不要尝试让托盘图标接收拖放**：托盘图标由 explorer 绘制，光标下的窗口属于 explorer；实测 `TrayNotifyWnd` 上没有 OLE 投放目标，投放到托盘图标上的文件任何应用都收不到。

## 已知问题

- **SteelSeries Sonar 等虚拟音频设备**：若系统装有 Sonar，其应用路由可能把进程自动分流到非 eConsole 端点（如 Sonar-Media）；实测播放与音量正常，程序不干预（符合「播放器只负责播放」）。验证时需用全端点探针（audio_endpoints_probe）而不是只看默认端点。
- 播放结束后音频会话仍短暂存在（SAR 行为），峰值归 0，无实际输出；程序本身驻留。
- 内存泄漏已修复（会话重建），但句柄仍有约 **+2/次**的缓慢增长（修复前 +18/次）。
  已排除：IPC 路径（0）、会话创建/销毁（0）、只 resolve（0）、只建拓扑（0）、同一拓扑反复起停（≈0）。
  剩余增长来自「每次 SetTopology 新拓扑被会话累积」——会话重建已消除绝大部分（13→2），
  残留约 2 个/次在媒体加载路径内部，无法在不改架构的前提下进一步定位。
  影响：按 +2/次计，连续打开 1000 个文件约 +2000 句柄，不影响正常使用。
- `SingleInstance::ForwardFileToExisting` 在首实例窗口尚未创建时最多重试 2s；若仍失败，第二实例静默退出、该文件被丢弃（未做兜底重试或提示）。
- **ARM64 实机运行尚未验证**：开发环境无 ARM64 设备，本机 MSVC 只装了 x64/x86 工具链
  （无 arm64/arm64ec 库），无法产出 ARM64 产物交叉验证。已核实的是 PE 架构、依赖、
  代码中无架构相关构造，以及 Microsoft 关于 x64 模拟的官方说明。
- Windows 10 on ARM64 无法运行 x64 产物（该系统只提供 x86 模拟）。

## 当前开发任务

- 已完成：换歌不切换播放修复、播放位置基准、UI 布局重排、进程显示、音量修复、
  多格式支持（MP4/M4A/AAC/FLAC/WMA）、播放结束自动清理、内存泄漏修复
- 待用户实机验证：各种格式的实际听感、播放结束后的 UI、长时间内存表现
- 后续：MP3/多格式实机回归、文件关联（需用户主动设置，不默认修改）

## 不允许修改的设计

- 技术栈：C++ / Win32 / Media Foundation，禁止引入 .NET、Qt、Electron、FFmpeg 等。
- 核心体验：静默优先；不显示欢迎页/主窗口；错误才提示。
- 不增加：播放列表、音乐库、歌词、EQ、可视化、网络音乐、账户、遥测、自动更新、数据库。
- 单实例、托盘「退出」才退出、关闭窗口继续播放。
- 最小改动原则（开发.md 第 17 节）。

## 最近修改

2026-09-20（拖文件到窗口直接播放）：

- 新增 `PlayerWindow::DropTarget`（OLE IDropTarget + CF_HDROP），`RegisterFileDrop()`/`UnregisterFileDrop()`；
  `App::Run` 的 `CoInitializeEx` 改为 `OleInitialize`。Drop 里取第一个文件后调 `App::LoadFile()`（与命令行/IPC 同一入口）。
- 托盘拖放：确认 Windows 层面不可行（实测托盘窗口无投放目标），不做。
- 新增探针 `drop_probe`（投放目标检测 / HDROP 自检 / 托盘投放目标取证）、`drag_probe`（合成 OLE 拖放）。
- 验证：`RegisterDragDrop(应用窗口)` 返回 `DRAGDROP_E_ALREADYREGISTERED` 证明目标已注册；
  换 OleInitialize 后回归全过；退出码 0；销毁路径 6 轮无异常。
  **真实鼠标拖放的最终体验需用户实测**（合成拖放无法稳定完成 Drop）。

2026-09-20（新增「销毁」· 单 EXE 双架构确认）：

- 销毁：新增 `App::DestroyMedia()`（UI 按钮 + 托盘菜单共用）+ `App::ResetToIdle()`；
  播放结束/出错/打开失败也复用同一条清理路径。按钮行改为「播放/暂停·停止·销毁」，
  窗口尺寸不变；托盘菜单在「停止」后插入「销毁」。
- 单 EXE 双架构：确认发布产物为 x64 即可同时覆盖 x64 Windows 与 Windows 11 on ARM64
  （系统 x64 模拟层）；ARM64EC/ARM64X 只能跑 ARM64，不采用。CMake 增加架构策略提示与告警。
- 探针：`player_state_probe` 增加 `--destroy` / `--trayitem` / `--traydump`（托盘菜单可用 `MN_GETHMENU` 枚举）。
- 验证：干净重建零错误零警告；PE=`8664 (x64)`、依赖仅 8 个系统 DLL；销毁两条路径结果一致；
  停止≠销毁；A→销毁→B 正常出声；播放/销毁 ×10 内存不增长。**ARM64 实机未验证**。

2026-09-20（按内容识别 · 探针段错误修复）：

- 内容识别：`ResolveMediaSource` 改为「读文件头嗅探容器 → 字节流 + MIME → CreateObjectFromByteStream(url=nullptr)」，
  嗅探失败再回退按扩展名。扩展名写错（MP4 叫 .mp3、MP3 叫 .mp4）现在都能播放；损坏/截断文件仍安全回到空闲。
- 修掉 `mf_load_cycle_probe --mode=restart` 段错误（restart 模式下会话未创建，空指针解引用）。
- 句柄残留针对性排查完成，未改架构，数据记入 TODO。
- 新增 `test/resolve_probe.cpp`；`fetch_samples.py` 增加错误扩展名/损坏样本生成。
- 验证：干净重建零错误零警告；6 种错误扩展名/损坏场景全部符合预期；原文件 md5 未变；回归全过。

2026-09-20（格式扩展 · 播放结束清理 · 内存泄漏修复）：

- 格式：`BuildTopology` 对非音频流显式 `DeselectStream` → MP4（含视频轨）/M4A/AAC/FLAC/WMA 全部原生可播，无第三方库、无转码、无临时文件。
- 清理：新增 `AudioPlayer::CloseMedia()` + `PlayerWindow::ClearMedia()`；播放结束/出错后释放媒体并回到「未打开文件」空闲状态（音量保持、窗口尺寸不变、进程驻留）。
- 防串扰：事件消息带媒体代号（`m_mediaGen`），`App::OnAudioEvent` 丢弃旧媒体的迟到事件。
- 内存：定位到「MF 会话随每次呈现累积（约 4.8MB + 13 句柄/次，只有 Shutdown 才释放）」，
  在 `ReleaseMedia()` 里重建会话封顶；实测切歌 10 次工作集 31.3→32.9 MB 不再增长。
- 新增探针：`mf_codec_probe`、`proc_mem_probe`、`mf_load_cycle_probe`、`fetch_samples.py`。
- 验证：干净重建零错误零警告；7 种格式全部实测通过；切歌 10 次进程数恒为 1；EXE 271,872 字节，无新增 DLL。

2026-09-20（音量修复 + 剩余功能项验证）：

- 修复：音量滑块只改界面文字、实际输出不变 —— `ApplyVolume()` 误用 `MR_AUDIO_POLICY_SERVICE`（返回 `E_NOINTERFACE`），改为 `MR_POLICY_VOLUME_SERVICE`（一行改动，未碰播放逻辑）。
- 新增 `test/audio_loopback_probe.cpp`（WASAPI 环回采集真实输出幅度）；`player_state_probe` 增加 `--seek/--volume/--close` 操作模拟与 `svol` 会话实际音量列。
- 验证：音量端到端线性吻合；拖动进度条、关闭窗口后台继续播放、暂停/继续、切歌全部通过。

2026-09-20（UI 布局 + 进程显示）：

- UI：客户端 420×222（按 DPI 缩放），改为按行分区：文件名 / 状态副标题 / 进度条 / 当前时间+总时长（左右对齐）/ 居中按钮 / 音量；新增状态副标题控件与灰色层次；长文件名 `SS_ENDELLIPSIS`。实测 10 个控件零重叠。
- 进程显示：`resources/app.rc` 补上标准 `VS_VERSION_INFO`（FileDescription/ProductName/OriginalFilename/InternalName/FileVersion/ProductVersion = SilentPlayer / 1.0.0.0）。此前完全没有版本资源，任务管理器拿不到任何产品标识。
- 新增探针：`test/player_state_probe.cpp` 增加 `--dump`（几何 + 重叠检查）；新增 `test/window_shot.cpp`（窗口截图）。
- 验证：Debug/Release 零错误零警告；进程 `Description`/`Product` 由空变为 SilentPlayer；换歌/暂停/继续回归通过。

2026-09-20（换歌不切换播放 · 核心修复）：

- 修复：`OpenFile` 中 `Stop()` 未等 `MESessionStopped` 就 `SetTopology`，新拓扑被排队 → 换歌后声音仍是上一首。改为 `StopSessionSync()` + `MFSESSION_SETTOPOLOGY_IMMEDIATE`。
- 修复：自动播放用 `VT_EMPTY` 会沿用上一首位置 → 改为显式 `VT_I8 / 0`，进度归零。
- 修复：`Position()` 直接用会话时钟，换歌后进度不归零、结束后超过总时长 → 引入 `m_clockBase` 基准 + 钳制。
- 调整：`m_loading` 期间不上报 `Stopped/Paused/Ended` 到 UI。
- 新增：`test/player_state_probe.cpp`（读 UI 状态 + 真实音频峰值 + `--toggle` 模拟播放/暂停）。
- 验证：Debug/Release 零错误零警告；实测换歌进度归零、声音无中断、暂停/继续正常、连切 3 次无死锁。

2026-09-19（首个版本完整实现）：

- 项目从零创建：CMake + Win32 + MF，x64 Release 单 EXE
- 修复：拓扑缺失呈现描述符（SelectStream + PD/SD/STREAMID 属性）
- 修复：Start(nullptr) → E_POINTER，改 `Start(&GUID_NULL, &varStart)`
- 修复：`IMFClock::GetTime` 移除 → `GetCorrelatedTime`
- 全量行为回归通过（播放/静默/单实例/切换/播放结束驻留）

## 后续计划

- 完善 MP3 实测与文件关联（用户主动设置）
- 按需优化内存与启动速度（当前 WS 约 26 MB，CPU 空闲近 0）

# SilentPlayer 能力清单

本文件是**当前版本实际具备的能力**清单，依据 `src/` 源码逐条核对（源码是最终事实来源）。
每条都标注出处；标「不做」的是源码里确实没有的能力，不是待办。

规模：`src/` 8 个 `.cpp` + 7 个 `.h`。

---

## 1. 打开文件的 3 个入口

三个入口最终都调用**同一个** `App::LoadFile(path)`，不存在"某个入口另有一套逻辑"。

| 入口 | 行为 | 出处 |
| ---- | ---- | ---- |
| 命令行 / 打开方式 | 取 `argv[1]` 作为文件路径；不带参数启动则静默驻留托盘，不显示任何窗口 | `main.cpp:19-22` |
| 第二个实例转发 | Named Mutex 判定单实例；第二实例用 `WM_COPYDATA`（魔数 `"SPLR"`）把路径发给首实例后退出。最多重试 20 次 × 100ms 等首实例窗口创建 | `SingleInstance.cpp:5,19,29-43` |
| 拖文件到窗口 | OLE `IDropTarget` + `CF_HDROP`；取第一个文件，多余的忽略 | `PlayerWindow.cpp`（`DropTarget` / `RegisterFileDrop`） |

> 为什么拖放用 `IDropTarget` 而不是 `WM_DROPFILES`：后者只有 explorer 会发，
> 第三方进程连构造测试消息都会被 User32 以 `ERROR_INVALID_HANDLE` 拒绝（实测），见 CHANGELOG。

## 2. 媒体能力（`AudioPlayer`）

### 2.1 格式识别：以内容为准

- 读文件头嗅探真实容器 → 通过 `MF_BYTESTREAM_CONTENT_TYPE` 交给 MF，
  并用 `CreateObjectFromByteStream(pwszURL = nullptr)` **绕开扩展名**；嗅探不出再回退 `CreateObjectFromURL`。
  `AudioPlayer.cpp:50,447-475`
- 嗅探规则（只看前 64 字节）：`ftyp`→MP4、`1A45DFA3`→Matroska、`RIFF/RF64`+`WAVE`→WAV、
  `fLaC`→FLAC、ASF GUID→ASF/WMA、`0xFFF?`(layer=00)→ADTS AAC、`ID3`→MP3、`0xFFE?`→MPEG 音频、`0B77`→AC3
- 实测可播放：**MP3 / WAV / MP4（只取音频轨）/ M4A / AAC / FLAC / WMA**；
  扩展名写错（MP4 叫 `.mp3` 或反向）同样能播

### 2.2 拓扑

- **只保留音频流**：音频流 `SelectStream`，其余流一律 `DeselectStream`
  （MSDN 明确 `SelectStream` 不会自动取消其它流；MP4 含视频轨时必须显式取消）。`AudioPlayer.cpp:527-533`
- 源节点设 `MF_TOPONODE_SOURCE` / `PRESENTATION_DESCRIPTOR` / `STREAM_DESCRIPTOR` / `STREAMID`；
  输出节点用 `MFCreateAudioRendererActivate()`（默认播放设备的 SAR）。`AudioPlayer.cpp:574`

### 2.3 播放控制（公开 API）

| 能力 | 接口 | 说明 |
| ---- | ---- | ---- |
| 打开并自动播放 | `OpenFile(path)` | 显式从 0 开始（`VT_I8/0`），不是"从当前位置" |
| 播放 / 暂停 / 停止 | `Play` / `Pause` / `Stop` | 停止**保留**媒体与界面信息（≠ 销毁） |
| 按比例定位 | `SeekToFraction(0.0~1.0)` | 相对总时长 |
| 音量 | `SetVolume(0.0~1.0)` / `GetVolume()` | 默认 80%，仅内存不持久化 |
| 查询 | `Position()` / `Duration()` / `State()` / `HasFile()` / `MediaGeneration()` | `Position()` 不可用时返回 -1 |

### 2.4 实现要点（决定了上面这些能力为什么可靠）

- **音量直接作用于音频会话**：`MR_POLICY_VOLUME_SERVICE` → `IMFSimpleAudioVolume`。`AudioPlayer.cpp:415-421`
- **换歌真停止**：`StopSessionSync()` 调 `Stop()` 后等 `MESessionStopped` 再换拓扑
  （MF 的 `Stop` 是异步的，不等会导致新拓扑被排队，表现为"UI 换歌但声音还是上一首"）。
- **换拓扑用 `MFSESSION_SETTOPOLOGY_IMMEDIATE`**：立即结束当前呈现。`AudioPlayer.cpp:279`
- **进度基准**：`Position() = 会话时钟 − 本次呈现基准`，换歌归零并钳制到总时长
  （会话时钟是会话级时间线，不会自动归零）。
- **释放媒体时重建媒体会话**：MF 会话会随每次呈现累积内部对象（实测约 4.8MB + 十几个句柄，
  只有 `Shutdown()` 才释放），重建可把长期占用封顶；`MFStartup/MFShutdown` 仍是进程级只做一次。
  `AudioPlayer.cpp:119-143,210-213`
- **事件防串扰**：事件带媒体代号 `m_mediaGen`，UI 丢弃旧媒体的迟到事件
  （否则 A 的 `Ended` 迟到会把新播放的 B 停掉）。

## 3. 窗口 UI（`PlayerWindow`）

- 逻辑尺寸 **420 × 222**，所有坐标经 `Scale()` 按显示器 DPI 缩放（本机 150% → 实际 630 × 333）
- **11 个控件、零重叠**（可用 `test/player_state_probe.exe 0 0 --dump` 复核）：
  文件名（`SS_ENDELLIPSIS` 长名省略）/ 状态副标题 / 进度条 / 当前时间 / 总时长 /
  播放暂停 / 停止 / 销毁 / 音量标签 / 音量滑块 / 音量百分比
- 窗口标题随文件变化（`SilentPlayer — 文件名`），空闲时回到 `SilentPlayer`
- 次要信息（副标题/时间/百分比）用灰色区分层次（`WM_CTLCOLORSTATIC`）；三种字号随 DPI 缩放
- 窗口样式 `WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX`（无最大化按钮）
- **关闭窗口 = 隐藏并继续播放**（`WM_CLOSE` → `Hide()`）
- 进度刷新：500ms 定时器，且**仅窗口可见时**才运行；拖动进度条期间不被定时器覆盖

## 4. 托盘（`TrayIcon` + `PlayerWindow::ShowTrayMenu`）

- 托盘图标 + tooltip
- 左键单击 / 双击 → 显示播放器窗口
- 右键菜单 **7 项**（顺序固定）：

  ```
  打开文件…          (id=7)
  ────────────────
  文件名（灰色禁用项，仅有文件时出现）
  ────────────────
  播放 / 暂停        (id=1，右侧显示快捷键提示)
  停止              (id=2)
  销毁              (id=5)
  输出到麦克风        (id=6，可勾选)
  ────────────────
  显示播放器         (id=3)
  ────────────────
  退出              (id=4)
  ```

  > 菜单项**位置**会随"是否有文件"变化（顶部文件名项时有时无），用 `--traydump` 看实际位置。

- 「销毁」与 UI 的销毁按钮共用 `App::DestroyMedia()` 同一个入口
- **托盘菜单「打开文件…」**：系统文件对话框选音频文件（始终可用）
- **鼠标滚轮调音量**（窗口**任意位置**滚轮 ±5%，含进度条/音量条/按钮上方）、**键盘快捷键**（空格 = 播放/暂停，←/→ = 快退/快进 5 秒，Esc = 隐藏窗口）
- **进度条 / 音量条都"点哪跳哪"**：点轨道任意位置直接跳到该位置（不是只挪一页）
- **托盘左键单击切换显示/隐藏**（双击确保显示）
- 打开失败时**错误码附人话原因**（如 `0xC00D36C4（格式不受支持，或文件已损坏）`）
- **托盘悬停显示当前播放**（`SilentPlayer — 文件名（状态）`）
- **命令行控制命令**：`--toggle` / `--stop` / `--show` / `--exit` / `--volume=<0-100>`
  （由已运行实例执行，可给脚本 / Stream Deck 用；第二实例转发后自行退出）
- **关闭窗口后一次性提示**（静音气泡，仅本次运行一次）
- 托盘菜单**顶部显示当前文件名**（灰色禁用项；菜单项位置随是否有文件而变）
- 副标题会显示**当前输出模式**（如 `正在播放 · 输出到麦克风`）
- 「输出到麦克风」（**真正生效**）：勾选后把**本播放器**的输出设备切到虚拟麦克风设备
  （用**按应用音频路由**，`src/PerAppAudio.{h,cpp}`），声音进入虚拟麦克风；取消勾选则清除设置、
  回到原播放设备（系统默认），你继续正常听歌。切换时保持播放位置。启动时自动清除，默认即"正常听歌"
  - 设备检测与推荐见 `src/AudioDevices.{h,cpp}`；弹窗为两段式（**普通用户步骤在前，技术详情在后**）
  - 实测：渲染到 `Sonar - Microphone` 的"渲染侧"才会进麦克风（渲染到 Gaming/Media/Aux/Chat 都不会）
  - **底层是未公开接口** `IAudioPolicyConfigFactory`（WinRT `Windows.Media.Internal.AudioPolicyConfig`），
    Sonar 等软件用的也是它；接口不可用时提示并降级，不影响播放

## 5. 生命周期与状态

- **播放自然结束 / 播放出错 / 用户点「销毁」（UI 或托盘）/ 打开失败 → 全部走同一条清理路径**
  `App::DestroyMedia()`（= `AudioPlayer::CloseMedia()` + `App::ResetToIdle()`）
- 空闲态（「未打开文件」）：文件名清空、标题回 `SilentPlayer`、状态「未打开文件」、
  `00:00 / 00:00`、进度 0、进度/播放/停止/销毁按钮禁用；
  **音量保持**、窗口尺寸不变、进程与托盘继续驻留
- **停止 ≠ 销毁**：停止保留文件名与总时长（状态「已停止」）；销毁释放媒体并清空界面
- 打开失败会弹出错误码对话框（例如损坏文件 `0xC00D36C4`），
  并**不影响之后打开其它文件**
- 退出（托盘「退出」）：销毁会话 → `MFShutdown` → `OleUninitialize`

## 6. 资源与部署

| 项 | 值 |
| ---- | ---- |
| 语言 / 标准 | C++17 |
| 子系统 | WIN32（无控制台窗口） |
| 目标系统 | `_WIN32_WINNT=0x0A00`（Windows 10+） |
| CRT | 静态链接（`/MT`）→ **无需 VC++ Redistributable** |
| 第三方库 | **无**（不用 FFmpeg / Qt / .NET / Electron） |
| 依赖 DLL | 8 个系统 DLL（`MF` `MFPlat` `ole32` `USER32` `GDI32` `SHELL32` `COMCTL32` `KERNEL32`）+ 2 个系统 API set（`api-ms-win-core-winrt-l1-1-0`、`api-ms-win-core-winrt-string-l1-1-0`，按应用音频路由需要）；**无第三方 DLL** |
| 产物 | 单个 `SilentPlayer.exe`（Release 308,224 字节） |
| 架构 | x64 PE（`8664`）：x64 原生运行；Windows 11 on ARM64 由系统 x64 模拟层运行 |
| 版本资源 | 1.0.0.0（`FileDescription` / `ProductName` / `OriginalFilename`）；图标 `app.ico` |

## 7. 明确不做 / 不支持

| 项 | 说明 |
| ---- | ---- |
| 播放列表 / 历史 / 播放状态 | 源码里没有持久化，音量默认 80% 仅内存 |
| 封面 / 歌词 / 均衡器 / 可视化 | 无 |
| 系统媒体键（键盘播放/暂停键） | **不做**：`RegisterHotKey` 是全局独占，会抢走其它播放器的媒体键；只提供窗口内快捷键 |
| 直接"写进麦克风" | **用户态不可行**（采集端点只读）。改用**按应用音频路由**把本播放器的输出切到虚拟麦克风设备——实测有效 |
| 拖到托盘图标 | **Windows 层面不可行**：托盘图标由 explorer 绘制，实测 `TrayNotifyWnd` 上没有 OLE 投放目标，投放到托盘图标的文件任何应用都收不到 |
| 文件关联注册 | 不主动修改系统默认程序；需用户自行设置「打开方式」 |
| 多文件 / 文件夹拖入 | 只播第一个（单文件播放器） |
| Windows 10 on ARM64 | 不支持（该系统只提供 x86 模拟，不提供 x64 模拟） |
| ARM64 实机 | **尚未验证**（开发环境无 ARM64 设备，本机 MSVC 也只有 x64/x86 工具链） |
| 网络流 / 播放列表文件（m3u） | 只按本地文件路径处理 |

## 8. 已知残留问题

- 句柄缓慢增长约 **+2/次**加载（修复前 +18/次）；内存已完全封顶。
  测试条件、已排除项与后续方向见 [TODO.md](TODO.md)。
- `SingleInstance::ForwardFileToExisting` 在首实例窗口未创建时最多重试 2s，
  仍失败则第二实例静默退出、该文件被丢弃（无兜底提示）。

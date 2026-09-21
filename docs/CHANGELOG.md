# Changelog

## 2026-09-20（「输出到麦克风」改为真正生效：按应用音频路由）

### Changed

- 托盘菜单项由「麦克风输出提示」改为 **「输出到麦克风」**（可勾选），语义从"只给提示"变成**真正生效**：
  - **勾选** → 把**本播放器**的输出设备切到虚拟麦克风设备，声音进入虚拟麦克风（别人能听到）；
  - **取消勾选** → 清除本应用的设置，回到**原播放设备**（系统默认），你继续正常听歌。
  - 切换时保持播放位置（记住位置与播放意图，重载后续上），实测连续切换 5 轮全程 PLAYING、进度连续。
- 启动时清除本应用的按应用路由设置，保证**默认就是"正常听歌"状态**
  （否则被 Sonar 之类的软件路由到麦克风后，一启动就听不到声音）。

### 技术方案：按应用音频路由（参考 SonicRoute / EarTrumpet）

用户提供了参考实现（SonicRoute wiki「技术实现」），据此采用**未公开**的
`IAudioPolicyConfigFactory`（WinRT 激活名 `Windows.Media.Internal.AudioPolicyConfig`）：

- IID `ab3d4648-e242-459f-b02f-541c70306324`（21H2+ 变体，vtable 与 EarTrumpet 一致）；
- `SetPersistedDefaultAudioEndpoint` 在 **0-based 槽位 25**（前面 19 个占位方法必须保留，
  否则 vtable 对不齐会调到错误的方法上）；
- 设备 ID 必须包成**设备接口路径**：`\\?\SWD#MMDEVAPI#{短ID}#{e6327cad-dcec-4949-ae8a-991e976a79d2}`，
  并以 **HSTRING** 传入；传 null 表示清除（回系统默认）；
- 需要**同时设置 eConsole 与 eMultimedia 两个角色**。
- 新增 `src/PerAppAudio.{h,cpp}`；CMake 链接 `runtimeobject`。

### Found（实测结论，决定了方案）

| 渲染到 | 是否出现在 `Sonar - Microphone` |
| ---- | ---- |
| `Sonar - Gaming` / `Media` / `Aux` / `Chat` | ✗ 全部 0.0000 |
| **`Sonar - Microphone`** | **✓ peak 0.3500（测试音 0.35 无损直通）** |

即：虚拟驱动把同一设备同时暴露成"渲染侧"和"麦克风侧"，**渲染到它的渲染侧才会进麦克风**。
另外实测发现 Sonar 早已用同一套按应用路由把本播放器强制路由到了麦克风
（播放器实际渲染端点是 `Sonar - Microphone`，而系统默认是 `Sonar - Gaming`）——
这正是用户"听不到自己放的歌"的原因。

### Fixed（本轮暴露并修掉的两个真实缺陷）

1. **旧会话的迟到事件会清掉正在播放的媒体**：换歌 / 换输出设备都会重建会话，旧会话迟到的
   `Ended` 被按新代号投递，未被过滤 → UI 被清成"未打开文件"。
   修法：`BeginGetEvent` 时把**会话自身作为状态对象**传入，回调里比对不是当前会话就直接丢弃。
2. **重载后误判为暂停**：`ApplyOutputDevice` 原先用事件驱动的 `State()` 判断是否在播，
   而刚重载时 `MESessionStarted` 还没到、`State()` 短暂是 `Stopped` → 被误判成"本来就没播"而暂停。
   修法：新增**播放意图** `AudioPlayer::WantPlaying()`（`Play/OpenFile` 置真，`Pause/Stop` 置假），
   用它做判断。实测连续切换 5 轮不再出现误暂停。

### Verified

- 开关实测（从 `Sonar - Microphone` 采集）：默认 **0.0000** → 勾选 **0.8139** → 取消 **0.0000** ✓
- 连续切换 5 轮：全程 `PLAYING`、进度 00:11→00:50 连续 ✓
- 启动默认状态：`PLAYING` 且麦克风 0.0000（正常听歌）✓
- 回归：9 种格式 + 错误扩展名 + 损坏文件全部正常、托盘菜单 8 项 ✓
- Debug / Release **干净重建**，**零错误、零警告**；EXE 308,224 字节；PE `8664 (x64)` ✓
- 依赖新增 2 个**系统 API set**：`api-ms-win-core-winrt-l1-1-0.dll`、
  `api-ms-win-core-winrt-string-l1-1-0.dll`（WinRT 激活所需，系统自带、无需分发）。
  其余仍为原 8 个系统 DLL，**无第三方 DLL**。

> 说明：该接口是**未公开**的。若将来系统移除或变更它，功能会失效（失败时给出提示并降级，
> 不影响播放本身）。

## 2026-09-20（麦克风提示：指引改版 + 实测链路已通）

### Changed

- 「麦克风输出提示」弹窗改为**两段式**：**上面是普通用户能看懂的简短步骤**（只说点哪里），
  下面用分隔线隔开才是技术检测详情。第 2 步标注为「只有对方听不到时才需要」，
  并提示「不少虚拟声卡默认就会把系统声音送进虚拟麦克风，先只做第 1 步试试」。
- 第 1 步推荐的麦克风改为**与虚拟输出设备同属一个驱动**的那个（按名字括号内的驱动名做家族匹配），
  而不是随便取第一个。实测本机上由 `Steam Streaming Microphone` 改为正确的
  `SteelSeries Sonar - Microphone`。

### Found（实测，重要）

- 新增探针 `test/audio_capture_probe.cpp`：从指定采集端点（麦克风/虚拟麦克风）录音并测 peak/rms。
- 用它做**对照实验**，验证「播放器的声音是否真的进了虚拟麦克风」：

  | 场景 | peak | rms |
  | ---- | ---- | ---- |
  | 播放器未播放 | 0.0000 | 0.0000 |
  | 播放器正在播放 | **0.7064** | **0.1530** |
  | 停止播放后 | 0.0000 | 0.0000 |

  未播放时是**精确的数字静音 0.0000**（不是环境噪声或真麦串音），播放时立即出现信号
  —— 说明**本机上播放器的声音已经出现在 `SteelSeries Sonar - Microphone` 里**，
  用户只需要在语音软件里把麦克风选成它，对方就能听到，**不需要任何设备切换**。
- 因此本轮**没有实现**「勾选时改播放设备、取消时改回」：播放器从未改过用户的播放设备，
  没有"原设备"需要恢复；且真正决定"声音是否进麦克风"的是虚拟声卡软件自己的路由，
  不是播放设备的选择。已把实测数据与结论交给用户决定是否仍需要该行为。

### Verified

- 弹窗实测：普通用户段在前且简短（第 1 步给出具体设备名）、技术详情在后 ✓
- 推荐设备 = `SteelSeries Sonar - Microphone`（与实测有信号的那个一致）✓
- 回归：9 种格式 + 错误扩展名 + 损坏文件全部正常（BADFILE 仅出现在损坏文件上）、
  托盘菜单 8 项、托盘销毁正常 ✓
- Debug / Release **干净重建**，**零错误、零警告**；EXE 302,592 字节 ✓

> 测试踩坑记录：批量脚本里 `taskkill` 后等待过短时，新实例会把文件转发给**正在退出的旧实例**，
> 导致探针读到 BADFILE/NOFILE 的假失败。等待需 ≥1.5s。

## 2026-09-20（托盘新增「麦克风输出提示」）

### Added

- 托盘右键菜单新增可勾选项 **「麦克风输出提示」**（`kCmdMicHint = 6`，位于「销毁」之后）。
  勾选后立即检测系统音频设备并弹出操作指引；再次点击取消勾选。状态仅本次运行有效，不写盘。
- 新增 `src/AudioDevices.{h,cpp}`：**只读**枚举渲染/采集端点（MMDevice API），
  按设备名启发式标记「虚拟设备 / 回环监听 / 麦克风侧」，并组装中文指引文本。
  菜单顺序（实测）：`播放/暂停(1) · 停止(2) · 销毁(5) · 麦克风输出提示(6) · ──── · 显示播放器(3) · ──── · 退出(4)`
- 探针增强：`player_state_probe` 增加 `--michint`（自动触发该菜单项、读出弹窗全文并关闭），
  `--traydump` 现在会显示菜单项的勾选状态（`[CHECKED]`）。

### 为什么只是"检测 + 指引"而不是真的把声音送进麦克风

**普通用户态程序无法把音频写进麦克风**，这不是实现难度问题，是 Windows 的机制限制：

| 证据 | 内容 |
| ---- | ---- |
| WASAPI 接口 | 采集端点只有 `IAudioCaptureClient`（只读），没有 `IAudioRenderClient`；对采集端点写数据会被拒 |
| 官方文档 | 虚拟音频设备是 SysAudio 依据**内核态**音频组件构建的 filter graph（注册在 `KSCATEGORY_AUDIO_DEVICE`），用户态创建不了 |
| 现实印证 | 所有"虚拟麦克风"都是内核驱动（VB-Cable / Virtual-Audio-Driver / TVirtAudio / MicyWMD…），本机的 SteelSeries Sonar / Steam Streaming / DroidCam / Virtual Desktop 也都装了驱动 |
| 本项目约束 | 禁止第三方库，也不可能分发内核驱动（需签名 + 管理员安装） |

所以本轮按用户选定的「方案 B」实现：**只做检测与说明，不改变任何音频路由**，
并明确告知用户该在语音软件里把麦克风选成哪个设备、以及路由由谁负责。

### Verified

- 托盘菜单实测 8 项，新增项 `id=6`，勾选后 `--traydump` 显示 `[CHECKED]` ✓
- 弹窗内容实测（`--michint` 读取全文并落盘核对）：
  - 正确检出本机 **4 个虚拟麦克风**：`Steam Streaming Microphone`、`SteelSeries Sonar - Microphone`、
    `DroidCam Virtual Audio`、`Virtual Desktop Audio`
  - 输出设备按侧标注：`[输出侧]`（Sonar - Gaming/Media/Aux/Chat、Steam Streaming Speakers）
    与 `[麦克风侧，一般不作输出]`（Sonar - Microphone、Steam Streaming Microphone）
  - 正确读出当前默认播放设备
- 回归：9 种格式/错误扩展名/损坏文件、A→B 切歌（进程数 1）、拖动进度、音量（80%→30%）、
  暂停/继续、关闭窗口后台播放、托盘销毁、托盘停止、托盘退出（退出码 **0**）全部通过。
- Debug / Release **干净重建**，**零错误、零警告**；EXE 300,032 字节；
  依赖仍为 8 个系统 DLL，**无新增 DLL**。

## 2026-09-20（文档：新增能力清单）

### Added

- `docs/CAPABILITIES.md`：**当前版本实际具备的能力清单**，逐条标注源码出处，
  并单列「明确不做 / 不支持」与「已知残留问题」。内容依据 `src/` 通读核对
  （6 个 `.cpp` + 5 个 `.h`，2167 行），不是照抄旧文档。
  README「构建与开发」里已加入索引。

## 2026-09-20（拖文件到窗口直接播放）

### Added

- **把音频文件拖到播放器窗口上即可直接播放**。用 OLE `IDropTarget` 实现
  （`PlayerWindow::DropTarget`），只接受 `CF_HDROP`，取第一个文件后走与命令行 /
  单实例 IPC 完全相同的入口 `App::LoadFile()`，因此不产生任何新的播放/清理分支。
  - 拖入时窗口显示"复制"光标（`DROPEFFECT_COPY`）；非文件内容显示"禁止"（`DROPEFFECT_NONE`）。
  - 支持多文件拖入但**只播第一个**（SilentPlayer 是单文件播放器）。
  - `App::Run` 里 `CoInitializeEx` 改为 `OleInitialize`（拖放需要 OLE，MF 用同一套单线程套间）。

### 为什么用 IDropTarget 而不是 WM_DROPFILES（实测结论）

| 方案 | 结果 |
| ---- | ---- |
| `DragAcceptFiles` + `WM_DROPFILES` | 只有 explorer 会发这条消息；实测第三方进程 `PostMessage(WM_DROPFILES)` 直接返回 **0 / ERROR_INVALID_HANDLE**（User32 会校验 HDROP 句柄），连构造测试消息都发不进去，无法验证 |
| OLE `IDropTarget` + `RegisterDragDrop` | 现代 shell 拖放走的就是这条通道；实测注册后能收到 `DragEnter` / `DragOver` / `Drop`（带 `CF_HDROP`），且**可被自动化验证** |

### 关于「拖到托盘图标」——Windows 不支持

- 官方文档（Notifications and the Notification Area）里，通知区域图标与应用的通道**只有鼠标/键盘
  回调消息**，没有任何拖放相关接口。
- 实测取证：托盘窗口（`TrayNotifyWnd`）**没有注册任何 OLE 投放目标**
  （`RegisterDragDrop` 返回 `S_OK`，说明该窗口上原本没有任何投放目标）。
  第三方托盘图标只是 explorer 画的一个图标，光标下的窗口属于 explorer，
  因此投放到托盘图标上的文件**任何应用都收不到**。
- 结论：不做该功能（用户已确认不需要）。若将来要做，只能用一个覆盖在托盘图标上的透明窗口
  抢走光标，会破坏托盘图标本身的点击行为，得不偿失。

### Verified

- 应用的投放目标确实已注册：从外部对应用窗口调用 `RegisterDragDrop` 返回
  **`DRAGDROP_E_ALREADYREGISTERED (0x80040101)`**。
- 一个已注册的 IDropTarget 能收到真实 OLE 拖放的 `DragEnter` / `DragOver` / `Drop`（带 CF_HDROP）——已实测。
- 换用 `OleInitialize` 后回归全部通过：7 种格式 + 错误扩展名 + 损坏文件、
  A→B 切歌、拖动进度、暂停/继续、音量、关闭窗口后台播放、单实例、销毁（UI 与托盘）。
- 退出路径（新增的 `RevokeDragDrop` + `OleUninitialize`）实测退出码 **0**，干净退出。
- 稳定性：启动→播放→销毁→再销毁 共 6 轮，异常 0 次；托盘销毁 8 轮进程均存活。
- Debug / Release **干净重建**，**零错误、零警告**；EXE 276,480 字节；
  PE `8664 machine (x64)`；依赖仍为 8 个系统 DLL，**无新增 DLL**。

### 未完成验证（如实说明）

- **"真实鼠标拖放后确实开始播放"这一步没能自动化验证**：合成鼠标驱动的 OLE 拖放
  （`test/drag_probe.cpp`）能让目标收到 `DragEnter`/`DragOver`/`Drop`，但无法稳定地把投放
  落到应用窗口上（OLE 在该合成场景下常以 `DragLeave` 收尾，光标位置也受真实鼠标影响）。
  已验证的是：投放目标已注册 + IDropTarget 通道可收到带 CF_HDROP 的拖放 + 处理函数转发到
  已端到端验证过的 `App::LoadFile()`。**真实拖放体验需要用户用鼠标实测。**

## 2026-09-20（新增「销毁」· 单 EXE 双架构确认）

### Added

- **「销毁」功能**：停止并彻底释放当前媒体，回到「未打开文件」的空闲状态。
  - UI 按钮行改为 **播放/暂停 · 停止 · 销毁**（三个按钮居中，间距不变，窗口尺寸不变，未重新设计 UI）。
  - 托盘菜单新增「销毁」，位置在「停止」之后，并补上「退出」前的分隔线。
  - **UI 按钮与托盘菜单共用同一个入口 `App::DestroyMedia()`**，没有第二套清理逻辑。
- 新增 `App::ResetToIdle()`：把「清文件引用 + 清 UI + 禁用控件」集中成一处，
  供**销毁 / 播放自然结束 / 播放出错 / 打开失败**四条路径共用，避免清理逻辑各自漂移。
- 探针增强：`player_state_probe` 增加 `--destroy=i`（点 UI 销毁按钮）、
  `--trayitem=i:n`（键盘导航并激活托盘菜单第 n 个可选项）、`--traydump`（用 `MN_GETHMENU` 枚举托盘菜单内容）。

### 单 EXE 双架构（研究结论 + 验证）

- **结论：一个 `SilentPlayer.exe`（x64 PE）同时满足 x64 Windows 与 Windows 11 on ARM64，且无需改动构建系统。**
  - Windows x64：原生运行。
  - Windows 11 on ARM64：由系统内置的 x64 模拟层运行。Microsoft 文档明确
    「Windows 11 on Arm supports emulation of both x86 and x64 apps」，且系统二进制以
    **Arm64X PE** 形式提供，「can be loaded into both x64 and Arm64 processes」——
    所以 x64 进程可以完整访问整个 OS（含 Media Foundation）。
  - 注意：**Windows 10 on Arm 只支持 x86 模拟、不支持 x64**，因此 x64 产物需要 Windows 11 on Arm64。
- **ARM64EC 不能满足本需求**：ARM64EC 的最终 PE 虽然是 `8664 (x64) (ARM64X)`，
  但其代码是 ARM64 指令，**只能在 ARM64 上执行**，无法在 x64 Windows 上运行；
  ARM64X 同样是「含 ARM64 + ARM64EC 代码、可被 x64 与 Arm64 进程加载」的 ARM64 家族 PE。
  两者都会让「一个 EXE 同时兼容 x64 与 ARM64」失效，因此**没有采用**，
  也没有把目标架构改名后宣称支持。
- 已核实本项目不存在架构限制：无内联汇编、无编译器 intrinsics、无架构条件编译
  （`_M_X64`/`_WIN64` 等一个都没有）、无架构/模拟检测 API、无 `/arch:` 编译选项、
  无第三方头文件与 DLL —— 只依赖 Windows 系统 API。
- CMake 增加架构策略提示：配置时打印目标架构，若目标被改成 ARM64EC/ARM64 则给出
  明确 WARNING（提示该产物无法在 x64 Windows 上运行）。
- **ARM64 实机运行尚未验证**（本环境无 ARM64 设备；本机 MSVC 只安装了 x64/x86 工具链，
  没有 arm64/arm64ec 库，无法产出 ARM64 产物做交叉验证）。

### Verified

- Debug / Release **干净重建**，**零错误、零警告**；EXE 273,408 字节。
- PE：`8664 machine (x64)`、`PE32+`、`Windows GUI`；依赖仍为 8 个系统 DLL
  （MF / MFPlat / ole32 / USER32 / GDI32 / SHELL32 / COMCTL32 / KERNEL32），**无新增 DLL**。
- 销毁（UI 按钮）：播放中点击 → 文件名清空、状态「未打开文件」、时间 `00:00/00:00`、
  进度 0、峰值归 0、**进程存活（1 个）**。
- 销毁（托盘菜单）：与 UI 按钮结果完全一致（`--trayitem` 自动化验证）。
- **停止 ≠ 销毁**：停止后仍显示 `t.flac` 与总时长 `02:02`、状态「已停止」；
  销毁后全部清空为「未打开文件」/`00:00`。两者语义未混。
- A 播放 → 销毁 → 打开 B：B 从 0 开始、时长正确、**峰值 0.10~0.15 真实出声**、
  A 无残留声音、A 的事件未影响 B、进程数始终为 1。
- 播放 → 销毁 ×10：工作集 31.3 → 32.8 MB（静置回落到 32.7，**不增长**）；
  句柄 +2/轮（与已知残留一致，销毁未引入新的泄漏类别）。
- 回归：7 种格式 + 错误扩展名（MP4 名 `.mp3` / MP3 名 `.mp4`）+ 损坏文件、
  A→B 切歌、拖动进度、暂停/继续、音量（界面与会话音量同步 80%→40%）、
  关闭窗口后台继续播放、单实例 —— 全部通过。

## 2026-09-20（按内容识别媒体类型 · 探针段错误修复）

### Added

- **按文件内容识别媒体类型，不信任扩展名**。`ResolveMediaSource` 改为两段式：
  1. 读文件头嗅探真实容器（MP4/M4A 的 `ftyp`、RIFF/WAVE、`fLaC`、ASF GUID、ADTS、
     `ID3`/MPEG 同步字、AC3 等），`MFCreateFile` 打开成字节流，把嗅探出的 MIME 通过
     `MF_BYTESTREAM_CONTENT_TYPE` 交给 MF，并以 `CreateObjectFromByteStream(url=nullptr)`
     **绕开扩展名**选择处理器；
  2. 嗅探不出时回退到原来的 `CreateObjectFromURL`（按扩展名），保证既有行为不变。
  - 效果：`music.mp4` 改名成 `music.mp3`（或反向）都能正常播放其中的音频；
    不转码、不修改原文件、不产生临时文件、不引入 FFmpeg、无新增 DLL。
- 新探针 `test/resolve_probe.cpp`：对比 `CreateObjectFromURL`（扩展名）/
  字节流+MIME / URL+属性存储三种机制，并打印解析出的流信息。
- `fetch_samples.py` 增加派生样本生成：`mp4content.mp3`、`mp3content.mp4`、`normal.mp3`、
  `normal.mp4`、`broken.mp3`（随机字节）、`truncated.mp3`（截断的 MP4 头）。

### Fixed

- **`mf_load_cycle_probe --mode=restart` 段错误**：根因是 `MFCreateMediaSession` 只在
  `mode == "session"` 时执行，restart 模式下 `pSession` 为空，`pSession->SetTopology`
  空指针解引用。改为按模式判断是否需要会话（`session` / `restart` / `sessiononly`）。
  顺带把回调里的会话指针改为原子并在拆除前清空（避免在途回调访问已释放会话），
  并给探针加了无缓冲输出（崩溃时也能看到已打印内容）。**四个模式现在全部 exit=0**。

### Investigated（句柄残留，未改架构）

针对性排查了句柄缓慢增长，结论（全部实测）：

| 场景 | 句柄增长 |
| ---- | ---- |
| 只 resolve + 释放 | 0 |
| resolve + 建拓扑 + 释放（不碰会话） | 0 |
| 会话创建/销毁（不加载媒体）×20 | **0**（249 → 249） |
| 同一拓扑反复 Start/Stop | ≈0 |
| 每次 SetTopology 新拓扑（不重建会话） | 约 +13/次 |
| 纯 IPC（不带文件，窗口已显示）×20 | **0**（442 → 430） |
| 应用实际：媒体加载 ×20 / ×40（含会话重建） | 约 **+2.2 / +1.9 次** |

即：主要增长来自「会话随每个新拓扑累积」，已由会话重建消除绝大部分（13 → 2）；
残留约 2 个句柄/次来自媒体加载路径内部（已排除 IPC、会话重建、resolve、拓扑构建），
无法在不改架构的前提下进一步定位。**内存已完全封顶**，句柄增长不影响正常使用
（按 +2/次计，连续打开 1000 个文件才约 +2000 句柄）。已在 TODO 记录。

### Verified

- Debug / Release **干净重建**，**零错误、零警告**；EXE 273,408 字节（+1.5 KB）；
  `dumpbin /dependents` 仍只有系统 DLL，**未新增任何 DLL**。
- 错误扩展名矩阵（真实样本，判定标准为状态 PLAYING 且峰值 > 0）：

| 实际内容 | 文件名 | 结果 |
| ---- | ---- | ---- |
| MP3 | `.mp3` | ✓ |
| MP4 | `.mp4` | ✓ |
| MP4 | `.mp3` | ✓ |
| MP3 | `.mp4` | ✓ |
| 随机字节（损坏） | `.mp3` | ✓ 不崩溃、回到空闲、错误码 `0xC00D36C4` |
| 截断的 MP4 头 | `.mp3` | ✓ 同上 |

- 原文件完整性：打开前后 `md5sum -c` 全部 OK，**文件未被修改**。
- 损坏文件之后再打开正常文件：正常播放 ✓。
- 回归：单实例（进程数恒为 1）、A→B 混合切歌 12 次（含错误扩展名文件）、
  播放结束清理、暂停/继续、拖动进度、关闭窗口后台继续播放、音量 —— 全部通过。

## 2026-09-20（格式扩展 · 播放结束清理 · 内存泄漏修复）

### Added

- **更多播放格式**：MP4（含视频轨）、M4A、AAC、FLAC、WMA 现在都能直接播放，
  全部走 Windows Media Foundation 原生链路，**未引入任何第三方库、未增加 DLL**。
  - 关键改动：`BuildTopology` 里对**非音频流显式 `DeselectStream`**。MSDN 的
    `IMFPresentationDescriptor::SelectStream` 只负责选中指定流，**不会自动取消其它流**，
    所以含视频轨的 MP4 必须显式取消视频流，否则拓扑解析会把视频也算进去。
  - MP4 只解码音频、不创建任何视频渲染节点、不显示视频窗口，无需转码。
- **播放结束 / 出错后的自动清理**：新增 `AudioPlayer::CloseMedia()` 与 `PlayerWindow::ClearMedia()`。
  播放自然结束后：停止 → 释放媒体对象 → 清空文件名 → 状态回到「未打开文件」→ 进度与时间归零 →
  按钮恢复默认（禁用）→ **音量保持不变** → 窗口尺寸不变 → 进程继续驻留托盘。
- **迟到事件防串扰**：事件消息带上「媒体代号」（`AudioPlayer::MediaGeneration()`），
  `App::OnAudioEvent` 丢弃代号不匹配的事件。这样 A 的结束事件即使在 B 开始播放后才被处理，
  也不会把 B 停掉或清空 B 的 UI。
- 新探针：`mf_codec_probe`（枚举本机 MF 编解码器）、`proc_mem_probe`（进程内存/句柄/GDI/USER 计数）、
  `mf_load_cycle_probe`（隔离复现 MF 加载/释放循环）、`fetch_samples.py`（下载各格式测试样本）。

### Fixed

- **内存与句柄随每次加载/切歌线性增长**（严重）：实测 8 次加载工作集 31.8→61.4 MB、
  20 次私有内存 2.4→98.2 MB（约 4.8 MB + 18 句柄/次），且不回落。
  - 定位过程（全部有数据支撑）：用 `mf_load_cycle_probe` 隔离复现后确认
    **只做 resolve = 不泄漏、只建拓扑并释放 = 不泄漏、走会话 `SetTopology + Start + Stop` = 泄漏**；
    排空事件队列无效、`MFSESSION_SETTOPOLOGY_CLEAR_CURRENT` 无效、等待 `MESessionStopped` /
    `MESessionTopologiesCleared` 也无效；泄漏对象在 `IMFMediaSession::Shutdown()` 时才释放，
    说明是**会话内部随每次呈现累积**。
  - 修复：`ReleaseMedia()` 里**重建媒体会话**（`DestroySession()` + `CreateSession()`），
    把占用封顶。`MFStartup` / `MFShutdown` 仍是进程级、只执行一次。
  - 效果：切歌 10 次工作集 31.3→32.9 MB、私有内存 10.9→10.3 MB（**不再增长**），
    静置后回落到约 29 MB；句柄增长由 +18/次降到约 +2.3/次。
- 修正 `ClearTopologies()` 的误用认知：MSDN 明确它**只清队列、不清当前拓扑**，
  所以补上 `SetTopology(MFSESSION_SETTOPOLOGY_CLEAR_CURRENT, nullptr)` 显式释放当前拓扑。

### Verified

- Debug / Release **干净重建**，**零错误、零警告**；EXE 271,872 字节（较之前 +2 KB）；
  `dumpbin /dependents` 仍只有系统 DLL，**未新增任何 DLL**。
- 格式矩阵（真实样本 + 环回峰值判定，不只看界面）：MP3 ✓、WAV ✓、MP4（含视频轨）✓、
  M4A ✓、FLAC ✓、AAC ✓、WMA ✓ —— 全部 `PLAYING` 且峰值 > 0。
- 播放结束：进度归零、文件名清空、状态「未打开文件」、峰值归 0、进程存活。
- 切歌：连续 10 次（MP3/FLAC/MP4 混合）进程数恒为 1，UI 只显示最后一个文件，无旧声音残留。
- 拖动进度条 / 暂停继续 / 关闭窗口后台继续播放：全部通过。

## 2026-09-20（音量修复 + 剩余功能项自动化验证）

### Fixed

- **音量滑块不生效（只改界面文字，实际输出不变）**：`ApplyVolume()` 用
  `MFGetService(m_session, MR_AUDIO_POLICY_SERVICE, IID_PPV_ARGS(&m_audioVolume))` 索取
  `IMFSimpleAudioVolume`，实测返回 **`E_NOINTERFACE`（0x80004002）**、指针为 null，因此每次调用都失败。
  按 SDK 的 `mfidl.h`：`IMFSimpleAudioVolume`(8880) 对应 **`MR_POLICY_VOLUME_SERVICE`**(8997)，
  而 `MR_AUDIO_POLICY_SERVICE`(9299) 对应的是 `IMFAudioPolicy`（显示名/图标/分组等策略）。
  修正为 `MR_POLICY_VOLUME_SERVICE`（一行改动，与播放逻辑无关）。

### Added

- `test/audio_loopback_probe.cpp`：WASAPI 环回采集指定进程所连端点的**真实渲染幅度**（peak / RMS）。
  用于验证音量等只能通过听感判断的功能 —— 会话峰值 `GetPeakValue` 和 `ISimpleAudioVolume` 都
  反映不出 Media Foundation 内部应用的音量，必须测实际输出。
- `test/player_state_probe.cpp` 增加操作模拟：`--seek=<i>:<pct>`（模拟拖动进度条）、
  `--volume=<i>:<v>`（模拟音量滑块）、`--close=<i>`（模拟关闭窗口），并输出 `vol`（界面百分比）
  与 `svol`（WASAPI 会话实际音量）两列。
- `test/audio_endpoints_probe.cpp` 增加会话音量输出（`sessionVol` / `chan0Vol`）。

### Verified

- Debug / Release 均编译通过，**零错误、零警告**。
- 音量端到端实测（环回采集，源幅度 0.36621）：滑块 100%→峰值 0.36621、50%→0.18311、
  20%→0.07324、5%→0.01831，与会话实际音量（1.0 / 0.5 / 0.2 / 0.05）线性吻合。
- 拖动进度条：拖到 50% 后当前时间由 00:03 跳到 00:15、进度条位置 116→500，之后继续正常递增。
- 关闭窗口（WM_CLOSE）：窗口隐藏（`vis=0`）、进程存活、**峰值持续 0.35 后台继续播放**。
- 暂停/继续：暂停时进度冻结、峰值归 0；继续后从冻结位置接续。
- 切歌回归：播放中打开新文件 → 进度归零、时长更新、声音无中断；进程数始终为 1。

## 2026-09-20（UI 布局 + 进程显示）

### Changed

- **播放器 UI 布局重排**（只改布局，未改任何播放逻辑）：
  - 客户端由 360×150 改为 **420×222**（逻辑尺寸），并按显示器 DPI 缩放，高 DPI 下不再挤在一起。
  - 按行分区，每类信息独占一行：文件名 → 状态副标题 → 进度条 → 当前时间 / 总时长（左 / 右对齐）→ 居中按钮 → 音量。
  - 进度条独占一行且宽度拉满内容区（逻辑 384px），左右各留 18px；时间不再挤进进度条。
  - 播放/暂停与停止按钮居中排列，间距 20px，尺寸 84×32（不是大按钮）。
  - 音量独占一行：标签 + 滑块 + 百分比，互不争抢空间。
  - 新增状态副标题控件（`SetStatus`）：正在播放 / 已暂停 / 已停止 / 已播放完 / 未打开文件 / 播放出错。
  - 次要信息（副标题、时间、音量百分比）改为灰色，与文件名拉开层次。
  - 文件名用 `SS_ENDELLIPSIS`，长文件名显示为 `a_very_long_music_file_name_tha...`，不撑破窗口。
  - 新增 3 个字体（10pt 文件名 / 9pt 正文 / 8pt 副标题），随 DPI 缩放。

### Added

- `resources/app.rc` 补上标准 **VERSIONINFO**：`FileDescription` / `ProductName` = `SilentPlayer`，
  `OriginalFilename` = `SilentPlayer.exe`，`InternalName` = `SilentPlayer`，`FileVersion` / `ProductVersion` = `1.0.0.0`。
- `test/player_state_probe.cpp` 新增 `--dump`：打印客户端尺寸与每个子控件的位置/尺寸并检测控件重叠，用于免人眼检查布局。
- `test/window_shot.cpp`：把播放器窗口截图成 24 位 BMP，用于目视核对布局。

### Fixed

- **任务管理器找不到 / 认不出 SilentPlayer**：`app.rc` 原本只有图标，**完全没有版本资源** ——
  实测 `VersionInfo` 的 `FileDescription`、`ProductName`、`OriginalFilename` 全为空，`FileVersionRaw = 0.0.0.0`，
  Windows 拿不到任何产品标识。补上 VERSIONINFO 后，进程的 `Description` / `Product` 变为 `SilentPlayer`。

### Verified

- Debug / Release 均编译通过，**零错误、零警告**。
- 最终 EXE 名称确认为 `SilentPlayer.exe`。
- PE 版本信息实测：FileDescription / ProductName = SilentPlayer，OriginalFilename = SilentPlayer.exe，版本 1.0.0.0。
- 运行中进程实测：`ProcessName = SilentPlayer`、`Description = SilentPlayer`、`Product = SilentPlayer`、`Path = ...\SilentPlayer.exe`。
- 布局实测（DPI 感知，150% 缩放屏幕）：客户端 630×333 物理像素，10 个控件，**overlaps = 0**。
- 长文件名实测：文件名正确省略，其余控件位置不变，无重叠。
- 单实例与音频切换回归通过：播放中打开新文件 → 进度归零、时长更新、声音无中断；暂停/继续正常；进程数始终为 1。

## 2026-09-20（换歌不切换播放 · 核心修复）

### Fixed

- **核心问题：打开新音频文件后只更新了 UI，声音仍是上一首。**
  - 原因一：`IMFMediaSession::Stop()` 是**异步**的（返回时只表示请求已受理），紧接着调用 `SetTopology(0, topo)` 时会话仍被视为运行中，按 MF 规范新拓扑被**排队**，要等当前呈现播完才生效。
  - 原因二：自动播放用的 `Start(&GUID_NULL, &varStart)` 中 `varStart.vt = VT_EMPTY` 表示“从当前位置开始”，会沿用上一次呈现的位置。
  - 修改：新增 `AudioPlayer::StopSessionSync()` —— 调用 `Stop()` 后等待 `MESessionStopped`（上限 2s）再继续，确保旧播放真正停止、旧拓扑与媒体源被释放；`SetTopology` 改为带 `MFSESSION_SETTOPOLOGY_IMMEDIATE`；`OpenFile` 的自动播放改为显式从 0 开始（`VT_I8` / 0）。
- **换歌后进度不归零（直接显示 00:05 / 00:08）、播放结束后进度超过总时长（00:11 / 00:08）。**
  - 原因：会话时钟是**会话级时间线**，新呈现开始时不会归零，且停止后仍继续走。
  - 修改：新增位置基准 `m_clockBase`，在 `MESessionStarted` 时记录本次呈现起点，`Position()` 返回 `会话时钟 − 基准`；基准未就绪时返回 -1（UI 不显示进度）；结果钳制在总时长内；`Stopped` / `Ended` / `Error` 后基准失效。

### Changed

- 切换文件期间（`m_loading`）不再向 UI 上报旧会话的 `Stopped` / `Paused` / `Ended` 事件，避免冲掉新文件的 UI 状态，也避免旧文件的 `Ended` 误触发 `Stop()` 打断新播放。
- `Paused` 状态不影响位置基准，因此暂停/继续时进度会接着走。
- 修正 `docs/DEVELOPMENT.md` 中的依赖清单：按实际 `dumpbin /dependents` 结果，实际依赖为 `MF.dll / MFPlat.dll / ole32.dll / USER32.dll / GDI32.dll / SHELL32.dll / COMCTL32.dll / KERNEL32.dll`（无 VC 运行时）。

### Added

- `test/player_state_probe.cpp`：自动化验证探针。读取运行中实例的文件名、播放状态、时间标签、进度条位置，并读取该进程在**所有活动渲染端点**上的真实音频峰值；支持 `--toggle=i` 模拟播放/暂停。用于确认“换歌后播放真的重启、进度归零、暂停/继续正常”，而不依赖人耳判断。

### Verified

- Debug / Release 均编译通过，**零错误、零警告**；Release 单 EXE 约 260 KB，依赖仅系统 DLL。
- 启动后主窗口隐藏（`visible=0`），无错误对话框。
- 运行时实测（Release + 探针）：
  - A 播放中打开 B → 文件名变为 B、时长变为 B、**进度立即归零**、峰值全程 0.35 **声音无中断**；
  - 暂停 → 进度冻结、峰值归 0；继续 → 从冻结位置继续、峰值恢复；
  - 播放自然结束 → 正确停止、进度归零、不再超过总时长；
  - 连续快速切换 3 次（间隔 200ms）无死锁、无状态错乱，进程数始终为 1。

## 2026-09-19

### Added

- 从零创建 SilentPlayer 项目：CMake + C++17 + Win32 + Media Foundation。
- 单实例机制（Named Mutex `Local\SilentPlayer_SingleInstance_Mutex` + WM_COPYDATA，magic `0x53504C52`）。
- 系统托盘（Shell_NotifyIcon）：左键显示播放器窗口，右键菜单（播放/暂停、停止、显示播放器、退出）。
- 极简播放器窗口（约 360×150）：文件名、进度条（可拖动）、当前时间、总时长、播放/暂停、停止、音量条。
- 音频自动播放（命令行参数或打开方式），默认静默无 UI。
- 新文件切换：第二实例把文件路径转发给首实例，切换并自动播放。
- 播放结束停止并驻留托盘，不自动播放下一首。
- 程序图标（resources/app.ico，多尺寸 PNG 压缩）。
- 测试样本与诊断探针（test/）。

### Changed

- 无（首个版本）。

### Fixed

- 修复 `MF_E_TOPO_MISSING_PRESENTATION_DESCRIPTOR`（0xC00D5217）：拓扑构建时显式 `pPD->SelectStream(音频流)`，并为源节点设置 `MF_TOPONODE_PRESENTATION_DESCRIPTOR` / `MF_TOPONODE_STREAM_DESCRIPTOR` / `MF_TOPONODE_STREAMID`。
- 修复 `IMFMediaSession::Start` 返回 E_POINTER（0x80004003）：`Start(nullptr, nullptr)` 改为 `Start(&GUID_NULL, &varStart)`（`varStart.vt = VT_EMPTY`），`Play()` 同步修正。
- 修复 `IMFClock::GetTime` 编译错误：新 SDK 已移除该方法，改用 `GetCorrelatedTime(0, &clockTime, nullptr)`。
- 修复 `MF_MEDIASESSION_SERVICE` 未声明：新 SDK 已移除该导出，改用 `MFGetService(m_session, MR_AUDIO_POLICY_SERVICE)` 获取 `IMFSimpleAudioVolume`。
- 修复 `TrayIcon.cpp` 缺 `#include <shellapi.h>`（WIN32_LEAN_AND_MEAN 排除 Shell API）。

### Technical

- 静态链接 CRT（/MT），EXE 仅依赖系统 DLL，无 VC 运行时依赖。
- `/utf-8` 编译选项；`_WIN32_WINNT=0x0A00`；DPI 感知（PER_MONITOR_AWARE_V2）。
- 进度刷新用 500ms 定时器，仅窗口可见时刷新；拖动进度条期间暂停自动刷新（`m_draggingProgress`）。
- 音量默认 80%，仅内存不持久化。
- 事件通过 `IMFAsyncCallback` + `PostMessage(WM_APP_AUDIO_EVENT)` 跨线程上报，UI 线程安全。

### Notes

- 未实现文件关联（按规范不默认修改系统默认程序，需用户主动设置，见 TODO）。
- 未引入任何第三方库；未实现播放列表、歌词、EQ、网络音乐等（规范明确不做）。
- 播放结束后音频会话仍短暂存在于默认端点（SAR 行为），峰值归 0，无实际输出。

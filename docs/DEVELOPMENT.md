# SilentPlayer 开发文档

目标：另一个 AI 或开发者拿到项目后，可以直接开始开发。

## 开发环境

| 项 | 值 |
| ---- | ---- |
| 操作系统 | Windows 10/11 x64 |
| 编译器 | MSVC 14.38（Visual Studio 2022 Community 17.8） |
| Windows SDK | 10.0.22621 |
| CMake | 4.4.3（`C:\Program Files\CMake\bin\cmake.exe`，winget 安装） |
| 生成器 | NMake Makefiles（配合 vcvars64） |

> 注：本机 VS 未安装 CMake 组件，且 VS 实例注册不完整，「Visual Studio 17 2022」生成器不可用，因此使用 **vcvars64.bat + NMake Makefiles** 方案。其他机器若有完整 VS 安装，直接用 `-G "Visual Studio 17 2022"` 亦可。

## 编译器环境准备

每次构建前先加载 MSVC x64 环境：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

（路径以实际 VS 安装为准。）

## CMake 配置与构建

### 首次配置

```bat
cmake -S E:\kunkun\slientPlayer -B E:\kunkun\slientPlayer\build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
```

### Release 构建

```bat
cmake --build E:\kunkun\slientPlayer\build --config Release
```

产物：`build\SilentPlayer.exe`

### Debug 构建

```bat
cmake -S E:\kunkun\slientPlayer -B E:\kunkun\slientPlayer\build-debug -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build E:\kunkun\slientPlayer\build-debug --config Debug
```

### 干净重建

```bat
rmdir /s /q E:\kunkun\slientPlayer\build
（然后重新执行「首次配置」+「Release 构建」）
```

### 在无法调用 cmd.exe 的环境中构建

如果所在环境禁止 `cmd.exe`（例如受限的自动化沙箱），不必用 `vcvars64.bat`，
直接设置环境变量后调用 cmake 即可（`cl.exe` 会从 PATH 找到）：

```bash
MSVC="C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.38.33130"
SDK="C:\\Program Files (x86)\\Windows Kits\\10"; V="10.0.22621.0"
export INCLUDE="$MSVC\\include;$SDK\\Include\\$V\\ucrt;$SDK\\Include\\$V\\shared;$SDK\\Include\\$V\\um;$SDK\\Include\\$V\\winrt"
export LIB="$MSVC\\lib\\x64;$SDK\\Lib\\$V\\ucrt\\x64;$SDK\\Lib\\$V\\um\\x64"
export PATH="$MSVC\\bin\\Hostx64\\x64:$SDK\\bin\\$V\\x64:$PATH"
"/c/Program Files/CMake/bin/cmake.exe" --build E:/kunkun/slientPlayer/build
```

注意 `winrt` 目录不能漏（`wrl/client.h` 在这里），否则报 C1083。
另外链接前必须确保没有 `SilentPlayer.exe` 在运行，否则 LNK1104。

## 构建配置要点（CMakeLists.txt）

- C++17；`WIN32` 可执行（GUI 子系统，无控制台）。
- **静态 CRT**：`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`（/MT），Debug 为 /MTd —— 这是「单 EXE 无运行时依赖」的关键。
- 编译定义：`UNICODE / _UNICODE / WIN32_LEAN_AND_MEAN / NOMINMAX / _WIN32_WINNT=0x0A00`。
- 编译选项：`/utf-8`（源码含中文 UI 文本）、`/W3`。
- 链接库：`mf mfplat mfuuid ole32 oleaut32 user32 gdi32 shell32 comctl32 advapi32`。
- 无第三方库。

## 发布方式

1. Release 构建。
2. 复制 `build\SilentPlayer.exe` 到发布目录。
3. 验证单 EXE（见下）。
4. 可选：附带 README.md。

## 如何测试

### 冒烟测试（自动化探针，位于 test/）

探针均为独立小工具，直接编译：

```bat
cl /nologo /EHsc test\xxx.cpp
```

| 探针 | 用途 |
| ---- | ---- |
| mutex_probe | 检查单实例互斥体存在性 |
| window_probe | 枚举窗口（主窗口是否隐藏、标题是否正确） |
| dialog_probe / dlg_detail | 检查是否有错误对话框及文本 |
| poll_probe | 定时轮询进程/窗口状态 |
| audio_session_probe | 默认端点音频会话 + 峰值（验证真实出声） |
| audio_endpoints_probe | 全部渲染端点 + 会话（验证输出设备） |
| tray_find | 通过 Shell_NotifyIconGetRect 定位托盘图标 |
| player_state_probe | 读取运行中实例的文件名 / 播放状态 / 当前时间 / 总时长 / 进度条位置 / 界面音量 / **会话实际音量**，并读取该进程在所有活动端点上的音频峰值；可模拟操作：`--toggle=i`（播放/暂停）、`--seek=i:pct`（拖动进度条）、`--volume=i:v`（音量滑块）、`--close=i`（关闭窗口）；`--dump` 打印布局几何并检测控件重叠 |
| window_shot | 把播放器窗口截图保存为 24 位 BMP，用于目视核对布局 |
| audio_loopback_probe | WASAPI 环回采集指定进程所连端点的**真实渲染幅度**（peak / RMS），用于验证音量等只能靠听感判断的功能 |
| proc_mem_probe | 打印指定进程的工作集 / 私有内存 / 峰值 / 句柄 / 线程 / GDI / USER 对象数，用于内存与资源泄漏回归 |
| mf_codec_probe | 枚举本机 Media Foundation 的音频解码器与编码器，确认哪些格式原生可用 |
| mf_load_cycle_probe | 隔离复现 MF 的加载/释放循环，把泄漏精确定位到 resolve / topology / session 某一层 |
| resolve_probe | 对比「按扩展名 / 字节流+MIME / 属性存储+MIME」三种解析机制，打印解析出的流信息 |
| drop_probe | `checktarget` 验证应用窗口是否已注册 OLE 投放目标；`selftest` 校验 HDROP 解析；`traycheck` 取证托盘窗口有无投放目标 |
| drag_probe | 用 `SHCreateDataObject` 造 CF_HDROP 数据对象并驱动真实 `DoDragDrop`（含合成鼠标输入） |
| fetch_samples.py | 下载 MP3/WAV/MP4/M4A/FLAC/AAC/WMA 测试样本，并派生错误扩展名/损坏样本到 `test/samples/` |

### 格式回归流程

```bat
python test\fetch_samples.py            :: 下载 + 派生样本到 test\samples\
SilentPlayer.exe test\samples\t.mp3     :: 逐个打开
player_state_probe.exe 2 500            :: 看 file / state / total / peak
```

判定标准：状态 `PLAYING`、总时长正确、**peak > 0**（真实出声）。只看界面文字不可信。

`test/samples/` 是生成物（约 27 MB），可随时删除后用脚本重建。

#### 错误扩展名 / 损坏文件回归

```bat
SilentPlayer.exe test\samples\mp4content.mp3   :: 真实 MP4，名字 .mp3 → 应 PLAYING
SilentPlayer.exe test\samples\mp3content.mp4   :: 真实 MP3，名字 .mp4 → 应 PLAYING
SilentPlayer.exe test\samples\broken.mp3       :: 随机字节      → 应 BADFILE，进程存活
SilentPlayer.exe test\samples\truncated.mp3    :: 截断的 MP4 头 → 应 BADFILE，进程存活
```

打开前后用 `md5sum -c` 校验，确认原文件未被修改。
解析机制本身可用 `resolve_probe.exe <file>` 单独查看（会打印嗅探结果与三种机制的成败）。

### 拖放验证

```bat
SilentPlayer.exe test\samples\t.flac
drop_probe.exe checktarget      :: 期望 DRAGDROP_E_ALREADYREGISTERED => 投放目标已注册
drop_probe.exe traycheck        :: 取证：托盘窗口没有投放目标（所以托盘收不到拖放）
drop_probe.exe selftest <file>  :: 校验 DROPFILES 结构与 DragQueryFile 解析
```

**注意**：`WM_DROPFILES` 无法跨进程伪造——`PostMessage(WM_DROPFILES)` 会被 User32
以 `ERROR_INVALID_HANDLE` 拒绝（它会校验 HDROP 句柄），所以旧方案连测试都做不了。
合成 OLE 拖放（`drag_probe --synthetic`）能让目标收到 `DragEnter`/`DragOver`/`Drop`，
但无法稳定把投放落到指定窗口，**真实拖放的最终体验仍需人工用鼠标验证**。

### 内存回归流程

```bat
SilentPlayer.exe test\samples\t.mp3
proc_mem_probe.exe <pid> 起始
:: 连续切歌若干次后再测；工作集/私有内存应封顶而不是线性增长
proc_mem_probe.exe <pid> N次后
```

`mf_load_cycle_probe` 是查泄漏的主要工具：

```bat
mf_load_cycle_probe.exe <file> <cycles> [--mode=resolve|topology|session|restart] [--drain] [--wait]
```

`--mode=resolve` / `--mode=topology` 不碰会话，`--mode=session` 才走 `SetTopology + Start + Stop`；
逐层对比即可定位泄漏所在。

`player_state_probe` 用法：

```bat
player_state_probe.exe [samples] [intervalMs] [--toggle=i,j] [--seek=i:pct] [--volume=i:v] [--close=i] [--destroy=i] [--trayitem=i:n]
player_state_probe.exe 26 500 --toggle=9,15
player_state_probe.exe 26 500 --seek=4:50 --volume=8:40 --close=16
player_state_probe.exe 0 0 --dump          :: 只看布局几何与重叠
player_state_probe.exe 0 0 --traydump      :: 枚举托盘菜单条目（id + 文本）
player_state_probe.exe 8 600 --destroy=3   :: 第 3 次采样时点 UI「销毁」
player_state_probe.exe 6 600 --trayitem=2:2 :: 点托盘菜单第 2 个可选项（0 起）
```

它会先向主窗口发送托盘左键消息把窗口显示出来（否则 500ms 进度定时器不刷新），再逐次采样。
用于自动化验证「换歌后播放是否真的重启、进度是否归零、暂停/继续、拖动进度、关闭窗口后台播放、
销毁（UI 与托盘两条路径）」，不需要人耳判断。探针自身是 DPI 感知的，`--dump` 输出的坐标是真实物理像素。

`--trayitem` 的两个注意点：
- 菜单窗口不是普通窗口，取 HMENU 要用 `MN_GETHMENU`（`GetMenu` 拿不到）。
- 菜单刚打开时没有选中项，第一次 `VK_DOWN` 才落到第 0 项，所以激活第 N 项要按 **N+1** 次下移。

`audio_loopback_probe` 用法：

```bat
audio_loopback_probe.exe <pid> <milliseconds>
```

它会自动找到该进程所连的渲染端点（本项目常被 SteelSeries Sonar 路由到 `Sonar - Media`），
用共享模式环回采集并输出 peak / RMS。

> 注意：`IAudioMeterInformation::GetPeakValue` 反映不出 Media Foundation 内部应用的音量，
> 界面文字与会话音量也可能对不上。**验证音量必须用环回采集测真实输出幅度。**

`window_shot` 用法：

```bat
window_shot.exe E:\path\out.bmp
```

BMP 可用任意看图工具打开；若要转 PNG，可用 Python 标准库（`struct` + `zlib`）自行转换，无需第三方库。

测试样本：`sample.wav`（440 Hz，5 秒）、`sample2.wav`（523.25 Hz，8 秒），由 python wave 生成。
多格式样本由 `fetch_samples.py` 下载到 `test/samples/`（不入库，按需重新下载）。

### 架构验证（发布前必做）

```bat
dumpbin /headers build\SilentPlayer.exe | findstr /i "machine magic subsystem"
dumpbin /dependents build\SilentPlayer.exe
```

要求：`8664 machine (x64)`、`PE32+`、`subsystem (Windows GUI)`，
依赖只有系统 DLL（MF / MFPlat / ole32 / USER32 / GDI32 / SHELL32 / COMCTL32 / KERNEL32）。

**发布产物必须是 x64**：它在 x64 Windows 原生运行，在 Windows 11 on ARM64 由系统 x64 模拟层运行，
这样一个 EXE 覆盖两者。改成 ARM64EC/ARM64 会让产物无法在 x64 Windows 上运行
（CMake 配置阶段会对此给出 WARNING）。

### 典型验证流程（x64 Release）

1. 启动：`SilentPlayer.exe sample.wav` → 无主窗口（MainWindowHandle=0）、无对话框。
2. 出声：播放中 `audio_session_probe <pid>` 应看到 `state=1 peak≈0.35`（默认端点或 Sonar-Media 等被路由端点）。
3. 单实例：再次 `SilentPlayer.exe sample2.wav` → 第二进程约 0.15s 退出，进程数=1；首实例标题变为「SilentPlayer — sample2.wav」且继续出声。
4. 换歌回归（重点）：`SilentPlayer.exe sample.wav` → 等 1s → `SilentPlayer.exe sample2.wav` →
   `player_state_probe.exe 24 500` 观察：文件名变 sample2.wav 的同时**时间标签必须回到 00:00 / 00:08**，
   之后进度单调递增到 00:07，音频峰值全程 > 0（不得中断、不得继续播 sample.wav 的 5 秒）。
5. 播放结束：等 sample2.wav（8s）播完，进程仍存活、驻留托盘，进度归零且不超过总时长。
6. 托盘交互：左键显示窗口；右键菜单 播放/暂停、停止、显示、退出。

## 如何验证单 EXE

```
dir build\SilentPlayer.exe          # 单个 exe，约 260 KB
dumpbin /dependents build\SilentPlayer.exe
```

期望依赖仅为系统 DLL（2026-09-20 实测）：

```
MF.dll, MFPlat.dll, ole32.dll, USER32.dll,
GDI32.dll, SHELL32.dll, COMCTL32.dll, KERNEL32.dll
```

**不应出现** `VCRUNTIME140.dll` / `MSVCP140.dll` / `ucrtbase.dll`（说明静态 CRT 生效）。

## 如何验证没有运行时依赖

- 拷贝 `SilentPlayer.exe` 到一台干净的 Windows 10/11 x64 机器（无任何开发环境），双击直接运行。
- 或在当前机器验证：依赖列表见上；确认无 .NET / Java / Python / Qt 等运行库。

## 常用调试技巧

- 播放无声时先用 `audio_endpoints_probe` 看会话落在哪个端点（SteelSeries Sonar 等虚拟设备会把应用路由到 Sonar-Media，属正常）。
- 会话事件可用临时日志跟踪（`HandleEvent` 里写文件），定位后移除。
- 重新链接失败 LNK1104（EXE 被占用）：先 `Stop-Process -Name SilentPlayer`。

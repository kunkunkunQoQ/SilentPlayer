# SilentPlayer

<p align="left">
  <a href="https://github.com/kunkunkunQoQ/SilentPlayer/releases/latest"><img src="https://img.shields.io/github/v/release/kunkunkunQoQ/SilentPlayer?label=%E6%9C%80%E6%96%B0%E7%89%88%E6%9C%AC&color=0078D6" alt="最新版本" /></a>
  <a href="https://github.com/kunkunkunQoQ/SilentPlayer/releases"><img src="https://img.shields.io/github/downloads/kunkunkunQoQ/SilentPlayer/total?label=%E4%B8%8B%E8%BD%BD%E9%87%8F&color=success" alt="下载量" /></a>
  <img src="https://img.shields.io/badge/%E5%B9%B3%E5%8F%B0-Windows%2010%2F11%20(x64%20%7C%20ARM64)-blue?logo=windows" alt="平台" />
  <img src="https://img.shields.io/badge/%E8%AF%AD%E8%A8%80-C%2B%2B17-00599C?logo=c%2B%2B" alt="C++17" />
  <img src="https://img.shields.io/badge/%E4%BD%93%E7%A7%AF-~313%20KB-brightgreen" alt="体积" />
  <a href="https://www.ifdian.net/a/koukou021"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8C%81%E9%A1%B9%E7%9B%AE-%E7%88%B1%E5%8F%91%E7%94%B5-FF7A2F?logo=afdian&logoColor=white" alt="支持项目-爱发电" /></a>
</p>

极简、静默、低资源占用的 Windows 原生音频播放器。

> **核心流程**：打开音频 → 自动播放 → 静默隐藏 → 托盘驻留

它只专注做好一件事——**把音频文件静默、纯粹地播放出来**。  
平时完全看不到界面，不抢占焦点，也不占用任务栏空间；点击系统托盘图标才显示极简的播放控制窗口。单 EXE 仅约 313 KB，基于 C++17 与 Win32 / Windows Media Foundation 原生打造，**零第三方依赖、免安装运行**。

---

## ✨ 核心特性

- **🎯 极致静默与无感伴随**
  - **打开即播**：支持命令行参数、系统「打开方式」关联，或直接**拖拽音频文件到播放器窗口**。
  - **托盘驻留**：左键单击切换控制窗口显隐；关闭窗口继续后台播放，托盘选择「退出」才彻底关闭。
  - **单实例运行**：重复打开新文件自动转交现有实例播放；播放结束自动清理资源，不自动跳播下一首。
  - **停止 ≠ 销毁**：「停止」保留曲目与时长信息；「销毁」才彻底释放媒体并回到空闲待机态。

- **🎙️ 虚拟麦克风推流 / 双路同时输出（独家特色）**
  - **输出到麦克风**：利用底层**按应用音频路由**（Per-App Audio Routing），仅将本播放器的音频输出切到虚拟麦克风设备（支持 **VB-Cable、赛睿 GG (SteelSeries Sonar)、VoiceMeeter** 等常见虚拟麦克风），**完全不改动系统默认播放设备**，语音聊天中的队友可清晰听到音频。
  - **麦克风 + 播放设备同时输出**：采用双独立会话并发解码渲染——**你自己听得到，语音里的朋友也听得到**！两路模式互斥，取消勾选立即恢复原状，不留任何系统残留。

- **🔍 真实内容嗅探与原生解码**
  - **按文件内容识别**：读取文件头智能嗅探真实格式，不信任也不依赖扩展名（即便 `.mp4` 扩展名改成 `.mp3` 也能准确认出并播放）。
  - **纯正系统原生**：全部依托 Windows Media Foundation 原生解码，**零第三方库、零外部解码器**；仅提取音频流，自动忽略视频轨道。

- **🎛️ 便捷交互与精细控制**
  - **滚轮调音量**：在控制窗口**任意位置**滚动鼠标滚轮均可调节音量（±5%）。
  - **点哪跳哪**：进度条与音量条均支持在轨道任意位置**单次点击直接跳转**。
  - **托盘状态感知**：鼠标悬停托盘图标提示当前曲目与播放状态；托盘右键菜单顶部直观展示当前文件名。

- **⏱️ 360 秒无播放自动退出**
  - 累计 360 秒未处于播放状态（停止、暂停、播放完成或未打开文件）自动优雅退出，**默认开启**，托盘菜单可随心开关。放完即走，省心省资源。

- **🤖 命令行控制与脚本自动化**
  - 提供完整的二次调用命令行接口（`--toggle` / `--stop` / `--show` / `--exit` / `--volume=<0-100>` / `--micmode=<off|mic|dual>`），适合与脚本、快捷键工具、Stream Deck 或自动化软件联动。

- **📦 绿色纯净、单 EXE 双架构**
  - 静态编译（`/MT`），无需安装 VC++ 运行库、无需 .NET、不写注册表、不留缓存。
  - 单个 x64 可执行文件覆盖两种环境：Windows x64 原生运行 + Windows 11 on ARM64 模拟运行。

---

## 🎵 支持格式

| 格式分类 | 支持扩展名 / 容器 | 说明 |
| :--- | :--- | :--- |
| **标准音频** | `.mp3` / `.wav` / `.flac` / `.wma` / `.asf` | 纯系统 Media Foundation 原生硬解 |
| **高级流媒体/容器** | `.m4a` / `.aac` / `.mp4`（仅音频轨） | 自动剔除视频流，仅解析并播放音频轨 |

> 提示：格式识别以文件头数据特征为准，不依赖文件后缀名，不转码、不修改原文件。

---

## 🚀 使用指南

### 快速播放

```bat
# 命令行播放指定音频（打开即自动播放并驻留托盘）
SilentPlayer.exe "C:\Music\song.mp3"
```

- **图形界面使用**：也可以直接将音频文件拖放到 SilentPlayer 窗口，或在系统里设置「打开方式」关联音频文件。
- **窗口与托盘**：单击系统托盘图标可在显示/隐藏之间快速切换；点击窗口右上角关闭按钮仅隐藏界面，音频仍会在后台继续播放。

### 快捷键与鼠标操作

| 操作 | 对应功能 |
| :--- | :--- |
| **空格键 (Space)** | 播放 / 暂停 |
| **左方向键 (←)** | 快退 5 秒 |
| **右方向键 (→)** | 快进 5 秒 |
| **Esc 键** | 隐藏播放器窗口（继续后台播放） |
| **鼠标滚轮** | 窗口任意区域滚动即可调节音量（±5%） |
| **进度条 / 音量条点击** | 点哪跳哪（无需按住拖拽，点击任意位置直接跳到对应进度/音量） |
| **托盘图标左键单击** | 切换显示 / 隐藏控制窗口 |

### 命令行控制参数

已运行 SilentPlayer 时，再次运行并传入以下参数可向后台实例发送控制指令：

```bat
SilentPlayer.exe --toggle              # 切换播放 / 暂停
SilentPlayer.exe --stop                # 停止播放
SilentPlayer.exe --show                # 唤出并显示控制窗口
SilentPlayer.exe --volume=75           # 将音量设为 75%（范围 0-100）
SilentPlayer.exe --micmode=dual        # 切换输出模式：off（默认设备）/ mic（仅麦克风）/ dual（麦克风+设备）
SilentPlayer.exe --exit                # 退出 SilentPlayer
```

---

## 📥 下载与 Releases

### 立即下载最新版

| 产物文件 | 平台架构 | 文件体积 | 运行环境要求 | 快速下载 |
| :--- | :--- | :--- | :--- | :--- |
| **`SilentPlayer.exe`** | Windows x64 / ARM64 | ~313 KB (320,000 字节) | **零依赖**（免安装，无需 VC++ 运行库 / .NET） | [⬇️ **直接下载 v1.0.3**](https://github.com/kunkunkunQoQ/SilentPlayer/releases/download/v1.0.3/SilentPlayer.exe) |

- **安全校验 (SHA-256)**：
  ```
  718d0b840beb41cc93a88abff701b309bcba449a1296dca96bd15032d83383b6  SilentPlayer.exe
  ```

> 📌 **版本发布历史**：更多历史版本与更新细节，请查阅 [GitHub Releases 页面](https://github.com/kunkunkunQoQ/SilentPlayer/releases)。

---

## 🛠️ 从源码构建

需要环境：Visual Studio 2022（MSVC v143）+ Windows SDK 10.0.22621 + CMake 3.20+。

```bash
# 1. 载入预置 MSVC x64 + Windows SDK 构建环境
source tools/buildenv.sh

# 2. 生成构建文件
"$CMAKE" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release

# 3. 编译发布产物
"$CMAKE" --build build
```

编译产物位于 `build/SilentPlayer.exe`。技术栈采用纯粹的 C++17 / Win32 / Windows Media Foundation，静态链接 CRT（`/MT`）。

---

## 🧩 相关作品与组合用法

- **[SonicRoute](https://github.com/kunkunkunQoQ/SonicRoute)**：Windows 按应用音量与设备路由、实时电平、快速面板、快捷键、托盘滚轮、OSD、自动化与命令行。SilentPlayer 的「输出到麦克风」即是按应用路由的经典场景。

> **💡 组合玩法**：将 SilentPlayer 配置为 SonicRoute 自动化里的「启动程序」动作，即可打造类似 SoundPad 的按键音频播放器，一键播放指定提示音或背景音效。

<a href="https://github.com/kunkunkunQoQ/XiaoYue-Comics" title="小跃的奇妙日常"><img src="https://raw.githubusercontent.com/kunkunkunQoQ/SonicRoute/master/docs/images/mascot-comics.png" width="84" align="right" alt="点击从蛋壳里探头、抱着漫画册的小跃，阅读漫画连载"></a>

**[小跃的奇妙日常](https://github.com/kunkunkunQoQ/XiaoYue-Comics)**

从鸡蛋里蹦出来的魔法少女，以及她越帮越忙的离谱日常。 [阅读漫画](https://github.com/kunkunkunQoQ/XiaoYue-Comics)

---

<a href="https://github.com/kunkunkunQoQ/SonicRoute" title="音跃 SonicRoute"><img src="https://raw.githubusercontent.com/kunkunkunQoQ/SonicRoute/master/docs/images/mascot-cozy-knit.png" width="56" align="right" alt="点击看板娘发现一个彩蛋"></a>

感谢使用 SilentPlayer。欢迎 [反馈问题与建议](https://github.com/kunkunkunQoQ/SilentPlayer/issues)。

**作者主页：[哔哩哔哩](https://b23.tv/TDqSAKM) | [爱发电](https://www.ifdian.net/a/koukou021) | [GitHub](https://github.com/kunkunkunQoQ)**

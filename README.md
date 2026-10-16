# SilentPlayer

![GitHub 下载量](https://img.shields.io/github/downloads/kunkunkunQoQ/SilentPlayer/total?label=下载量)
[![支持项目-爱发电](https://img.shields.io/badge/支持项目-爱发电-FF7A2F?logo=afdian&logoColor=white)](https://www.ifdian.net/a/koukou021)

极简、静默、低资源占用的 Windows 音频播放器：**打开音频 → 自动播放 → 隐藏 → 托盘驻留**。

它只做一件事——把音频文件静默地播放出来：平时完全看不到界面，点击系统托盘图标才显示一个极简的播放控制窗口。

## 功能

- 自动播放：命令行参数 / 打开方式 / **拖文件到播放器窗口**
- 托盘驻留：左键切换显示/隐藏；右键菜单（打开文件…、播放/暂停、停止、销毁、输出到麦克风、麦克风 + 播放设备同时输出、360 秒无播放自动退出、显示播放器、退出）
- 单实例；**停止 ≠ 销毁**；播放结束自动清理，不自动播下一首
- 格式按**文件内容**识别，不信任扩展名；不转码、不修改原文件
- 鼠标滚轮在窗口任意位置调音量；空格暂停/继续；←/→ 快退快进 5 秒；Esc 隐藏窗口
- 进度条与音量条都是"点哪跳哪"
- 托盘悬停显示当前播放；菜单顶部显示当前文件名
- 命令行可发控制命令：`--toggle` / `--stop` / `--show` / `--exit` / `--volume=<0-100>` / `--micmode=<off|mic|dual>`
- **360 秒没有在播放就自动退出**（默认开启，托盘菜单可关）
- 不保存播放列表/历史/状态；关闭窗口继续后台播放
- **单 EXE 双架构**：一个 x64 文件，Windows x64 原生运行，Windows 11 on ARM64 经 x64 模拟层运行

> 「输出到麦克风」用**按应用音频路由**把本播放器的输出切到虚拟麦克风设备（只影响本播放器，不改系统默认设备）；
> 支持 **VB-Cable、赛睿 GG（SteelSeries Sonar）**、VoiceMeeter 等常见虚拟麦克风设备；
> 取消勾选立即切回原设备继续正常听歌。
>
> 「**麦克风 + 播放设备同时输出**」在此之上让你**自己也听得到**（两路同时出声），
> 与「输出到麦克风」互斥，两个都不勾就是普通播放。

## 支持格式

MP3 / WAV / MP4（仅音频轨）/ M4A / AAC / FLAC / WMA 等，全部 Windows Media Foundation 原生解码，**无第三方库**。

## 使用

```bat
SilentPlayer.exe music.mp3    :: 打开即自动播放，驻留托盘
```

点击托盘图标（左键）显示播放器；关闭窗口继续播放，托盘「退出」才真正结束程序。

## 下载

[Releases](https://github.com/kunkunkunQoQ/SilentPlayer/releases) 中的 `SilentPlayer.exe`（单个可执行文件，约 313 KB，复制即用）。

**系统要求**：Windows x64（Windows 11 on ARM64 可用）；无需安装 .NET / 运行库 / 任何第三方依赖。

## 构建

需要 Visual Studio 2022（MSVC v143）+ Windows SDK 10.0.22621 + CMake 3.20+。

```bash
source tools/buildenv.sh                              # 固化好的 MSVC x64 + SDK 环境
"$CMAKE" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release   # 首次配置
"$CMAKE" --build build                                # 产出 build/SilentPlayer.exe
```

技术栈：C++17 / Win32 / Windows Media Foundation，静态 CRT（`/MT`），**零第三方库**。
发布产物必须是 **x64**（同时覆盖 x64 原生与 Windows 11 on ARM64 的 x64 模拟层）。

## 🧩 其他作品与组合用法

- **[SonicRoute](https://github.com/kunkunkunQoQ/SonicRoute)**：Windows 按应用音量与设备路由、实时电平、快速面板、快捷键、托盘滚轮、OSD、自动化与命令行。本播放器的「输出到麦克风」就是按应用路由的一种应用场景。
- **[WinAudioRoute](https://github.com/kunkunkunQoQ/WinAudioRoute)**：Windows 音频控制库（.NET），提供设备、会话、按应用路由、事件与 CLI。

> 组合用法：把 SilentPlayer 配成 SonicRoute 自动化里的「启动程序」动作，即可实现按键播放音效（类似 SoundPad）。

<a href="https://github.com/kunkunkunQoQ/XiaoYue-Comics" title="小跃的奇妙日常"><img src="https://raw.githubusercontent.com/kunkunkunQoQ/SonicRoute/master/docs/images/mascot-comics.png" width="84" align="right" alt="点击从蛋壳里探头、抱着漫画册的小跃，阅读漫画连载"></a>

**[小跃的奇妙日常](https://github.com/kunkunkunQoQ/XiaoYue-Comics)**

从鸡蛋里蹦出来的魔法少女，以及她越帮越忙的离谱日常。 [阅读漫画](https://github.com/kunkunkunQoQ/XiaoYue-Comics)

---

<a href="https://github.com/kunkunkunQoQ/SonicRoute" title="音跃 SonicRoute"><img src="https://raw.githubusercontent.com/kunkunkunQoQ/SonicRoute/master/docs/images/mascot-cozy-knit.png" width="56" align="right" alt="点击看板娘发现一个彩蛋"></a>

感谢使用 SilentPlayer。 [查看文档](https://github.com/kunkunkunQoQ/SonicRoute/wiki) · [反馈问题](https://github.com/kunkunkunQoQ/SilentPlayer/issues)。

**作者主页：[哔哩哔哩](https://b23.tv/TDqSAKM) | [爱发电](https://www.ifdian.net/a/koukou021) | [GitHub](https://github.com/kunkunkunQoQ)**

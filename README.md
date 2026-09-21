# SilentPlayer

极简、静默、低资源占用的 Windows 音频播放器：**打开音频 → 自动播放 → 隐藏 → 托盘驻留**。

它只做一件事——把音频文件静默地播放出来：平时完全看不到界面，点击系统托盘图标才显示一个极简的播放控制窗口。

## 功能

- 自动播放：命令行参数 / 打开方式 / **拖文件到播放器窗口**
- 托盘驻留：左键显示播放器；右键菜单（播放/暂停、停止、销毁、麦克风输出提示、显示播放器、退出）
- 单实例；**停止 ≠ 销毁**；播放结束自动清理，不自动播下一首
- 格式按**文件内容**识别，不信任扩展名；不转码、不修改原文件
- 不保存播放列表/历史/状态；关闭窗口继续后台播放
- **单 EXE 双架构**：一个 x64 文件，Windows x64 原生运行，Windows 11 on ARM64 经 x64 模拟层运行

> 麦克风输出提示只做检测与说明，不会把音频写进麦克风，详见 [docs/CAPABILITIES.md](docs/CAPABILITIES.md)。

## 支持格式

MP3 / WAV / MP4（仅音频轨）/ M4A / AAC / FLAC / WMA 等，全部 Windows Media Foundation 原生解码，**无第三方库**。实测矩阵见 [docs/CHANGELOG.md](docs/CHANGELOG.md)。

## 使用

```bat
SilentPlayer.exe music.mp3    :: 打开即自动播放，驻留托盘
```

点击托盘图标（左键）显示播放器；关闭窗口继续播放，托盘「退出」才真正结束程序。

## 下载

[Releases](https://github.com/kunkunkunQoQ/SilentPlayer/releases) 中的 `SilentPlayer.exe`（单个可执行文件，约 270 KB，复制即用）。

**系统要求**：Windows x64（Windows 11 on ARM64 可用）；无需安装 .NET / 运行库 / 任何第三方依赖。

## 文档

[能力清单](docs/CAPABILITIES.md) · [开发文档](docs/DEVELOPMENT.md) · [架构说明](docs/ARCHITECTURE.md) · [发布规范](docs/RELEASING.md) · [变更记录](docs/CHANGELOG.md)

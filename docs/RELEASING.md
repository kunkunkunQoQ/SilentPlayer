# SilentPlayer 发布规范（GitHub）

> 目的：把「GitHub 仓库创建 → 提交 → 推送 → Release 发行」的完整流程固化为可复现的规范，
> 任何开发者或 AI 拿到本文件即可按步骤发布新版本。
> 本规范基于 2026-09-21 首次发布（v1.0.0）的实测流程整理，含踩坑记录。

## 0. 发布流程总览

```
① 本地验证（构建/测试/架构检查）→ ② 本地提交 → ③ 推送 → ④ 新建仓库（仅首次）→ ⑤ 创建 Release 并上传 EXE → ⑥ 验证
```

- 首次发布：执行 ①→⑥（④ 只在仓库不存在时执行）。
- 版本更新：执行 ①→②→③→⑤→⑥（④ 跳过；⑤ 换新 tag）。

## 1. 前置条件

| 项 | 要求 |
| ---- | ---- |
| Git | 已安装（2.55+ 实测可用），`git config --global user.name / user.email` 已配置 |
| GitHub 凭据 | Windows 凭据管理器中已保存账号的令牌（Git Credential Manager） |
| 令牌权限 | 需包含 `repo`（读写仓库/Release）、`workflow`（如需 CI） |

### 1.1 获取访问令牌（不要写死令牌）

令牌从凭据管理器动态读取，任何命令都不要把令牌明文写进脚本或文档：

```powershell
$tok = (("protocol=https`nhost=github.com`n`n") | git credential fill 2>$null | Select-String '^password=').ToString().Substring(9)
```

验证令牌（应返回账号信息，如 `login=kunkunkunQoQ`）：

```powershell
Invoke-RestMethod -Uri 'https://api.github.com/user' -Headers @{ Authorization = "token $tok"; 'User-Agent'='Doubao-Agent' }
```

### 1.2 代理问题（本机踩坑）

- 全局 Git 配置了代理 `http.proxy=http://127.0.0.1:7890`；**代理不可达时推送会失败**
  （报 `Failed to connect to github.com:443 over proxy`）。
- 处理：只对**本次命令**临时禁用代理，不改全局配置：

```powershell
git -c http.proxy= -c https.proxy= push -u origin main
```

- GitHub API（`api.github.com` / `uploads.github.com`）直连通常可用，不受此影响。

## 2. 发布资产规范（重要）

| 项 | 值 |
| ---- | ---- |
| 发布产物 | `build\SilentPlayer.exe`（Release 构建，x64，约 270 KB，单 EXE） |
| 上传平台 | GitHub Release 资产 |
| MSIX | **不制作**（除非用户明确要求） |
| MSI | 仅当用户**明确要求**时制作；MSI/MSIX **一律不上传 GitHub** |

- 发布前必须完成「架构验证」（见 DEVELOPMENT.md）：`8664 machine (x64)`、
  `PE32+`、`Windows GUI`、依赖仅系统 DLL。
- 版本号统一来自 `CMakeLists.txt` 的 `project(SilentPlayer VERSION x.y.z ...)` 与
  `resources/app.rc` 的 VERSIONINFO，两者需一致。

## 3. 本地提交规范

### 3.1 排除项（.gitignore 已配置，勿改动白名单）

- `build/`、`build-debug/`（构建产物）
- `test/*.exe`、`test/*.obj`（探针二进制，源码才入库）
- `test/samples/`（约 25–27 MB 下载样本，可 `python test\fetch_samples.py` 重建，不入库）
- `.workbuddy-ai/`（AI 会话记忆）

### 3.2 提交信息格式

```
<type>: SilentPlayer vX.Y.Z

<要点摘要，可选>
```

- `type` 用 `Initial commit` / `Release` / `Docs` / `Fix` 等。
- 首行不超过 72 字符，说明版本号；正文列出本次改动要点。
- 发布前提交顺序：源码/文档变更 → 构建出 Release EXE → 打 tag。

```powershell
Set-Location E:\kunkun\slientPlayer
git add -A
git commit -m "Release: SilentPlayer vX.Y.Z`n`n<要点>"
```

## 4. 新建 GitHub 仓库（仅首次）

```powershell
$body = @{ name = 'SilentPlayer'; description = '<中文描述>'; public = $true } | ConvertTo-Json
```

**⚠ 编码红线（本机踩坑）**：PowerShell 5.1 的 `Invoke-RestMethod` 直接传 JSON 字符串时
中文会变成 `?`（乱码）。**必须**先把 JSON 转成 UTF-8 字节再发送：

```powershell
$utf8 = [System.Text.Encoding]::UTF8
$bytes = $utf8.GetBytes($json)
Invoke-RestMethod -Uri 'https://api.github.com/user/repos' -Method Post `
    -Headers @{ Authorization = "token $tok"; 'User-Agent'='Doubao-Agent' } `
    -ContentType 'application/json; charset=utf-8' -Body $bytes
```

所有带中文的 API 请求体（仓库描述、Release 正文、issue 等）都必须走这个 UTF-8 字节流程。

### 4.1 仓库标签（topics）

```powershell
# 标签只允许小写字母、数字、连字符；PUT 会整体替换，需一次传全量
$json = @{ names = @('c-plus-plus','win32','windows','audio-player','media-foundation','cmake') } | ConvertTo-Json
# 同样用 UTF-8 字节发送，URI 为 .../repos/kunkunkunQoQ/SilentPlayer/topics，方法 Put
```

## 5. 推送

```powershell
git remote add origin https://github.com/kunkunkunQoQ/SilentPlayer.git
git -c http.proxy= -c https.proxy= push -u origin main
```

推送成功标志：输出 `* [new branch] main -> main`（stderr 显示不代表失败）。

## 6. 创建 Release 并上传 EXE

### 6.1 创建 Release（自动打 tag）

```powershell
$body = @{
    tag_name         = 'v1.0.0'          # 每次发布递增，如 v1.1.0
    target_commitish = 'main'
    name             = 'SilentPlayer v1.0.0'
    draft            = $false            # 直接发布，不用草稿
    body             = @'
## 版本标题
## 功能列表（README 要点）
## 系统要求
## 下载（SilentPlayer.exe）
'@
} | ConvertTo-Json
# UTF-8 字节发送到 /repos/{owner}/{repo}/releases，方法 Post
# 成功返回 html_url 与 id（id 用于 6.2 上传）
```

### 6.2 上传发布资产（EXE）

```powershell
$uploadUrl = "https://uploads.github.com/repos/kunkunkunQoQ/SilentPlayer/releases/{id}/assets?name=SilentPlayer.exe"
Invoke-RestMethod -Uri $uploadUrl -Method Post `
    -Headers @{ Authorization = "token $tok"; 'User-Agent'='Doubao-Agent' } `
    -ContentType 'application/octet-stream' -InFile 'E:\kunkun\slientPlayer\build\SilentPlayer.exe'
```

成功标志：返回 `name=SilentPlayer.exe`、`size` 与 `browser_download_url`。

## 7. 发布验证清单（发布后必做）

1. **仓库**：`GET /repos/{owner}/{repo}` → description 无乱码、topics 齐全。
2. **Release**：`GET /repos/{owner}/{repo}/releases/tags/v1.0.0` →
   `draft=False`、body 中文无乱码、assets 数量 = 1。
3. **Tag**：`GET /repos/{owner}/{repo}/git/refs/tags/v1.0.0` → 指向发布提交。
4. **下载**：对 `browser_download_url` 发 HEAD 请求 → `HTTP 200` 且
   `Content-Length` 与本地 EXE 字节数一致（276,480 @ v1.0.0）。

## 8. 常见问题

| 问题 | 原因 | 处理 |
| ---- | ---- | ---- |
| 仓库描述 / Release 正文中文变 `?` | PS 5.1 发送 JSON 非 UTF-8 | 改用 UTF-8 字节发送，并 `PATCH` 修正已乱码字段 |
| `Failed to connect ... over proxy` | 全局代理不可达 | 单次命令 `-c http.proxy= -c https.proxy=` |
| `gh` 命令不存在 | 未安装 GitHub CLI | 本项目规范使用 REST API，不依赖 gh |
| 令牌无权限（403） | scope 不足 | 需要 `repo` + `workflow`；重新登录凭据管理器 |
| 上传 422 / 资产重复 | 同名资产已存在 | 删除旧资产或改文件名后重传 |

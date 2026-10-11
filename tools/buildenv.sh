#!/usr/bin/env bash
# =============================================================================
# SilentPlayer 构建环境（MSVC x64 + Windows SDK），在 Git Bash / MSYS 下使用。
#
# 用法：
#   source tools/buildenv.sh
#   "$CMAKE" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
#   "$CMAKE" --build build
#
# 为什么不用 vcvars64.bat：
#   1) 本机 VS 2022 实例注册不完整，"Visual Studio 17 2022" CMake 生成器不可用，
#      只能用 NMake Makefiles（需要 cl.exe/link.exe 已在 PATH）；
#   2) 受限环境（自动化沙箱）禁止 cmd.exe，无法 call vcvars64.bat。
#   因此这里直接设置 INCLUDE / LIB / PATH —— 效果等价。
#
# 注意：
#   - INCLUDE / LIB 必须用 **Windows 风格路径 + 分号分隔**（cl.exe 只认这种）。
#   - PATH 用 **Unix 风格路径**（冒号分隔，供 bash 查找 cl.exe / dumpbin.exe）。
#   - INCLUDE 里的 winrt 目录不能漏（wrl/client.h 在这里），否则 C1083。
#   - 链接前必须结束正在运行的 SilentPlayer.exe，否则 LNK1104。
# =============================================================================

export MSVC_ROOT="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.38.33130"
export SDK_ROOT="/c/Program Files (x86)/Windows Kits/10"
export SDK_VER="10.0.22621.0"
export CMAKE="/c/Program Files/CMake/bin/cmake.exe"

export INCLUDE="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\shared;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\winrt"
export LIB="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64"
export PATH="$MSVC_ROOT/bin/Hostx64/x64:$SDK_ROOT/bin/$SDK_VER/x64:$PATH"

# 便捷：cl 在 MSYS 下会把 /nologo 之类的开关当成路径，编译探针时统一加 MSYS_NO_PATHCONV=1
export MSYS_NO_PATHCONV=1

# 自检
if ! command -v cl.exe >/dev/null 2>&1; then
    echo "[buildenv] 警告：PATH 里找不到 cl.exe，请检查 MSVC 版本目录是否变化" >&2
fi

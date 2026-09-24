cmake_minimum_required(VERSION 3.20)

project(SilentPlayer VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # 静态链接 CRT：最终 EXE 无需 vcruntime 再分发，单个 EXE 直接运行。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 架构策略（重要，勿随意改动发布产物）：
#   发布产物必须是 **x64** 的 SilentPlayer.exe。原因：
#     - Windows x64            → 本机原生运行
#     - Windows 11 on ARM64    → 由系统内置的 x64 模拟层运行（系统 DLL 是 Arm64X PE，
#                                可被 x64 进程加载，因此 Media Foundation 等全部可用）
#   ARM64EC / ARM64X 虽然能让代码在 ARM64 上原生执行，但**只能跑在 ARM64 上**，
#   无法在 x64 Windows 上运行，因此不满足「一个 EXE 同时兼容两者」的要求。
#   本项目只依赖 Windows 系统 API（无第三方 DLL），所以 x64 产物在模拟层下功能完整。
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(MSVC_CXX_ARCHITECTURE_ID STREQUAL "ARM64EC")
        message(WARNING
            "当前目标是 ARM64EC：产物将【无法】在 x64 Windows 上运行。"
            "发布用的单 EXE 必须是 x64（Windows 11 on Arm64 会通过 x64 模拟运行它）。")
    elseif(MSVC_CXX_ARCHITECTURE_ID STREQUAL "ARM64")
        message(WARNING
            "当前目标是 ARM64：产物将【无法】在 x64 Windows 上运行。"
            "发布用的单 EXE 必须是 x64（Windows 11 on Arm64 会通过 x64 模拟运行它）。")
    else()
        message(STATUS
            "目标架构 = ${MSVC_CXX_ARCHITECTURE_ID}（发布产物应为 x64：x64 原生 + "
            "Windows 11 on Arm64 通过 x64 模拟运行）")
    endif()
endif()

add_executable(SilentPlayer WIN32
    src/main.cpp
    src/App.cpp
    src/App.h
    src/AudioPlayer.cpp
    src/AudioPlayer.h
    src/AudioDevices.cpp
    src/AudioDevices.h
    src/PerAppAudio.cpp
    src/PerAppAudio.h
    src/PlayerWindow.cpp
    src/PlayerWindow.h
    src/SingleInstance.cpp
    src/SingleInstance.h
    src/TrayIcon.cpp
    src/TrayIcon.h
    resources/app.rc
)

target_compile_definitions(SilentPlayer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_include_directories(SilentPlayer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

if(MSVC)
    # /utf-8：源码含中文 UI 文本，统一按 UTF-8 编译
    target_compile_options(SilentPlayer PRIVATE /utf-8 /W3)
endif()

target_link_libraries(SilentPlayer PRIVATE
    runtimeobject
    mf
    mfplat
    mfuuid
    ole32
    oleaut32
    user32
    gdi32
    shell32
    comctl32
    advapi32
)

set_target_properties(SilentPlayer PROPERTIES
    OUTPUT_NAME "SilentPlayer"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

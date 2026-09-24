// SilentPlayer — 极简静默 Windows 音频播放器。
#include <windows.h>
#include <shellapi.h>
#include <string>

#include "App.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;
    (void)nCmdShow;

    // 每显示器 DPI 感知（Windows 10 1703+），保证控件清晰。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 解析命令行：
    //   SilentPlayer.exe <音频文件路径>      打开并播放
    //   SilentPlayer.exe --toggle|--stop|--show|--exit|--volume=<0-100>
    //                                        把控制命令发给已运行的实例
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring filePath;
    std::wstring command;
    if (argc > 1 && argv[1] && argv[1][0] != L'\0') {
        const std::wstring arg = argv[1];
        if (arg.rfind(L"--", 0) == 0) {
            command = arg.substr(2); // 去掉 "--"
        } else {
            filePath = arg;
        }
    }
    if (argv) {
        LocalFree(argv);
    }

    App app;
    return app.Run(hInstance, filePath, command);
}

// Temporary diagnostic: enumerate top-level windows and check FindWindow.
#include <windows.h>
#include <cstdio>

int wmain() {
    wprintf(L"FindWindow(SilentPlayerMainWindow): %p\n",
            FindWindowW(L"SilentPlayerMainWindow", nullptr));
    wprintf(L"FindWindow(by title): %p\n",
            FindWindowW(nullptr, L"SilentPlayer"));
    EnumWindows(
        [](HWND h, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wchar_t cls[256] = {}, title[512] = {};
            GetClassNameW(h, cls, 256);
            GetWindowTextW(h, title, 512);
            wprintf(L"  hwnd=%p pid=%lu class='%ls' title='%ls' visible=%d\n", h,
                    pid, cls, title, IsWindowVisible(h));
            return TRUE;
        },
        0);
    return 0;
}

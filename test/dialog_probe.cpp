// Temporary diagnostic: dump text of a dialog owned by a given PID.
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static DWORD g_targetPid = 0;

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_targetPid) {
        return TRUE;
    }
    wchar_t cls[256] = {}, title[512] = {}, text[2048] = {};
    GetClassNameW(h, cls, 256);
    GetWindowTextW(h, title, 512);
    SendMessageW(h, WM_GETTEXT, 2047, reinterpret_cast<LPARAM>(text));
    wprintf(L"hwnd=%p class='%ls' title='%ls' visible=%d\n  text='%ls'\n", h, cls,
            title, IsWindowVisible(h), text);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: dialog_probe.exe <pid>\n");
        return 1;
    }
    g_targetPid = static_cast<DWORD>(_wtoi(argv[1]));
    EnumWindows(EnumProc, 0);
    return 0;
}

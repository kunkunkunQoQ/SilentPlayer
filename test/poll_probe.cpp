// Temporary diagnostic: watch a PID's windows for a while, print #32770 text.
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static DWORD g_pid = 0;

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_pid) {
        return TRUE;
    }
    wchar_t cls[256] = {}, title[512] = {}, text[2048] = {};
    GetClassNameW(h, cls, 256);
    GetWindowTextW(h, title, 512);
    SendMessageW(h, WM_GETTEXT, 2047, reinterpret_cast<LPARAM>(text));
    wprintf(L"  [t+%llu ms] hwnd=%p class='%ls' title='%ls' vis=%d text='%ls'\n",
            GetTickCount64(), h, cls, title, IsWindowVisible(h), text);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: poll_probe.exe <pid> <seconds>\n");
        return 1;
    }
    g_pid = static_cast<DWORD>(_wtoi(argv[1]));
    const DWORD totalMs = static_cast<DWORD>(_wtoi(argv[2])) * 1000;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < totalMs) {
        wprintf(L"--- snapshot ---\n");
        EnumWindows(EnumProc, 0);
        Sleep(700);
    }
    return 0;
}

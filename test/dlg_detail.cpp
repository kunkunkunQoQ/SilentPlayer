// Temporary diagnostic: dump a #32770 dialog's child controls (message text).
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static HWND g_dlg = nullptr;

static BOOL CALLBACK EnumChildProc(HWND h, LPARAM) {
    wchar_t cls[256] = {}, text[2048] = {};
    GetClassNameW(h, cls, 256);
    SendMessageW(h, WM_GETTEXT, 2047, reinterpret_cast<LPARAM>(text));
    wprintf(L"    child hwnd=%p class='%ls' text='%ls'\n", h, cls, text);
    // 同时以 UTF-8 落盘：控制台代码页显示不了中文
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"E:////kunkun////slientPlayer////build////_dlg.txt",
                      L"a, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"child class=%ls text=%ls\n", cls, text);
            fclose(f);
        }
    }
    return TRUE;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"#32770") != 0) {
        return TRUE;
    }
    wchar_t title[512] = {};
    GetWindowTextW(h, title, 512);
    wprintf(L"dlg hwnd=%p title='%ls' vis=%d owner=%p style=0x%lX\n", h, title,
            IsWindowVisible(h), GetWindow(h, GW_OWNER), GetWindowLongPtrW(h, GWL_STYLE));
    EnumChildWindows(h, EnumChildProc, 0);
    return TRUE;
}

int wmain() {
    EnumWindows(EnumProc, 0);
    return 0;
}

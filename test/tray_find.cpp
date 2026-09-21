// Temp diagnostic: locate SilentPlayer tray icon rect via Shell_NotifyIconGetRect.
#include <windows.h>
#include <shellapi.h>
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

int wmain() {
    HWND hwnd = FindWindowW(L"SilentPlayerMainWindow", nullptr);
    wprintf(L"player window=%p\n", hwnd);
    if (!hwnd) return 1;
    NOTIFYICONIDENTIFIER nii = {};
    nii.cbSize = sizeof(nii);
    nii.hWnd = hwnd;
    nii.uID = 1;
    nii.guidItem = GUID_NULL;
    RECT r{};
    HRESULT hr = Shell_NotifyIconGetRect(&nii, &r);
    wprintf(L"Shell_NotifyIconGetRect hr=0x%08lX\n", hr);
    if (SUCCEEDED(hr)) {
        wprintf(L"icon rect=(%ld,%ld)-(%ld,%ld) size=%ldx%ld\n", r.left, r.top,
                r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    }
    return 0;
}

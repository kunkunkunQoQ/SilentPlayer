// 托盘图标探针：定位图标位置、悬停并截取屏幕区域，用于核对 tooltip / 气泡等
// 只能"看"出来的行为（控制台读不到托盘提示文字）。
//
// usage:
//   tray_probe.exe rect                     打印图标矩形（Shell_NotifyIconGetRect）
//   tray_probe.exe hover <out.bmp> [width]  把光标移到图标上悬停后截图
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr UINT kTrayIconId = 1;
constexpr const wchar_t* kWindowClass = L"SilentPlayerMainWindow";

// 找到托盘图标矩形。返回 false 表示图标当前不在通知区域（可能被收进了溢出区）。
bool GetTrayRect(RECT* out) {
    HWND hwnd = FindWindowW(kWindowClass, nullptr);
    if (!hwnd) {
        wprintf(L"SilentPlayer window not found\n");
        return false;
    }
    NOTIFYICONIDENTIFIER id = {};
    id.cbSize = sizeof(id);
    id.hWnd = hwnd;
    id.uID = kTrayIconId;
    const HRESULT hr = Shell_NotifyIconGetRect(&id, out);
    wprintf(L"Shell_NotifyIconGetRect hr=0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        wprintf(L"  -> 图标当前不可见（可能被系统收进了溢出区，需要展开才看得到）\n");
        return false;
    }
    wprintf(L"icon rect = (%ld,%ld)-(%ld,%ld)\n", out->left, out->top, out->right,
            out->bottom);
    return true;
}

void SaveBmp(const wchar_t* path, int x, int y, int w, int h) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    const int stride = ((w * 3 + 3) / 4) * 4;
    bi.biSizeImage = stride * h;
    const DWORD dataSize = sizeof(BITMAPFILEHEADER) + sizeof(bi) + bi.biSizeImage;

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = dataSize;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        BYTE* line = new BYTE[stride];
        for (int row = h - 1; row >= 0; --row) {
            GetDIBits(mem, bmp, row, 1, line, reinterpret_cast<BITMAPINFO*>(&bi),
                      DIB_RGB_COLORS);
            fwrite(line, stride, 1, f);
        }
        delete[] line;
        fclose(f);
        wprintf(L"saved %ls (%dx%d at %d,%d)\n", path, w, h, x, y);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        wprintf(L"usage: tray_probe.exe rect | hover <out.bmp> [width]\n");
        return 1;
    }

    RECT rc = {};
    const bool visible = GetTrayRect(&rc);
    if (wcscmp(argv[1], L"rect") == 0) {
        return visible ? 0 : 2;
    }
    if (wcscmp(argv[1], L"hover") != 0 || argc < 3) {
        wprintf(L"unknown command\n");
        return 1;
    }
    if (!visible) {
        return 2;
    }

    const int width = argc > 3 ? _wtoi(argv[3]) : 420;
    POINT origin = {};
    GetCursorPos(&origin);
    // 用真实鼠标移动事件（而不是 SetCursorPos）：系统的悬停计时依赖鼠标消息。
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    INPUT in[3] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[0].mi.dx = (cx - 30) * 65535 / (sw > 0 ? sw : 1);
    in[0].mi.dy = (cy - 10) * 65535 / (sh > 0 ? sh : 1);
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[1].mi.dx = cx * 65535 / (sw > 0 ? sw : 1);
    in[1].mi.dy = cy * 65535 / (sh > 0 ? sh : 1);
    SendInput(2, in, sizeof(INPUT));
    Sleep(2000); // 等系统把 tooltip 弹出来

    // 以图标为中心，向左上取一块包含 tooltip 的区域。
    const int capX = (rc.left - width) > 0 ? (rc.left - width) : 0;
    const int capY = (rc.top - 120) > 0 ? (rc.top - 120) : 0;
    const int capW = rc.right - capX;
    const int capH = (rc.bottom + 40) - capY;
    SaveBmp(argv[2], capX, capY, capW, capH);

    SetCursorPos(origin.x, origin.y);
    return 0;
}

// Diagnostic probe: capture the SilentPlayer window to a 24-bit BMP.
// Used for visual layout checks without a human looking at the screen.
//
// usage: window_shot.cpp -> window_shot.exe [out.bmp]
#include <windows.h>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t* outPath = (argc > 1) ? argv[1] : L"window_shot.bmp";
    HWND hwnd = FindWindowW(L"SilentPlayerMainWindow", nullptr);
    if (!hwnd) {
        wprintf(L"SilentPlayer main window not found.\n");
        return 1;
    }

    SetForegroundWindow(hwnd);
    Sleep(600); // let it repaint before grabbing pixels

    RECT r{};
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        wprintf(L"bad window rect %d x %d\n", w, h);
        return 1;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    SelectObject(mem, old);

    const int stride = ((w * 3 + 3) / 4) * 4;
    const int imgSize = stride * h;
    BYTE* pixels = new BYTE[imgSize];

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h; // bottom-up
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    const int got = GetDIBits(screen, bmp, 0, h, pixels, &bi, DIB_RGB_COLORS);

    int rc = 0;
    if (got == 0) {
        wprintf(L"GetDIBits failed\n");
        rc = 1;
    } else {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42; // "BM"
        fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + imgSize;
        FILE* f = nullptr;
        if (_wfopen_s(&f, outPath, L"wb") != 0 || !f) {
            wprintf(L"cannot open output file\n");
            rc = 1;
        } else {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
            fwrite(pixels, imgSize, 1, f);
            fclose(f);
            wprintf(L"saved %ls  %d x %d\n", outPath, w, h);
        }
    }

    delete[] pixels;
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return rc;
}

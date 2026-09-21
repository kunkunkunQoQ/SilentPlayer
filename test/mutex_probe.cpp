// Temporary diagnostic: check mutex semantics exactly like SingleInstance.cpp.
#include <windows.h>
#include <cstdio>

int wmain() {
    const wchar_t* kMutexName = L"Local\\SilentPlayer_SingleInstance_Mutex";
    HANDLE m = CreateMutexW(nullptr, TRUE, kMutexName);
    DWORD err = GetLastError();
    wprintf(L"handle=%p err=%lu (ERROR_ALREADY_EXISTS=%lu)\n", m, err,
            ERROR_ALREADY_EXISTS);
    if (m) {
        ReleaseMutex(m);
        CloseHandle(m);
    }
    return 0;
}

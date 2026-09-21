// Diagnostic probe: process memory / resource counters for a given PID.
// Used to verify that media objects are really released (working set, private
// bytes, handles, threads, GDI/USER objects should not grow without bound).
//
// usage: proc_mem_probe.exe <pid> [label]
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

namespace {

DWORD ThreadCount(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    DWORD n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                ++n;
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: proc_mem_probe.exe <pid> [label]\n");
        return 1;
    }
    const DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    const wchar_t* label = (argc > 2) ? argv[2] : L"";

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        wprintf(L"[%ls] OpenProcess(pid=%lu) failed, err=%lu\n", label, pid,
                GetLastError());
        return 1;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) {
        wprintf(L"[%ls] GetProcessMemoryInfo failed\n", label);
        CloseHandle(h);
        return 1;
    }

    DWORD handles = 0;
    GetProcessHandleCount(h, &handles);
    const DWORD gdi = GetGuiResources(h, GR_GDIOBJECTS);
    const DWORD user = GetGuiResources(h, GR_USEROBJECTS);
    const DWORD threads = ThreadCount(pid);

    wprintf(L"[%ls] WS=%7.2f MB  PrivateWS=%7.2f MB  PeakWS=%7.2f MB  "
            L"handles=%-5lu threads=%-3lu gdi=%-4lu user=%-4lu\n",
            label, pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0,
            pmc.PeakWorkingSetSize / 1048576.0, handles, threads, gdi, user);

    CloseHandle(h);
    return 0;
}

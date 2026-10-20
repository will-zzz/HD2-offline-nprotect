// hd2vr_inject — minimal DLL injector (CreateRemoteThread + LoadLibraryW).
// Usage: hd2vr_inject.exe <process.exe> <path\to\hd2vr_mod.dll>
//
// NOTE (GameGuard): this is the simple injector for Phase 2 bring-up on the OFFLINE build.
// If GameGuard flags a LoadLibrary'd module (it enumerates loaded modules), switch to a
// MANUAL MAP loader so the DLL never appears in the module list. Structure stays the same;
// only the "put bytes in the target + call entry" step changes.
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>

static DWORD FindPid(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { std::wcerr << L"usage: hd2vr_inject <process.exe> <mod.dll>\n"; return 1; }
    DWORD pid = FindPid(argv[1]);
    if (!pid) { std::wcerr << L"process not found: " << argv[1] << L"\n"; return 2; }

    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(argv[2], MAX_PATH, full, nullptr)) { std::wcerr << L"bad dll path\n"; return 3; }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) { std::wcerr << L"dll not found: " << full << L"\n"; return 3; }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n"; return 4; }

    const SIZE_T bytes = (wcslen(full) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { std::wcerr << L"VirtualAllocEx failed\n"; CloseHandle(proc); return 5; }
    WriteProcessMemory(proc, remote, full, bytes, nullptr);

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) { std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n"; VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return 6; }

    WaitForSingleObject(th, INFINITE);
    std::wcout << L"injected " << full << L" into pid " << pid << L"\n";
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);
    return 0;
}

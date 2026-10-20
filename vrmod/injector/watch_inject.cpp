// hd2vr_watch — early injector for the real Helldivers 2 process.
// Usage: hd2vr_watch.exe <full path to game exe> <mod.dll> [command-line marker]
//
// Steam starts the game as a child of the DRM stub, and the stub exits. The stub is also named
// helldivers2.exe, so this watcher suspends each new helldivers2.exe at the game path as soon as
// it appears, reads its command line, and only injects into the process whose command line
// contains the marker (default "--bundle-dir", which the real game is started with). Anything else
// is resumed untouched. Injecting while suspended means the mod's hooks are in place before the
// game reads its data bundles.
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <string>
#include <iostream>
#include <set>

typedef NTSTATUS (NTAPI* NtSuspendProcessFn)(HANDLE);
typedef NTSTATUS (NTAPI* NtResumeProcessFn)(HANDLE);
typedef NTSTATUS (NTAPI* NtQueryInformationProcessFn)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

// Reads the process command line from its PEB (x64 offsets: PEB.ProcessParameters at 0x20,
// RTL_USER_PROCESS_PARAMETERS.CommandLine at 0x70).
static bool ReadCommandLine(HANDLE proc, NtQueryInformationProcessFn query, std::wstring& out) {
    PROCESS_BASIC_INFORMATION pbi{};
    if (query(proc, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr) != 0 || !pbi.PebBaseAddress) return false;
    ULONG64 params = 0;
    if (!ReadProcessMemory(proc, (BYTE*)pbi.PebBaseAddress + 0x20, &params, sizeof(params), nullptr) || !params) return false;
    UNICODE_STRING cl{};
    if (!ReadProcessMemory(proc, (BYTE*)params + 0x70, &cl, sizeof(cl), nullptr)) return false;
    if (cl.Length == 0 || cl.Length > 8192) return false;
    out.resize(cl.Length / sizeof(wchar_t));
    return ReadProcessMemory(proc, cl.Buffer, out.data(), cl.Length, nullptr) != 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { std::wcerr << L"usage: hd2vr_watch <game exe> <mod.dll> [marker]\n"; return 1; }

    wchar_t target[MAX_PATH], dll[MAX_PATH];
    if (!GetFullPathNameW(argv[1], MAX_PATH, target, nullptr)) return 2;
    if (!GetFullPathNameW(argv[2], MAX_PATH, dll, nullptr)) return 2;
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { std::wcerr << L"dll not found: " << dll << L"\n"; return 3; }
    const std::wstring marker = argc > 3 ? argv[3] : L"--bundle-dir";

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto NtSuspend = (NtSuspendProcessFn)GetProcAddress(ntdll, "NtSuspendProcess");
    auto NtResume  = (NtResumeProcessFn)GetProcAddress(ntdll, "NtResumeProcess");
    auto NtQuery   = (NtQueryInformationProcessFn)GetProcAddress(ntdll, "NtQueryInformationProcess");
    if (!NtSuspend || !NtResume || !NtQuery) return 4;

    std::wcout << L"watching for " << target << L" (marker " << marker << L")\n" << std::flush;
    std::set<DWORD> seen;
    const ULONGLONG deadline = GetTickCount64() + 600000;   // give up after 10 minutes
    while (GetTickCount64() < deadline) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 5;
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"helldivers2.exe") != 0 || seen.count(pe.th32ProcessID)) continue;
                seen.insert(pe.th32ProcessID);
                HANDLE proc = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION |
                                          PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                          PROCESS_CREATE_THREAD, FALSE, pe.th32ProcessID);
                if (!proc) continue;
                wchar_t image[MAX_PATH * 2];
                DWORD len = _countof(image);
                if (!QueryFullProcessImageNameW(proc, 0, image, &len) || _wcsicmp(image, target) != 0) {
                    CloseHandle(proc);
                    continue;
                }
                NtSuspend(proc);
                std::wstring cl;
                const bool isGame = ReadCommandLine(proc, NtQuery, cl) && cl.find(marker) != std::wstring::npos;
                if (!isGame) {
                    std::wcout << L"pid " << pe.th32ProcessID << L" is not the game (stub), resuming\n" << std::flush;
                    NtResume(proc);
                    CloseHandle(proc);
                    continue;
                }
                // Real game, still suspended: inject before it reads any data.
                const SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
                LPVOID remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                DWORD rc = 0;
                bool ok = false;
                if (remote && WriteProcessMemory(proc, remote, dll, bytes, nullptr)) {
                    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
                    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
                    if (th) {
                        WaitForSingleObject(th, 15000);
                        DWORD module = 0;
                        GetExitCodeThread(th, &module);
                        ok = module != 0;   // LoadLibrary returns the module handle
                        CloseHandle(th);
                    }
                }
                if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
                NtResume(proc);
                std::wcout << (ok ? L"injected into game pid " : L"INJECT FAILED for pid ") << pe.th32ProcessID << L"\n";
                CloseHandle(proc);
                CloseHandle(snap);
                return ok ? 0 : 6;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        Sleep(2);
    }
    std::wcerr << L"timed out waiting for the game process\n";
    return 7;
}

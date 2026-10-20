// File redirect for the HD2 VR mod. See file_redirect.h.
//
// Logs every open that touches a data folder to vrmod\redirect.log, and logs each redirect, so the
// timing and API the game uses are visible without guessing.
#include "file_redirect.h"
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <wchar.h>
#include <MinHook.h>

namespace hd2vr {

static const wchar_t kGameData[] = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Helldivers 2\\data\\";
static const wchar_t kModData[]  = L"D:\\HD2-Offline\\data\\";
static const wchar_t kLogPath[]  = L"C:\\HD2VR\\vrmod\\redirect.log";
static const int kMaxLogLines = 4000;

static LONG gLogLines = 0;
static thread_local bool tInLog = false;   // guards against the log's own CreateFileW re-entering us

using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using NtCreateFileFn = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                         PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
static CreateFileWFn  oCreateFileW   = nullptr;
static NtCreateFileFn oNtCreateFile  = nullptr;

static void Log(const char* tag, const wchar_t* path) {
    if (tInLog) return;
    if (InterlockedIncrement(&gLogLines) > kMaxLogLines) return;
    tInLog = true;
    char utf8[1024];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    char line[1200];
    sprintf_s(line, "%s %s\r\n", tag, utf8);
    HANDLE lf = CreateFileW(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (lf != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(lf, line, (DWORD)lstrlenA(line), &w, nullptr); CloseHandle(lf); }
    tInLog = false;
}

static bool IsDataPath(const wchar_t* p) {
    return wcsstr(p, L"\\data\\") != nullptr;
}

// If `in` names a file under the game's data folder and the same relative file exists in the
// modified copy, write that path to `out` (capacity 1024) and return true.
static bool MapPath(const wchar_t* in, wchar_t* out) {
    wchar_t full[1024];
    DWORD n = GetFullPathNameW(in, 1024, full, nullptr);
    if (n == 0 || n >= 1024) return false;
    size_t gl = wcslen(kGameData);
    if (_wcsnicmp(full, kGameData, gl) != 0) return false;
    wchar_t cand[1024];
    swprintf_s(cand, L"%ls%ls", kModData, full + gl);
    if (GetFileAttributesW(cand) == INVALID_FILE_ATTRIBUTES) return false;
    wcscpy_s(out, 1024, cand);
    return true;
}

static HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name && !tInLog && IsDataPath(name)) Log("cfw", name);
    wchar_t mapped[1024];
    if (name && !tInLog && MapPath(name, mapped)) {
        Log("cfw->D", mapped);
        return oCreateFileW(mapped, access, share, sa, disp, flags, tmpl);
    }
    return oCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static NTSTATUS NTAPI hkNtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa,
                                     PIO_STATUS_BLOCK iosb, PLARGE_INTEGER alloc, ULONG attrs, ULONG share,
                                     ULONG disp, ULONG opts, PVOID ea, ULONG ealen) {
    if (!tInLog && oa && !oa->RootDirectory && oa->ObjectName && oa->ObjectName->Buffer) {
        const wchar_t* nm = oa->ObjectName->Buffer;
        size_t nlen = oa->ObjectName->Length / sizeof(wchar_t);
        // Native paths look like \??\C:\...; strip the prefix, map it, and re-prefix.
        if (nlen > 4 && nm[0] == L'\\' && nm[1] == L'?' && nm[2] == L'?' && nm[3] == L'\\') {
            wchar_t raw[1024];
            size_t rl = nlen - 4;
            if (rl > 1023) rl = 1023;
            wmemcpy(raw, nm + 4, rl);
            raw[rl] = L'\0';
            if (IsDataPath(raw)) Log("nt", raw);
            wchar_t mapped[1024];
            if (MapPath(raw, mapped)) {
                wchar_t ntpath[1100];
                swprintf_s(ntpath, L"\\??\\%ls", mapped);
                UNICODE_STRING us;
                us.Length = (USHORT)(wcslen(ntpath) * sizeof(wchar_t));
                us.MaximumLength = us.Length + sizeof(wchar_t);
                us.Buffer = ntpath;
                OBJECT_ATTRIBUTES moa = *oa;
                moa.ObjectName = &us;
                Log("nt->D", mapped);
                return oNtCreateFile(handle, access, &moa, iosb, alloc, attrs, share, disp, opts, ea, ealen);
            }
        }
    }
    return oNtCreateFile(handle, access, oa, iosb, alloc, attrs, share, disp, opts, ea, ealen);
}

bool InstallFileRedirect() {
    MH_STATUS s0 = MH_Initialize();
    if (s0 != MH_OK && s0 != MH_ERROR_ALREADY_INITIALIZED) return false;

    FARPROC pCreate = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW");
    FARPROC pNt = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    if (!pCreate || !pNt) return false;

    MH_STATUS s1 = MH_CreateHook((void*)pCreate, (void*)hkCreateFileW, (void**)&oCreateFileW);
    MH_STATUS s2 = MH_CreateHook((void*)pNt, (void*)hkNtCreateFile, (void**)&oNtCreateFile);
    if (s1 != MH_OK || s2 != MH_OK) return false;
    if (MH_EnableHook((void*)pCreate) != MH_OK) return false;
    if (MH_EnableHook((void*)pNt) != MH_OK) return false;
    Log("install", L"file redirect hooks enabled (CreateFileW + NtCreateFile)");
    return true;
}

} // namespace hd2vr

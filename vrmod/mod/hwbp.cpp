// hd2vr_hwbp — in-process hardware watchpoints, driven by a command file.
//
// Write vrmod\hwbp_cmd.txt; the worker thread reads it, deletes it, arms the watchpoints on every
// thread of the game, waits, disarms, and logs to vrmod\hwbp.log every distinct instruction that
// touched the watched bytes, with how often and the unwound call stack.
//   w  <hexaddr> <len>     break on writes  (len 1, 2, 4 or 8; addr aligned to len); up to 4 lines
//   rw <hexaddr> <len>     break on reads or writes
//   go <seconds>           arm, wait, disarm, log (must be the last line)
// Data breakpoints trap after the access, so the logged RIP is the instruction after the one that
// touched the memory. Threads created while armed are not covered.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#pragma comment(lib, "psapi.lib")

static const wchar_t kCmdPath[] = L"C:\\HD2VR\\vrmod\\hwbp_cmd.txt";
static const wchar_t kLogPath[] = L"C:\\HD2VR\\vrmod\\hwbp.log";
static const int kFrames = 8;
static const int kMaxHits = 512;

struct Hit {
    DWORD64 rip;
    DWORD64 stack[kFrames];
    DWORD tid;
    int slot;
    volatile LONG count;
};
static Hit gHits[kMaxHits];
static volatile LONG gNumHits = 0;
static volatile LONG gLock = 0;
static volatile LONG gDropped = 0;
static volatile LONG gArmed = 0;

static void Log(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    HANDLE f = CreateFileW(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, line, (DWORD)strlen(line), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

// module!+offset for an address, e.g. "helldivers2.exe+1A2B3C"
static void Describe(DWORD64 a, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, MAX_PATH)) {
            const char* s = strrchr(full, '\\');
            strncpy_s(name, s ? s + 1 : full, _TRUNCATE);
        }
        snprintf(out, n, "%s+%llX", name, (unsigned long long)(a - (DWORD64)m));
    } else {
        snprintf(out, n, "%llX", (unsigned long long)a);
    }
}

static void Unwind(const CONTEXT* start, DWORD64* frames) {
    CONTEXT c = *start;
    for (int i = 0; i < kFrames; ++i) {
        frames[i] = c.Rip;
        if (!c.Rip) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (fn) {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
        } else {  // leaf function: return address is at RSP
            if (IsBadReadPtr((void*)c.Rsp, 8)) { frames[i + 1 < kFrames ? i + 1 : i] = 0; break; }
            c.Rip = *(DWORD64*)c.Rsp;
            c.Rsp += 8;
        }
    }
}

static LONG CALLBACK Handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    int slot = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3;
    c->Dr6 = 0;
    if (!gArmed) { c->Dr7 = 0; return EXCEPTION_CONTINUE_EXECUTION; }

    DWORD64 frames[kFrames] = {};
    Unwind(c, frames);
    while (InterlockedCompareExchange(&gLock, 1, 0)) YieldProcessor();
    int n = gNumHits, i = 0;
    for (; i < n; ++i)
        if (gHits[i].rip == c->Rip && gHits[i].slot == slot && gHits[i].stack[1] == frames[1]) break;
    if (i < n) {
        gHits[i].count++;
    } else if (n < kMaxHits) {
        Hit& h = gHits[n];
        h.rip = c->Rip;
        memcpy(h.stack, frames, sizeof(frames));
        h.tid = GetCurrentThreadId();
        h.slot = slot;
        h.count = 1;
        gNumHits = n + 1;
    } else {
        gDropped++;
    }
    gLock = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct Watch { DWORD64 addr; int len; bool rw; };

static DWORD64 Dr7For(const Watch* w, int count) {
    DWORD64 dr7 = 0;
    for (int i = 0; i < count; ++i) {
        DWORD64 lenBits = w[i].len == 1 ? 0 : w[i].len == 2 ? 1 : w[i].len == 8 ? 2 : 3;
        DWORD64 rwBits = w[i].rw ? 3 : 1;
        dr7 |= 1ull << (2 * i);
        dr7 |= rwBits << (16 + 4 * i);
        dr7 |= lenBits << (18 + 4 * i);
    }
    return dr7;
}

// Sets the debug registers on every other thread of this process; count 0 disarms. Returns threads set.
static int SetAllThreads(const Watch* w, int count) {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = { sizeof(te) };
    int done = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = count > 0 ? w[0].addr : 0;
                c.Dr1 = count > 1 ? w[1].addr : 0;
                c.Dr2 = count > 2 ? w[2].addr : 0;
                c.Dr3 = count > 3 ? w[3].addr : 0;
                c.Dr6 = 0;
                c.Dr7 = Dr7For(w, count);
                if (SetThreadContext(t, &c)) ++done;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return done;
}

static char* ReadCmd() {
    HANDLE f = CreateFileW(kCmdPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    char* buf = (char*)calloc(4097, 1);
    DWORD got = 0;
    ReadFile(f, buf, 4096, &got, nullptr);
    CloseHandle(f);
    DeleteFileW(kCmdPath);
    return buf;
}

static void Run(char* cmd) {
    Watch w[4];
    int count = 0;
    double seconds = 0;
    char* save = nullptr;
    for (char* line = strtok_s(cmd, "\r\n", &save); line; line = strtok_s(nullptr, "\r\n", &save)) {
        char op[8] = {};
        unsigned long long addr = 0;
        int len = 0;
        if (sscanf_s(line, "%7s", op, (unsigned)sizeof(op)) != 1) continue;
        if (!strcmp(op, "go")) {
            sscanf_s(line, "%*s %lf", &seconds);
        } else if ((!strcmp(op, "w") || !strcmp(op, "rw")) && count < 4 &&
                   sscanf_s(line, "%*s %llx %d", &addr, &len) == 2) {
            if (len != 1 && len != 2 && len != 4 && len != 8) len = 4;
            addr &= ~(unsigned long long)(len - 1);
            w[count++] = { addr, len, op[0] == 'r' };
        }
    }
    if (!count || seconds <= 0) { Log("hwbp: need 1-4 'w'/'rw' lines and 'go <seconds>'"); return; }
    if (seconds > 30) seconds = 30;

    gNumHits = 0;
    gDropped = 0;
    gArmed = 1;
    int threads = SetAllThreads(w, count);
    for (int i = 0; i < count; ++i)
        Log("hwbp: slot %d %s %llX len %d", i, w[i].rw ? "rw" : "w", (unsigned long long)w[i].addr, w[i].len);
    Log("hwbp: armed on %d threads for %.1f s", threads, seconds);
    Sleep((DWORD)(seconds * 1000));
    gArmed = 0;
    SetAllThreads(w, 0);

    int n = gNumHits;
    Log("hwbp: %d distinct hits (%ld dropped)", n, gDropped);
    for (int i = 0; i < n; ++i) {
        const Hit& h = gHits[i];
        char rip[128], line[1600];
        Describe(h.rip, rip, sizeof(rip));
        int at = snprintf(line, sizeof(line), "  slot %d  x%-7ld tid %-6lu after %s  <-", h.slot, h.count, h.tid, rip);
        for (int f = 1; f < kFrames && h.stack[f] && at < (int)sizeof(line) - 130; ++f) {
            char fr[128];
            Describe(h.stack[f], fr, sizeof(fr));
            at += snprintf(line + at, sizeof(line) - at, " %s", fr);
        }
        Log("%s", line);
    }
    Log("hwbp: done");
}

static DWORD WINAPI Worker(LPVOID) {
    AddVectoredExceptionHandler(1, Handler);
    Log("hwbp: ready; write vrmod\\hwbp_cmd.txt");
    for (;;) {
        if (char* cmd = ReadCmd()) { Run(cmd); free(cmd); }
        Sleep(250);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    }
    return TRUE;
}

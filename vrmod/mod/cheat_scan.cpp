// hd2vr_cheat — in-process memory search/patch tool, driven by a command file.
//
// Write one command line to vrmod\cheat2_cmd.txt. The worker thread reads it, deletes the file,
// runs it, and logs the result to vrmod\cheat2.log. Scans cover resident writable pages only.
//   scan <lo> <hi>       collect every 4-byte-aligned float in [lo, hi] in writable memory
//   dec [tol]            keep candidates that dropped by the elapsed seconds (+-tol, default 0.35)
//   inc [tol]            keep candidates that rose by the elapsed seconds
//   same                 keep candidates whose value did not change
//   list [n]             log up to n candidates (default 20) with current values
//   set <value>          write <value> to every candidate (refused above 64 candidates)
//   freeze <value>       keep writing <value> to the candidates every 50 ms; "unfreeze" stops it
//   dump <hexaddr> [n]   hex dump n bytes (default 128) at an address
// Reads in the scan and filters are SEH-guarded, so unreadable pages are skipped, not fatal.
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#pragma comment(lib, "psapi.lib")

static const wchar_t kCmdPath[] = L"C:\\HD2VR\\vrmod\\cheat2_cmd.txt";
static const wchar_t kLogPath[] = L"C:\\HD2VR\\vrmod\\cheat2.log";
static const size_t kMaxCandidates = 20 * 1000 * 1000;
static const size_t kMaxWrite = 64;

static uintptr_t* gAddr = nullptr;   // candidate addresses
static float* gVal = nullptr;        // value at last read
static float* gTime = nullptr;       // seconds (since load) of last read
static size_t gCount = 0;
static bool gOverflow = false;
static void* gSelfBase = nullptr;
static LARGE_INTEGER gFreq, gStart;
static volatile LONG gFreezeOn = 0;
static volatile float gFreezeValue = 0.0f;

static float Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (float)((double)(t.QuadPart - gStart.QuadPart) / (double)gFreq.QuadPart);
}

static void Log(const char* fmt, ...) {
    char line[1024];
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

static bool Writable(DWORD p) {
    if (p & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Appends matching floats in [base, base+size) to the candidate arrays. Scalar locals only, so
// __try is allowed; a fault ends this region early.
static void ScanRegion(const unsigned char* base, SIZE_T size, float lo, float hi, float t) {
    __try {
        const float* p = (const float*)base;
        const SIZE_T n = size / 4;
        for (SIZE_T i = 0; i < n; ++i) {
            const float v = p[i];
            if (v >= lo && v <= hi) {
                if (gCount >= kMaxCandidates) { gOverflow = true; return; }
                gAddr[gCount] = (uintptr_t)(p + i);
                gVal[gCount] = v;
                gTime[gCount] = t;
                ++gCount;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool ReadFloat(uintptr_t a, float* out) {
    __try { *out = *(const float*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteFloat(uintptr_t a, float v) {
    __try { *(float*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The candidate arrays get their own VirtualAlloc blocks so the scan can skip them; otherwise it
// would find the floats it just recorded and keep appending to itself.
static bool IsOwnMemory(void* allocationBase) {
    return allocationBase == gSelfBase || allocationBase == gAddr || allocationBase == gVal || allocationBase == gTime;
}

static void CmdScan(float lo, float hi) {
    if (!gAddr) {
        gAddr = (uintptr_t*)VirtualAlloc(nullptr, kMaxCandidates * sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        gVal = (float*)VirtualAlloc(nullptr, kMaxCandidates * sizeof(float), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        gTime = (float*)VirtualAlloc(nullptr, kMaxCandidates * sizeof(float), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!gAddr || !gVal || !gTime) { Log("scan: out of memory"); return; }
    }
    gCount = 0;
    gOverflow = false;
    const float t0 = Now();
    unsigned long long bytes = 0;
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < 0x7FFFFFFEFFFFull && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && Writable(mbi.Protect) && !IsOwnMemory(mbi.AllocationBase)) {
            // Only scan pages resident in the working set. Touching committed-but-untouched pages
            // makes Windows materialize them, which is what made full scans take many minutes;
            // live game state (timers, counters) is resident anyway.
            static PSAPI_WORKING_SET_EX_INFORMATION ws[1024];
            const unsigned char* base = (const unsigned char*)mbi.BaseAddress;
            const SIZE_T pages = mbi.RegionSize / 4096;
            for (SIZE_T first = 0; first < pages && !gOverflow; first += 1024) {
                const SIZE_T batch = (pages - first < 1024) ? pages - first : 1024;
                for (SIZE_T i = 0; i < batch; ++i) ws[i].VirtualAddress = (PVOID)(base + (first + i) * 4096);
                if (!QueryWorkingSetEx(GetCurrentProcess(), ws, (DWORD)(batch * sizeof(ws[0])))) continue;
                for (SIZE_T i = 0; i < batch && !gOverflow; ++i) {
                    if (!ws[i].VirtualAttributes.Valid) continue;
                    ScanRegion(base + (first + i) * 4096, 4096, lo, hi, Now());
                    bytes += 4096;
                }
            }
            if (gOverflow) break;
        }
        const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    Log("scan [%g, %g]: %zu candidates%s in %llu MB, %.1f s", lo, hi, gCount,
        gOverflow ? " (CAPPED: narrow the range)" : "", bytes >> 20, Now() - t0);
}

// mode: -1 = dropped by elapsed time, +1 = rose by elapsed time, 0 = unchanged.
static void CmdFilter(int mode, float tol) {
    const float now = Now();
    size_t kept = 0;
    for (size_t i = 0; i < gCount; ++i) {
        float v;
        if (!ReadFloat(gAddr[i], &v)) continue;
        const float dt = now - gTime[i];
        const float expect = mode < 0 ? gVal[i] - dt : (mode > 0 ? gVal[i] + dt : gVal[i]);
        const float limit = mode == 0 ? 0.0001f : tol;
        if (v - expect <= limit && expect - v <= limit) {
            gAddr[kept] = gAddr[i];
            gVal[kept] = v;
            gTime[kept] = now;
            ++kept;
        }
    }
    Log("%s: %zu -> %zu candidates", mode < 0 ? "dec" : (mode > 0 ? "inc" : "same"), gCount, kept);
    gCount = kept;
}

static void CmdList(size_t n) {
    for (size_t i = 0; i < gCount && i < n; ++i) {
        float v = 0;
        ReadFloat(gAddr[i], &v);
        Log("  [%zu] %p = %g", i, (void*)gAddr[i], v);
    }
    if (gCount > n) Log("  ... %zu more", gCount - n);
}

static void CmdSet(float v) {
    if (gCount > kMaxWrite) { Log("set: refused, %zu candidates (max %zu)", gCount, kMaxWrite); return; }
    size_t ok = 0;
    for (size_t i = 0; i < gCount; ++i) ok += WriteFloat(gAddr[i], v) ? 1 : 0;
    Log("set %g: wrote %zu of %zu", v, ok, gCount);
}

static bool ReadBytes(uintptr_t a, unsigned char* out, size_t n) {
    __try { memcpy(out, (const void*)a, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void CmdDump(uintptr_t a, size_t n) {
    if (n > 1024) n = 1024;
    unsigned char buf[1024];
    if (!ReadBytes(a, buf, n)) { Log("dump %p: unreadable", (void*)a); return; }
    for (size_t row = 0; row < n; row += 16) {
        char line[160];
        int len = sprintf_s(line, "  %p:", (void*)(a + row));
        for (size_t b = 0; b < 16 && row + b < n; ++b) len += sprintf_s(line + len, sizeof(line) - len, " %02x", buf[row + b]);
        len += sprintf_s(line + len, sizeof(line) - len, "  |");
        for (size_t b = 0; b < 16 && row + b < n; b += 4) {
            float f;
            memcpy(&f, buf + row + b, 4);
            len += sprintf_s(line + len, sizeof(line) - len, " %g", f);
        }
        Log("%s", line);
    }
}

static void Run(char* cmd) {
    char op[32] = {};
    double a = 0, b = 0;
    unsigned long long hex = 0;
    const int n = sscanf_s(cmd, "%31s %lf %lf", op, (unsigned)sizeof(op), &a, &b);
    Log("> %s", cmd);
    if (!strcmp(op, "scan") && n >= 3) CmdScan((float)a, (float)b);
    else if (!strcmp(op, "dec")) CmdFilter(-1, n >= 2 ? (float)a : 0.35f);
    else if (!strcmp(op, "inc")) CmdFilter(+1, n >= 2 ? (float)a : 0.35f);
    else if (!strcmp(op, "same")) CmdFilter(0, 0);
    else if (!strcmp(op, "list")) CmdList(n >= 2 ? (size_t)a : 20);
    else if (!strcmp(op, "set") && n >= 2) CmdSet((float)a);
    else if (!strcmp(op, "freeze") && n >= 2) {
        if (gCount > kMaxWrite) Log("freeze: refused, %zu candidates", gCount);
        else { gFreezeValue = (float)a; InterlockedExchange(&gFreezeOn, 1); Log("freeze %g on %zu", a, gCount); }
    }
    else if (!strcmp(op, "unfreeze")) { InterlockedExchange(&gFreezeOn, 0); Log("unfreeze"); }
    else if (!strcmp(op, "dump") && sscanf_s(cmd, "%*s %llx %lf", &hex, &a) >= 1) CmdDump((uintptr_t)hex, a > 0 ? (size_t)a : 128);
    else Log("unknown command");
}

static char* TakeCommand() {
    HANDLE f = CreateFileW(kCmdPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    char* buf = (char*)calloc(1, 512);
    DWORD got = 0;
    if (buf) ReadFile(f, buf, 511, &got, nullptr);
    CloseHandle(f);
    DeleteFileW(kCmdPath);
    if (buf) for (DWORD i = 0; i < got; ++i) if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = 0; break; }
    return buf;
}

static DWORD WINAPI Worker(LPVOID) {
    Log("cheat: loaded; write commands to vrmod\\cheat_cmd.txt");
    for (;;) {
        if (char* cmd = TakeCommand()) { Run(cmd); free(cmd); }
        if (gFreezeOn) {
            const float v = gFreezeValue;
            for (size_t i = 0; i < gCount && i < kMaxWrite; ++i) WriteFloat(gAddr[i], v);
            Sleep(50);
        } else {
            Sleep(150);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&gFreq);
        QueryPerformanceCounter(&gStart);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(&gCount, &mbi, sizeof(mbi))) gSelfBase = mbi.AllocationBase;
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}

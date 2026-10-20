// hd2vr_rename — in-process label rename.
// Press F6 in game: scans the game's writable memory for each name in kRenames (ASCII and
// UTF-16LE, case-insensitive) and overwrites every hit with its replacement. The game keeps its
// labels as NUL-terminated strings (e.g. "RICTUS SECTOR"), so a shorter name is written and the
// rest of the string is moved up to close the gap, leaving NULs at the end; the string never grows
// past its buffer. Without a nearby terminator the replacement is space-padded instead. Hits,
// per-name counts and timing go to vrmod\rename.log. No hooks are installed, so this can be
// injected alongside the other mod DLLs.
#include <windows.h>
#include <cstdio>
#include <cstring>

struct Rename { const char* from; const char* to; };
// A replacement must not be longer than the name it replaces; shorter ones are space-padded.
static const Rename kRenames[] = {
    { "RICTUS",        "NOVA" },
    { "GENESIS PRIME", "NOVA PRIME" },
    { "GRAFMERE",      "NOVA 426" },
    { "VALMOX",        "NOVA 7" },
    // Cleanup for strings an earlier build already space-padded.
    { "NOVA  ",        "NOVA" },
    { "NOVA PRIME   ", "NOVA PRIME" },
    // Description of enemy-held planets. It is one shared string, so every such planet changes.
    // The full text comes first; the per-sentence entries only match if the game stores the two
    // sentences separately.
    { "This planet is considered enemy territory. The Helldivers must reclaim it from its oppressive occupants.",
      "This planet has few Helldivers on it. A perfect place to work on mods!" },
    { "This planet is considered enemy territory.", "This planet has few Helldivers on it." },
    { "The Helldivers must reclaim it from its oppressive occupants.", "A perfect place to work on mods!" },
};
static const int kCount = sizeof(kRenames) / sizeof(kRenames[0]);
static const int kHotkey = VK_F6;
static const size_t kMaxTail = 512;   // how far past a hit to look for the string's terminator
static const char kLogPath[] = "C:\\HD2VR\\vrmod\\rename.log";
static const SIZE_T kBlock = 1024 * 1024;
static const long kMaxHitLines = 300;

struct Needle {
    unsigned char from[256];
    unsigned char to[256];  // replacement, space-padded to len
    size_t len;
    size_t toLen;           // replacement length without padding
    long ascii;
    long wide;
};
static Needle gNeedles[kCount];
static unsigned char gLower[256];
static bool gFirst[256];            // lower-cased first bytes of all needles
static size_t gMaxLen = 0;
static long gHitLines = 0;
static void* gSelfBase = nullptr;   // allocation base of this DLL, skipped so the table isn't patched

static void Log(const char* s) {
    HANDLE f = CreateFileA(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, s, (DWORD)strlen(s), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

static bool Init() {
    for (int c = 0; c < 256; ++c) gLower[c] = (unsigned char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    for (int r = 0; r < kCount; ++r) {
        const size_t fromLen = strlen(kRenames[r].from), toLen = strlen(kRenames[r].to);
        if (fromLen >= sizeof(gNeedles[r].from) || toLen > fromLen) {
            char line[200];
            sprintf_s(line, "rename: SKIP %.40s -> %.40s (replacement longer than original)", kRenames[r].from, kRenames[r].to);
            Log(line);
            continue;
        }
        Needle& nd = gNeedles[r];
        memcpy(nd.from, kRenames[r].from, fromLen);
        memset(nd.to, ' ', fromLen);
        memcpy(nd.to, kRenames[r].to, toLen);
        nd.len = fromLen;
        nd.toLen = toLen;
        gFirst[gLower[nd.from[0]]] = true;
        if (fromLen > gMaxLen) gMaxLen = fromLen;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(&gNeedles, &mbi, sizeof(mbi))) gSelfBase = mbi.AllocationBase;
    return gMaxLen > 0;
}

static void LogHit(const Needle& nd, bool wide, const unsigned char* at, DWORD protect) {
    if (++gHitLines > kMaxHitLines) return;
    char line[200];
    const int shown = nd.len > 40 ? 40 : (int)nd.len;   // keep long needles inside the line buffer
    sprintf_s(line, "%s hit %.*s @ %p protect=0x%lx", wide ? "UTF16" : "ASCII", shown, (const char*)nd.from, (const void*)at, protect);
    Log(line);
}

static bool Matches(const unsigned char* p, const Needle& nd, size_t stride) {
    for (size_t k = 0; k < nd.len; ++k) {
        if (gLower[p[k * stride]] != gLower[nd.from[k]]) return false;
        if (stride == 2 && p[k * 2 + 1] != 0) return false;
    }
    return true;
}

// Writes the replacement over a hit at p (stride 1 = ASCII, 2 = UTF-16LE). If the string's
// terminator is found before `end`, the rest of the string moves up behind the shorter name and the
// freed units become NULs, so "RICTUS SECTOR" becomes "NOVA SECTOR". Otherwise space-pad.
static void Apply(unsigned char* p, const unsigned char* end, const Needle& nd, size_t stride) {
    const size_t n = nd.len, t = nd.toLen;
    unsigned char* tail = p + n * stride;
    unsigned char* term = nullptr;
    for (unsigned char* q = tail; q + stride <= end && q < tail + kMaxTail * stride; q += stride) {
        if (q[0] == 0 && (stride == 1 || q[1] == 0)) { term = q; break; }
    }
    if (term && t < n) {
        for (size_t k = 0; k < t; ++k) {
            p[k * stride] = nd.to[k];
            if (stride == 2) p[k * 2 + 1] = 0;
        }
        const size_t tailBytes = (size_t)(term - tail);
        memmove(p + t * stride, tail, tailBytes);
        memset(p + t * stride + tailBytes, 0, (n - t) * stride);
    } else {
        for (size_t k = 0; k < n; ++k) {
            p[k * stride] = nd.to[k];
            if (stride == 2) p[k * 2 + 1] = 0;
        }
    }
}

// Scans and patches one block. SEH guards against pages that fault despite their protection.
static int ScanBlock(unsigned char* base, SIZE_T size, long* faults, DWORD protect) {
    __try {
        for (SIZE_T i = 0; i + 2 <= size; ++i) {
            const unsigned char c = gLower[base[i]];
            if (!gFirst[c]) continue;
            for (int r = 0; r < kCount; ++r) {
                Needle& nd = gNeedles[r];
                if (nd.len == 0 || gLower[nd.from[0]] != c) continue;
                if (i + nd.len <= size && Matches(base + i, nd, 1)) {
                    Apply(base + i, base + size, nd, 1);
                    ++nd.ascii;
                    LogHit(nd, false, base + i, protect);
                    i += nd.len - 1;
                    break;
                }
                if (i + 2 * nd.len <= size && Matches(base + i, nd, 2)) {
                    Apply(base + i, base + size, nd, 2);
                    ++nd.wide;
                    LogHit(nd, true, base + i, protect);
                    i += 2 * nd.len - 1;
                    break;
                }
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++*faults;
        return 1;
    }
}

static bool Writable(DWORD p) {
    if (p & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void RunScan() {
    const ULONGLONG start = GetTickCount64();
    long faults = 0;
    unsigned long long regions = 0, bytes = 0;
    gHitLines = 0;
    for (int r = 0; r < kCount; ++r) gNeedles[r].ascii = gNeedles[r].wide = 0;
    Log("scan: start");
    uintptr_t addr = 0;
    const uintptr_t maxAddr = 0x7FFFFFFEFFFFull;
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < maxAddr && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && Writable(mbi.Protect) && mbi.AllocationBase != gSelfBase) {
            ++regions;
            unsigned char* base = (unsigned char*)mbi.BaseAddress;
            for (SIZE_T off = 0; off < mbi.RegionSize; off += kBlock) {
                SIZE_T len = min(kBlock + 2 * gMaxLen, mbi.RegionSize - off);
                ScanBlock(base + off, len, &faults, mbi.Protect);
                bytes += len;
            }
        }
        const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    char line[200];
    for (int r = 0; r < kCount; ++r) {
        if (!gNeedles[r].len) continue;
        sprintf_s(line, "scan: %-14.40s ascii=%ld utf16=%ld", kRenames[r].from, gNeedles[r].ascii, gNeedles[r].wide);
        Log(line);
    }
    sprintf_s(line, "scan: done regions=%llu MB=%llu faults=%ld ms=%llu", regions, bytes >> 20, faults, GetTickCount64() - start);
    Log(line);
}

static DWORD WINAPI Worker(LPVOID) {
    if (!Init()) { Log("rename: no usable renames"); return 0; }
    Log("rename: loaded, F6 = apply renames in live memory");
    for (;;) {
        if (GetAsyncKeyState(kHotkey) & 1) RunScan();
        Sleep(50);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}

// hd2vr_scan — read-only memory scanner for a running process.
// Usage: hd2vr_scan.exe <pid> <needle> [output log]
// Finds every ASCII (case-insensitive) and UTF-16LE (case-insensitive) occurrence of <needle> in
// the process's committed, readable memory, and logs the address plus surrounding text.
// With "hash:<16 hex digits>", it instead searches for that 64-bit value stored little-endian (how
// the game keeps resource hashes) and logs a hex dump around each hit.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool IsReadable(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (protect & ok) != 0;
}

static char Printable(unsigned char c) { return (c >= 32 && c < 127) ? (char)c : '.'; }

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { fwprintf(stderr, L"usage: hd2vr_scan <pid> <needle> [log]\n"); return 1; }
    const DWORD pid = (DWORD)_wtoi(argv[1]);
    std::string needle;
    for (wchar_t* p = argv[2]; *p; ++p) needle.push_back((char)*p);
    const std::wstring logPath = argc > 3 ? argv[3] : L"C:\\HD2VR\\vrmod\\scan.log";

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError()); return 2; }
    FILE* out = _wfopen(logPath.c_str(), L"w");
    if (!out) { CloseHandle(proc); return 3; }

    const bool hashMode = needle.rfind("hash:", 0) == 0;
    unsigned char hashBytes[8] = {};
    if (hashMode) {
        const unsigned long long v = _strtoui64(needle.c_str() + 5, nullptr, 16);
        memcpy(hashBytes, &v, 8);   // x64 is little-endian, matching how the game stores hashes
    }
    // Text mode accepts several needles separated by '|'; the log tags each hit with its index.
    std::vector<std::string> needles;
    if (!hashMode) {
        size_t s = 0;
        for (size_t e; (e = needle.find('|', s)) != std::string::npos; s = e + 1) needles.push_back(needle.substr(s, e - s));
        needles.push_back(needle.substr(s));
    }
    size_t longest = 0;
    for (const auto& nd : needles) if (nd.size() > longest) longest = nd.size();
    const size_t n = hashMode ? 8 : longest;
    const SIZE_T kChunk = 4 * 1024 * 1024;
    int hits = 0;
    long regions = 0, readable = 0, readFails = 0;
    unsigned long long bytesRead = 0;
    LPBYTE addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQueryEx(proc, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        LPBYTE next = (LPBYTE)mbi.BaseAddress + mbi.RegionSize;
        ++regions;
        if (mbi.State == MEM_COMMIT && IsReadable(mbi.Protect)) {
            ++readable;
            for (SIZE_T off = 0; off < mbi.RegionSize; off += kChunk) {
                SIZE_T want = min(kChunk + 2 * n, mbi.RegionSize - off);
                std::vector<unsigned char> buf(want, 0);
                SIZE_T got = 0;
                if (!ReadProcessMemory(proc, (LPBYTE)mbi.BaseAddress + off, buf.data(), want, &got)) {
                    // One unreadable page fails the whole chunk: fall back to page-sized reads so the
                    // readable pages still get scanned. Unreadable pages stay zero.
                    got = 0;
                    for (SIZE_T p = 0; p < want; p += 4096) {
                        SIZE_T pageLen = min((SIZE_T)4096, want - p), pageGot = 0;
                        if (ReadProcessMemory(proc, (LPBYTE)mbi.BaseAddress + off + p, buf.data() + p, pageLen, &pageGot)) {
                            got = p + pageGot;
                        } else {
                            ++readFails;
                        }
                    }
                    if (got < 2 * n) continue;
                }
                bytesRead += got;
                if (hashMode) {
                    for (SIZE_T i = 0; i + 8 <= got; ++i) {
                        if (buf[i] != hashBytes[0] || memcmp(&buf[i], hashBytes, 8) != 0) continue;
                        LPBYTE at = (LPBYTE)mbi.BaseAddress + off + i;
                        fprintf(out, "HASH @ 0x%p  aligned8=%d  protect=0x%lx\n", at, (int)(((uintptr_t)at & 7) == 0), mbi.Protect);
                        for (long long row = -4; row < 8; ++row) {   // 64 bytes before, 128 from the hit
                            const long long start = (long long)i + row * 16;
                            if (start < 0 || start + 16 > (long long)got) continue;
                            fprintf(out, "  %+5lld:", row * 16);
                            for (int b = 0; b < 16; ++b) fprintf(out, " %02x", buf[(SIZE_T)start + b]);
                            fprintf(out, "  ");
                            for (int b = 0; b < 16; ++b) fputc(Printable(buf[(SIZE_T)start + b]), out);
                            fputc('\n', out);
                        }
                        if (++hits >= 300) goto done;
                    }
                    continue;
                }
                for (SIZE_T i = 0; i + 2 * n <= got; ++i) {
                    for (size_t w = 0; w < needles.size(); ++w) {
                        const std::string& nd = needles[w];
                        if (i + 2 * nd.size() > got) continue;
                        bool ascii = true, wide = true;
                        for (size_t k = 0; k < nd.size() && (ascii || wide); ++k) {
                            unsigned char c = buf[i + k];
                            if (ascii && tolower(c) != tolower((unsigned char)nd[k])) ascii = false;
                            if (wide) {
                                unsigned char wc = buf[i + 2 * k];
                                if (buf[i + 2 * k + 1] != 0 || tolower(wc) != tolower((unsigned char)nd[k])) wide = false;
                            }
                        }
                        if (!ascii && !wide) continue;
                        const SIZE_T stride = wide ? 2 : 1;
                        std::string ctx;
                        const SIZE_T from = i >= 24 ? i - 24 : 0;
                        for (SIZE_T j = from; j < got && j < i + 80 * stride; j += stride) ctx.push_back(Printable(buf[j]));
                        fprintf(out, "%s[%zu] @ 0x%p  %s\n", wide ? "UTF16" : "ASCII", w, (LPBYTE)mbi.BaseAddress + off + i, ctx.c_str());
                        if (++hits >= 2000) goto done;
                        break;
                    }
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
done:
    fprintf(out, "regions=%ld readable=%ld readFails=%ld bytesRead=%llu\n", regions, readable, readFails, bytesRead);
    fprintf(out, "total hits: %d\n", hits);
    fclose(out);
    CloseHandle(proc);
    wprintf(L"scanned pid %lu for '%hs': %d hits -> %s\n", pid, needle.c_str(), hits, logPath.c_str());
    return 0;
}

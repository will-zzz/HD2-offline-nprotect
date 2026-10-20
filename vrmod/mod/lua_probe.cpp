// hd2vr_lua — probe for the game's LuaJIT runtime (lua51.dll, LuaJIT 2.1).
//
//  - Hooks luaL_loadbufferx: logs the name and size of every chunk the game loads.
//  - Hooks lua_pcall: records each lua_State seen, and is the safe point (on the game's own thread,
//    inside a call the game made) where queued scripts run.
//  - Script runner: drop a Lua file at vrmod\lua_run.lua. The worker thread reads and deletes it,
//    and the next lua_pcall runs it inside that state. Scripts print with hd2vr_log(string).
//
// Everything goes to vrmod\lua.log. Scripts run through lua_pcall, so a Lua error is logged and
// never propagates into the game.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <MinHook.h>

struct lua_State;
typedef int (*lua_CFunction)(lua_State*);
static const int LUA_GLOBALSINDEX = -10002;

using pcall_t        = int (*)(lua_State*, int, int, int);
using loadbufferx_t  = int (*)(lua_State*, const char*, size_t, const char*, const char*);
using loadstring_t   = int (*)(lua_State*, const char*);
using gettop_t       = int (*)(lua_State*);
using settop_t       = void (*)(lua_State*, int);
using tolstring_t    = const char* (*)(lua_State*, int, size_t*);
using pushcclosure_t = void (*)(lua_State*, lua_CFunction, int);
using setfield_t     = void (*)(lua_State*, int, const char*);

static pcall_t        oPcall       = nullptr;
static loadbufferx_t  oLoadBufferX = nullptr;
static loadstring_t   pLoadstring  = nullptr;
static gettop_t       pGettop      = nullptr;
static settop_t       pSettop      = nullptr;
static tolstring_t    pTolstring   = nullptr;
static pushcclosure_t pPushcclosure = nullptr;
static setfield_t     pSetfield    = nullptr;

static const wchar_t kLogPath[]    = L"C:\\HD2VR\\vrmod\\lua.log";
static const wchar_t kScriptPath[] = L"C:\\HD2VR\\vrmod\\lua_run.lua";
static const long kMaxLoadLines = 5000;

static CRITICAL_SECTION gLogLock;
static volatile LONG gPcalls = 0;
static volatile LONG gLoadLines = 0;
static char* volatile gPendingScript = nullptr;   // heap buffer owned by whoever takes it
static lua_State* gStates[16] = {};
static volatile LONG gStateCount = 0;
static thread_local bool tInScript = false;

static void Log(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&gLogLock);
    HANDLE f = CreateFileW(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, line, (DWORD)strlen(line), &w, nullptr);
        WriteFile(f, "\r\n", 2, &w, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&gLogLock);
}

// hd2vr_log(s): lets scripts write to lua.log.
static int LuaLog(lua_State* L) {
    const char* s = pTolstring(L, 1, nullptr);
    Log("%s", s ? s : "(nil)");
    return 0;
}

// Runs a script inside L, leaving L's stack exactly as it was.
static void RunScript(lua_State* L, const char* code) {
    const int top = pGettop(L);
    pPushcclosure(L, LuaLog, 0);
    pSetfield(L, LUA_GLOBALSINDEX, "hd2vr_log");
    Log("== run script in state %p ==", (void*)L);
    if (pLoadstring(L, code) != 0 || oPcall(L, 0, 0, 0) != 0) {
        const char* err = pTolstring(L, -1, nullptr);
        Log("lua error: %s", err ? err : "(no message)");
    }
    Log("== script done ==");
    pSettop(L, top);
}

static void NoteState(lua_State* L) {
    const LONG n = gStateCount;
    for (LONG i = 0; i < n && i < 16; ++i) if (gStates[i] == L) return;
    const LONG slot = InterlockedIncrement(&gStateCount) - 1;
    if (slot < 16) {
        gStates[slot] = L;
        Log("state #%ld: %p (thread %lu)", slot, (void*)L, GetCurrentThreadId());
    }
}

static int hkPcall(lua_State* L, int nargs, int nresults, int errfunc) {
    InterlockedIncrement(&gPcalls);
    NoteState(L);
    if (!tInScript && gPendingScript) {
        char* code = (char*)InterlockedExchangePointer((PVOID volatile*)&gPendingScript, nullptr);
        if (code) {
            tInScript = true;
            RunScript(L, code);
            tInScript = false;
            free(code);
        }
    }
    return oPcall(L, nargs, nresults, errfunc);
}

static int hkLoadBufferX(lua_State* L, const char* buff, size_t size, const char* name, const char* mode) {
    if (!tInScript && InterlockedIncrement(&gLoadLines) <= kMaxLoadLines)
        Log("load %s (%zu bytes) state=%p", name ? name : "(null)", size, (void*)L);
    return oLoadBufferX(L, buff, size, name, mode);
}

// Reads vrmod\lua_run.lua (if present) into a heap buffer and deletes the file.
static char* TakeScriptFile() {
    HANDLE f = CreateFileW(kScriptPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    char* buf = nullptr;
    if (size != INVALID_FILE_SIZE && size < 4 * 1024 * 1024) {
        buf = (char*)malloc(size + 1);
        if (buf && ReadFile(f, buf, size, &got, nullptr)) buf[got] = '\0';
        else { free(buf); buf = nullptr; }
    }
    CloseHandle(f);
    DeleteFileW(kScriptPath);
    return buf;
}

static DWORD WINAPI Worker(LPVOID) {
    // When injected early (hd2vr_watch), the game hasn't loaded lua51.dll yet: wait for it.
    HMODULE lua = nullptr;
    for (int i = 0; i < 12000 && !(lua = GetModuleHandleW(L"lua51.dll")); ++i) Sleep(10);
    if (!lua) { Log("lua51.dll never loaded"); return 0; }
    void* pPcall = (void*)GetProcAddress(lua, "lua_pcall");
    void* pLoadBufX = (void*)GetProcAddress(lua, "luaL_loadbufferx");
    pLoadstring   = (loadstring_t)GetProcAddress(lua, "luaL_loadstring");
    pGettop       = (gettop_t)GetProcAddress(lua, "lua_gettop");
    pSettop       = (settop_t)GetProcAddress(lua, "lua_settop");
    pTolstring    = (tolstring_t)GetProcAddress(lua, "lua_tolstring");
    pPushcclosure = (pushcclosure_t)GetProcAddress(lua, "lua_pushcclosure");
    pSetfield     = (setfield_t)GetProcAddress(lua, "lua_setfield");
    if (!pPcall || !pLoadBufX || !pLoadstring || !pGettop || !pSettop || !pTolstring || !pPushcclosure || !pSetfield) {
        Log("missing lua exports");
        return 0;
    }
    MH_STATUS s0 = MH_Initialize();
    if (s0 != MH_OK && s0 != MH_ERROR_ALREADY_INITIALIZED) { Log("MH_Initialize failed"); return 0; }
    if (MH_CreateHook(pPcall, (void*)hkPcall, (void**)&oPcall) != MH_OK ||
        MH_CreateHook(pLoadBufX, (void*)hkLoadBufferX, (void**)&oLoadBufferX) != MH_OK ||
        MH_EnableHook(pPcall) != MH_OK || MH_EnableHook(pLoadBufX) != MH_OK) {
        Log("hook install failed");
        return 0;
    }
    Log("lua probe: hooks on lua_pcall + luaL_loadbufferx; drop vrmod\\lua_run.lua to run a script");

    ULONGLONG nextReport = GetTickCount64() + 10000;
    int reports = 0;
    for (;;) {
        if (!gPendingScript) {
            if (char* code = TakeScriptFile()) {
                if (InterlockedCompareExchangePointer((PVOID volatile*)&gPendingScript, code, nullptr) != nullptr) free(code);
                else Log("queued lua_run.lua (%zu bytes)", strlen(code));
            }
        }
        if (reports < 12 && GetTickCount64() >= nextReport) {
            Log("pcalls so far: %ld, states: %ld", gPcalls, gStateCount);
            nextReport += 10000;
            ++reports;
        }
        Sleep(200);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&gLogLock);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}

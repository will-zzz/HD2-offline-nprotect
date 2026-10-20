// hd2vr_cam — native render-camera hook (the "real VR" camera path).
//
// helldivers2.exe+4CFE40 runs on the render thread once per camera per frame:
//   void UpdateRenderCamera(RenderCamera* cam, float width, float height, float aspect)
// It combines the camera unit's world transform with the camera's local 4x4 at cam+0x238 and
// writes the world (+0xB8), view (+0xF8) and projection (+0x138) that culling and drawing read.
// We pre-multiply a head pose into that local matrix for the duration of the call, then restore
// it, so the game's own state never sees the change (aim, audio, gameplay camera stay untouched).
//
// Head pose comes from either:
//   - vrmod\camhook_cmd.txt (tests), one of:
//       yaw <deg> [pitch <deg>] [roll <deg>] [x <m>] [y <m>] [z <m>]   (x right, y forward, z up)
//       off | stats | filter screen | filter all | unhook
//   - hd2vr_cam_set_pose(const Hd2vrCamPose*), called by the VR module each frame (see cam_hook.h);
//     it can also override the vertical fov (cam+0x2C) and aspect for the call, and
//     hd2vr_cam_used() reports which pose seq the last view-camera update rendered with.
// Matrices are Stingray row-vector 4x4: rows x-axis, y-axis (forward), z-axis (up), translation.
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "MinHook.h"
#include "cam_hook.h"

static const wchar_t kCmdPath[] = L"C:\\HD2VR\\vrmod\\camhook_cmd.txt";
static const wchar_t kLogPath[] = L"C:\\HD2VR\\vrmod\\cam.log";
static const uintptr_t kUpdateRenderCameraRva = 0x4CFE40;
static const size_t kLocalOff = 0x238;           // float[16] camera-local pose

using UpdateFn = void(__fastcall*)(void* cam, float w, float h, float aspect);
static UpdateFn gOrig = nullptr;
static void* gTarget = nullptr;

static const size_t kVfovOff = 0x2C;            // float vertical fov (radians)

static SRWLOCK gLock = SRWLOCK_INIT;
static Hd2vrCamPose gPose = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, 0, 0, 0, 0 };
static volatile LONG gEnabled = 0;
static float gOffset[3] = {};                    // first-person offset (camera frame), under gLock
static volatile LONG gOffsetOn = 0;
static float gEye[3] = {};                       // first-person eye, world space, under gLock
static volatile LONG gEyeOn = 0;
static volatile DWORD gEyeTid = 0;
static volatile LONG gEyeUpdates = 0;               // thread that sets it (the game thread, from Lua)
static const size_t kWorldOff = 0xB8;            // float[16] world pose written by UpdateRenderCamera
static Hd2vrCamUsed gUsed = {};                 // the most recent view-camera update, any thread
static const int kThreadSlots = 4;
static Hd2vrCamUsed gThreadUsed[kThreadSlots] = {};
static volatile LONG gScreenOnly = 1;      // only cameras rendering at the game window's size
static volatile LONG gScreenW = 0, gScreenH = 0;
static volatile LONG gInFlight = 0;
static volatile LONG gUnhooked = 0;

// stats
static volatile LONG gCalls = 0;
static void* volatile gCams[8] = {};
static volatile LONG gCamCalls[8] = {};
static float gCamSize[8][2] = {};

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

// out = a * b (row-vector convention: apply a first, then b)
static void Mul(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

// Rigid (rotation + translation) inverse, row-vector convention.
static void RigidInverse(const float* m, float* out) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = m[c * 4 + r];
        out[r * 4 + 3] = 0;
    }
    for (int c = 0; c < 3; ++c) out[12 + c] = -(m[12] * out[c] + m[13] * out[4 + c] + m[14] * out[8 + c]);
    out[15] = 1;
}

static float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

static void NoteCamera(void* cam, float w, float h) {
    InterlockedIncrement(&gCalls);
    for (int i = 0; i < 8; ++i) {
        void* cur = gCams[i];
        if (cur == cam) { InterlockedIncrement(&gCamCalls[i]); return; }
        if (!cur && InterlockedCompareExchangePointer((void* volatile*)&gCams[i], cam, nullptr) == nullptr) {
            gCamSize[i][0] = w; gCamSize[i][1] = h;
            InterlockedIncrement(&gCamCalls[i]);
            return;
        }
    }
}

static void __fastcall Detour(void* cam, float w, float h, float aspect) {
    InterlockedIncrement(&gInFlight);
    float frameYaw = 0, framePitch = 0;
    bool haveFrame = false, realCam = false;   // realCam: not one of the identity scratch copies
    NoteCamera(cam, w, h);
    const bool view = !gScreenOnly || ((LONG)w == gScreenW && (LONG)h == gScreenH);
    if (view && gEyeOn && GetCurrentThreadId() == gEyeTid) {
        // First person: on the game thread, in the same frame Lua read the head, see where the game
        // camera really is (update it untouched first) and keep the eye offset in its frame. The
        // render thread reuses that offset, so a camera that moves between frames doesn't matter.
        gOrig(cam, w, h, aspect);
        const float* wm = (const float*)((char*)cam + kWorldOff);
        AcquireSRWLockExclusive(&gLock);
        const float d[3] = { gEye[0] - wm[12], gEye[1] - wm[13], gEye[2] - wm[14] };
        for (int a = 0; a < 3; ++a) gOffset[a] = d[0] * wm[a * 4] + d[1] * wm[a * 4 + 1] + d[2] * wm[a * 4 + 2];
        ReleaseSRWLockExclusive(&gLock);
        gOffsetOn = 1;
        InterlockedIncrement(&gEyeUpdates);
    }
    if (view && (gEnabled || gOffsetOn)) {
        Hd2vrCamPose p;
        AcquireSRWLockShared(&gLock);
        p = gPose;
        const float off[3] = { gOffset[0], gOffset[1], gOffset[2] };
        ReleaseSRWLockShared(&gLock);
        if (!gEnabled) {   // first-person offset only (no VR): identity head, game projection
            p = Hd2vrCamPose{ { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, 0, 0, 0, 0, 0, 0 };
        }
        if (gOffsetOn) {   // eye -> head pose -> first-person offset -> camera local -> unit
            p.head[12] += off[0];
            p.head[13] += off[1];
            p.head[14] += off[2];
        }
        float* local = (float*)((char*)cam + kLocalOff);
        float* vfov = (float*)((char*)cam + kVfovOff);
        float saved[16], moved[16];
        const float savedVfov = *vfov;
        memcpy(saved, local, sizeof(saved));
        if (gEnabled) {
            // The game camera as the game has it this frame (update it untouched once).
            float w0[16];
            gOrig(cam, w, h, aspect);
            memcpy(w0, (char*)cam + kWorldOff, sizeof(w0));
            frameYaw = atan2f(-w0[4], w0[5]);
            framePitch = asinf(fmaxf(-1.0f, fminf(1.0f, w0[6])));
            haveFrame = true;
            realCam = !(w0[0] == 1.0f && w0[5] == 1.0f && w0[10] == 1.0f);
            if (p.yawLock) {
                // world = head * W0 * Wc, Wc = yaw about the eye point by (yawRef - game yaw):
                // local = head * W0 * Wc * W0^-1 * saved
                const float d = WrapPi(p.yawRef - frameYaw);
                const float c = cosf(d), s = sinf(d);
                float px = w0[12], py = w0[13], pz = w0[14];
                if (gOffsetOn) {
                    px += off[0] * w0[0] + off[1] * w0[4] + off[2] * w0[8];
                    py += off[0] * w0[1] + off[1] * w0[5] + off[2] * w0[9];
                    pz += off[0] * w0[2] + off[1] * w0[6] + off[2] * w0[10];
                }
                // T(-pivot) * Rz(d) * T(pivot), row-vector
                const float wc[16] = { c, s, 0, 0,  -s, c, 0, 0,  0, 0, 1, 0,
                                       -px * c + py * s + px, -px * s - py * c + py, 0, 1 };
                (void)pz;
                float inv[16], a[16], b[16], e[16];
                RigidInverse(w0, inv);
                Mul(p.head, w0, a);
                Mul(a, wc, b);
                Mul(b, inv, e);
                Mul(e, saved, moved);
            } else {
                Mul(p.head, saved, moved);
            }
        } else {
            Mul(p.head, saved, moved);
        }
        memcpy(local, moved, sizeof(moved));
        if (p.vfov > 0) *vfov = p.vfov;
        gOrig(cam, w, h, p.aspect > 0 ? p.aspect : aspect);
        memcpy(local, saved, sizeof(saved));
        *vfov = savedVfov;
        gUsed.seq = p.seq;
    } else {
        gOrig(cam, w, h, aspect);
        if (view) gUsed.seq = 0;
    }
    if (view && !haveFrame) {   // untouched camera: its world pose is the game's own
        const float* wm = (const float*)((char*)cam + kWorldOff);
        frameYaw = atan2f(-wm[4], wm[5]);
        framePitch = asinf(fmaxf(-1.0f, fminf(1.0f, wm[6])));
        realCam = !(wm[0] == 1.0f && wm[5] == 1.0f && wm[10] == 1.0f);
    }
    if (view) {
        // The camera is updated on the game thread and again on the render thread (which presents);
        // keep each thread's last update so the presenter can ask about its own.
        const DWORD tid = GetCurrentThreadId();
        int slot = 0;
        while (slot < kThreadSlots - 1 && gThreadUsed[slot].tid && gThreadUsed[slot].tid != tid) ++slot;
        Hd2vrCamUsed& t = gThreadUsed[slot];
        t.gameVfov = *(float*)((char*)cam + kVfovOff);
        t.gameAspect = aspect > 0 ? aspect : (h > 0 ? w / h : 0);
        if (realCam) {   // scratch cameras with an identity pose would report a fixed 0 yaw
            t.gameYaw = frameYaw;
            t.gamePitch = framePitch;
        }
        t.seq = gUsed.seq;
        t.tid = tid;
        t.frames++;
        gUsed.gameVfov = t.gameVfov;
        gUsed.gameAspect = t.gameAspect;
        if (realCam) {
            gUsed.gameYaw = frameYaw;
            gUsed.gamePitch = framePitch;
        }
        gUsed.tid = tid;
        gUsed.frames++;
    }
    InterlockedDecrement(&gInFlight);
}

// First-person: move the rendered view by an offset in the game camera's own frame (x right,
// y forward, z up; metres). Lua sets it each frame to put the eyes at the Helldiver's head.
extern "C" __declspec(dllexport) void hd2vr_cam_set_offset(float x, float y, float z, int enabled) {
    AcquireSRWLockExclusive(&gLock);
    gOffset[0] = x; gOffset[1] = y; gOffset[2] = z;
    ReleaseSRWLockExclusive(&gLock);
    gOffsetOn = enabled ? 1 : 0;
}

// First person, preferred form: the eye position in world space (Lua, game thread, every frame).
// The hook turns it into an offset in the frame of the camera the game actually renders.
extern "C" __declspec(dllexport) void hd2vr_cam_set_eye(float x, float y, float z, int enabled) {
    AcquireSRWLockExclusive(&gLock);
    gEye[0] = x; gEye[1] = y; gEye[2] = z;
    ReleaseSRWLockExclusive(&gLock);
    gEyeTid = GetCurrentThreadId();
    gEyeOn = enabled ? 1 : 0;
    if (!enabled) gOffsetOn = 0;
}

extern "C" __declspec(dllexport) void hd2vr_cam_set_pose(const Hd2vrCamPose* p) {
    if (!p) { gEnabled = 0; return; }
    AcquireSRWLockExclusive(&gLock);
    gPose = *p;
    ReleaseSRWLockExclusive(&gLock);
    gEnabled = p->enabled ? 1 : 0;
}

// The calling thread's own last view-camera update if it makes them (the render thread does),
// otherwise the most recent one from any thread.
extern "C" __declspec(dllexport) void hd2vr_cam_used(Hd2vrCamUsed* out) {
    if (!out) return;
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < kThreadSlots; ++i)
        if (gThreadUsed[i].tid == tid) { *out = gThreadUsed[i]; return; }
    *out = gUsed;
}

// Test helper: head matrix only, game projection.
extern "C" __declspec(dllexport) void hd2vr_cam_set(const float* m, int enabled) {
    Hd2vrCamPose p = {};
    if (m) memcpy(p.head, m, sizeof(p.head));
    else { p.head[0] = p.head[5] = p.head[10] = p.head[15] = 1; }
    p.enabled = enabled;
    hd2vr_cam_set_pose(&p);
}

// Rotation (yaw about up/Z, pitch about right/X, roll about forward/Y), in camera space, plus offset.
static void HeadFromEuler(float yawDeg, float pitchDeg, float rollDeg, float x, float y, float z, float* m) {
    const float d2r = 3.14159265f / 180.0f;
    float cy = cosf(yawDeg * d2r), sy = sinf(yawDeg * d2r);
    float cp = cosf(pitchDeg * d2r), sp = sinf(pitchDeg * d2r);
    float cr = cosf(rollDeg * d2r), sr = sinf(rollDeg * d2r);
    float Y[16] = { cy, sy, 0, 0,  -sy, cy, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    float P[16] = { 1, 0, 0, 0,  0, cp, sp, 0,  0, -sp, cp, 0,  0, 0, 0, 1 };
    float R[16] = { cr, 0, -sr, 0,  0, 1, 0, 0,  sr, 0, cr, 0,  0, 0, 0, 1 };
    float RP[16];
    Mul(R, P, RP);      // roll, then pitch, then yaw
    Mul(RP, Y, m);
    m[12] = x; m[13] = y; m[14] = z;
}

static void Stats() {
    Log("cam: %ld calls since last stats; view updates %ld on render thread %lu, last seq %u",
        gCalls, gUsed.frames, gUsed.tid, gUsed.seq);
    Log("cam: first person eye %s (thread %lu, %ld offset updates), offset on %ld: right %.3f fwd %.3f up %.3f",
        gEyeOn ? "on" : "off", gEyeTid, gEyeUpdates, gOffsetOn, gOffset[0], gOffset[1], gOffset[2]);
    for (int i = 0; i < kThreadSlots; ++i)
        if (gThreadUsed[i].tid) Log("  view-camera thread %lu: %ld updates", gThreadUsed[i].tid, gThreadUsed[i].frames);
    gCalls = 0;
    for (int i = 0; i < 8 && gCams[i]; ++i) {
        const float* l = (const float*)((char*)gCams[i] + kLocalOff);
        const float* wm = (const float*)((char*)gCams[i] + kWorldOff);
        Log("  cam %d %p  calls %ld  size %.0fx%.0f  local.t (%.3f %.3f %.3f)  world.t (%.3f %.3f %.3f) yaw %.1f",
            i, gCams[i], gCamCalls[i], gCamSize[i][0], gCamSize[i][1], l[12], l[13], l[14], wm[12], wm[13], wm[14],
            atan2f(-wm[4], wm[5]) * 57.2958f);
        gCamCalls[i] = 0;
    }
}

static void RunCmd(char* cmd) {
    if (!strncmp(cmd, "off", 3)) { gEnabled = 0; Log("cam: off"); return; }
    if (!strncmp(cmd, "unhook", 6)) {
        gEnabled = 0;
        MH_DisableHook(gTarget);
        while (gInFlight) Sleep(1);
        gUnhooked = 1;
        Log("cam: unhooked");
        return;
    }
    if (!strncmp(cmd, "stats", 5)) { Stats(); return; }
    if (!strncmp(cmd, "filter all", 10)) { gScreenOnly = 0; Log("cam: applying to all cameras"); return; }
    if (!strncmp(cmd, "filter screen", 13)) { gScreenOnly = 1; Log("cam: applying to screen-size cameras (%ldx%ld)", gScreenW, gScreenH); return; }
    float yaw = 0, pitch = 0, roll = 0, x = 0, y = 0, z = 0;
    char* save = nullptr;
    for (char* tok = strtok_s(cmd, " \t\r\n", &save); tok; tok = strtok_s(nullptr, " \t\r\n", &save)) {
        char* val = strtok_s(nullptr, " \t\r\n", &save);
        if (!val) break;
        float v = (float)atof(val);
        if (!strcmp(tok, "yaw")) yaw = v;
        else if (!strcmp(tok, "pitch")) pitch = v;
        else if (!strcmp(tok, "roll")) roll = v;
        else if (!strcmp(tok, "x")) x = v;
        else if (!strcmp(tok, "y")) y = v;
        else if (!strcmp(tok, "z")) z = v;
    }
    float m[16];
    HeadFromEuler(yaw, pitch, roll, x, y, z, m);
    hd2vr_cam_set(m, 1);
    Log("cam: yaw %.1f pitch %.1f roll %.1f offset (%.2f %.2f %.2f)", yaw, pitch, roll, x, y, z);
}

// The game window's client size; cameras rendering at that size are the view (not shadows/probes).
static BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    if (GetClientRect(hwnd, &r) && r.right > 320) { *(HWND*)out = hwnd; return FALSE; }
    return TRUE;
}

static void UpdateScreenSize() {
    HWND hwnd = nullptr;
    EnumWindows(FindGameWindow, (LPARAM)&hwnd);
    RECT r;
    if (hwnd && GetClientRect(hwnd, &r)) { gScreenW = r.right; gScreenH = r.bottom; }
}

// True while any GameGuard process (GameMon*.des / GameGuard.des) is running.
static bool GameGuardRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsnicmp(pe.szExeFile, L"GameMon", 7) || !_wcsnicmp(pe.szExeFile, L"GameGuard", 9);
    CloseHandle(snap);
    return found;
}

static DWORD WINAPI Worker(LPVOID) {
    // Patching the game's own code while GameGuard runs gets the game killed (fail-fast 0xC0000409
    // ~45 s after launch). Hook only once GameGuard is gone: when built into the mod (injected at
    // process start) that means it must first appear and then be killed via kill-gg.flag.
#ifdef HD2VR_CAM_EMBEDDED
    bool seen = false;
    for (;;) {
        const bool running = GameGuardRunning();
        if (running && !seen) { seen = true; Log("cam: GameGuard running; hook waits for kill-gg.flag"); }
        if (seen && !running) break;
        Sleep(1000);
    }
    Sleep(2000);
#else
    while (GameGuardRunning()) Sleep(1000);
#endif
    HMODULE exe = GetModuleHandleW(nullptr);
    gTarget = (char*)exe + kUpdateRenderCameraRva;
    // Guard against a different game build: the function must start with our known prologue.
    static const unsigned char kPrologue[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x48, 0x8D, 0xA8, 0xF8, 0xFE, 0xFF, 0xFF };
    // An earlier copy of this DLL may already have hooked it (a jmp); MinHook chains over that.
    // Injected at process start, the code isn't unpacked yet (Steam stub): wait for it.
    bool ready = false;
    for (int i = 0; i < 1800 && !ready; ++i) {
        ready = *(unsigned char*)gTarget == 0xE9 || memcmp(gTarget, kPrologue, sizeof(kPrologue)) == 0;
        if (!ready) Sleep(100);
    }
    if (!ready) { Log("cam: prologue mismatch at exe+%llX; not hooking", (unsigned long long)kUpdateRenderCameraRva); return 0; }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { Log("cam: MH_Initialize %d", s); return 0; }
    if ((s = MH_CreateHook(gTarget, (void*)&Detour, (void**)&gOrig)) != MH_OK) { Log("cam: MH_CreateHook %d", s); return 0; }
    if ((s = MH_EnableHook(gTarget)) != MH_OK) { Log("cam: MH_EnableHook %d", s); return 0; }
    Log("cam: hooked UpdateRenderCamera at %p; write vrmod\\camhook_cmd.txt", gTarget);
    for (int tick = 0; !gUnhooked; ++tick) {   // once unhooked, leave the command file to a newer copy
        if (tick % 5 == 0) UpdateScreenSize();
        HANDLE f = CreateFileW(kCmdPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[512] = {};
            DWORD got = 0;
            ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
            CloseHandle(f);
            DeleteFileW(kCmdPath);
            RunCmd(buf);
        }
        Sleep(200);
    }
    return 0;
}

// Bit 0: yaw lock + game yaw reporting (Hd2vrCamPose.yawRef/yawLock, Hd2vrCamUsed.gameYaw).
extern "C" __declspec(dllexport) int hd2vr_cam_features() { return 1; }

// 1 while this copy's hook is installed (a newer test copy can unhook an older one).
extern "C" __declspec(dllexport) int hd2vr_cam_active() { return gOrig && !gUnhooked; }

#ifdef HD2VR_CAM_EMBEDDED
// Built into the mod DLL: its worker starts the hook.
namespace hd2vr { void StartCamHook() { CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)); } }
#else
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    }
    return TRUE;
}
#endif

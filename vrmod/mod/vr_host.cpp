// Hot-reload host for the VR module. See vr_host.h.
#include "vr_host.h"
#include <cstdio>
#include <cstring>

namespace hd2vr::vrhost {

static const wchar_t kModulePath[] = L"C:\\HD2VR\\vrmod\\build\\Release\\hd2vr_vr.dll";
static const ULONGLONG kCheckMs = 1000;
static const ULONGLONG kSettleMs = 1500;   // the file must stay unchanged this long (linker done)

using FrameFn = void (*)(ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource*, HWND);
using StatusFn = const char* (*)();
using ShutdownFn = void (*)();

static HMODULE gModule = nullptr;
static FrameFn gFrame = nullptr;
static StatusFn gStatus = nullptr;
static ShutdownFn gShutdown = nullptr;
static ULONGLONG gLoadedStamp = 0;     // last-write time of the build we loaded
static ULONGLONG gPendingStamp = 0;    // a newer build seen, waiting for it to settle
static ULONGLONG gPendingSince = 0;
static ULONGLONG gNextCheck = 0;
static char gStatusLine[160] = "VR: module not loaded";

static void Log(const char* msg) {
    HANDLE f = CreateFileW(L"C:\\HD2VR\\vrmod\\mod.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, msg, (DWORD)strlen(msg), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

static ULONGLONG BuildStamp() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(kModulePath, GetFileExInfoStandard, &a) || (a.nFileSizeLow == 0 && a.nFileSizeHigh == 0)) return 0;
    return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

static void Unload() {
    if (!gModule) return;
    if (gShutdown) gShutdown();   // destroys the OpenXR session and waits for the GPU
    FreeLibrary(gModule);
    gModule = nullptr;
    gFrame = nullptr;
    gStatus = nullptr;
    gShutdown = nullptr;
}

static void Load(ULONGLONG stamp) {
    Unload();
    wchar_t tmp[MAX_PATH], copy[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(copy, L"%shd2vr_vr_%llu.dll", tmp, GetTickCount64());
    if (!CopyFileW(kModulePath, copy, FALSE)) {
        sprintf_s(gStatusLine, "VR: can't copy the module (%lu)", GetLastError());
        return;
    }
    gModule = LoadLibraryW(copy);
    if (!gModule) {
        sprintf_s(gStatusLine, "VR: LoadLibrary failed (%lu)", GetLastError());
        return;
    }
    gFrame = (FrameFn)GetProcAddress(gModule, "hd2vr_vr_frame");
    gStatus = (StatusFn)GetProcAddress(gModule, "hd2vr_vr_status");
    gShutdown = (ShutdownFn)GetProcAddress(gModule, "hd2vr_vr_shutdown");
    if (!gFrame || !gStatus || !gShutdown) {
        strcpy_s(gStatusLine, "VR: module is missing exports");
        FreeLibrary(gModule);
        gModule = nullptr;
        return;
    }
    gLoadedStamp = stamp;
    Log("vrhost: loaded VR module build");
}

void Frame(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, HWND window) {
    const ULONGLONG now = GetTickCount64();
    if (now >= gNextCheck) {
        gNextCheck = now + kCheckMs;
        const ULONGLONG stamp = BuildStamp();
        if (stamp && stamp != gLoadedStamp) {
            if (stamp != gPendingStamp) {
                gPendingStamp = stamp;
                gPendingSince = now;
            } else if (now - gPendingSince >= kSettleMs) {
                Load(stamp);
            }
        }
    }
    if (gFrame) gFrame(device, queue, backBuffer, window);
}

const char* Status() {
    return gStatus ? gStatus() : gStatusLine;
}

} // namespace hd2vr::vrhost

// hd2vr_mod — injected mod entry point.
// On attach, start a worker thread that installs the D3D12 hook. Keep DllMain tiny
// (no heavy work under loader lock).
#include <windows.h>
#include "d3d12_hook.h"
#include "file_redirect.h"

namespace hd2vr { void StartCamHook(); }   // cam_hook.cpp: the render-camera hook for real VR

static HANDLE g_worker = nullptr;

static DWORD WINAPI Worker(LPVOID) {
    // Data reads from the Steam install go to the modified copy first (the install stays pristine).
    hd2vr::InstallFileRedirect();
    // Render-camera hook (waits on its own until the game's code is unpacked).
    hd2vr::StartCamHook();
    // Injected at process start (hd2vr_watch), the game hasn't loaded D3D12 yet: wait for it, then
    // install the swapchain Present / command-queue hooks (overlay + VR), retrying a few times.
    for (int i = 0; i < 1200 && !GetModuleHandleW(L"d3d12.dll"); ++i) Sleep(100);
    for (int attempt = 0; attempt < 10 && !hd2vr::InstallD3D12Hook(); ++attempt) Sleep(1000);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        hd2vr::RemoveD3D12Hook();
        break;
    }
    return TRUE;
}

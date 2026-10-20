#pragma once
// Hot-reload host for the VR module (hd2vr_vr.dll).
//
// The injected mod DLL keeps the D3D12 hooks; all VR logic lives in hd2vr_vr.dll, which this host
// loads from a temporary copy (so the build output is never locked). When the build output
// changes, the host shuts the module down, unloads it and loads the new build, inside the running
// game. Rebuild hd2vr_vr and the change is live within a couple of seconds.
#include <windows.h>
#include <d3d12.h>

namespace hd2vr::vrhost {
    // Call once per presented frame (after the overlay). backBuffer is in PRESENT state.
    void Frame(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, HWND window);

    // One line for the overlay.
    const char* Status();
}

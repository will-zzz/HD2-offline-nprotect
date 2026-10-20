#pragma once
// D3D12 / DXGI hook for the HD2 VR mod.
// Phase 2: hook IDXGISwapChain3::Present + ID3D12CommandQueue::ExecuteCommandLists and draw a
//          Dear ImGui overlay inside the game (proves the injection + render pipeline past GG).
// Phase 3+: the same Present hook is where per-eye rendering is submitted to the OpenXR swapchain.
namespace hd2vr {
    // Installs the hooks. Safe to call once from the injected worker thread.
    bool InstallD3D12Hook();
    // Unhooks and tears down ImGui/D3D resources.
    void RemoveD3D12Hook();
}

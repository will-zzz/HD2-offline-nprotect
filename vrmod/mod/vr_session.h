#pragma once
// hd2vr_vr.dll: the hot-reloadable VR module (loaded by vr_host in the injected mod DLL).
//
// Runs on the game's render thread from the Present hook. Creates an OpenXR session on the game's
// own D3D12 device and command queue, copies the finished back buffer into an OpenXR swapchain
// each frame and shows it as a quad layer. Head tracking v1 turns head yaw/pitch changes into
// relative mouse movement for the game window. Settings come from vrmod\vr_config.txt (written by
// the dashboard) and are re-read when the file changes; Home toggles head tracking in-game.
//
// Exports (C linkage, resolved by vr_host):
//   void hd2vr_vr_frame(ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource* backBuffer, HWND window)
//   const char* hd2vr_vr_status()
//   void hd2vr_vr_shutdown()      destroys the OpenXR session and waits for the GPU before unload

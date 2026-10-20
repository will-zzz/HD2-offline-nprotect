# 04 — Injection and the in-game overlay

With the offline build running and GameGuard handled (chapters 02–03), the next milestone was the
one that proves the entire modding pipeline: **inject a DLL and draw a Dear ImGui overlay inside
the game.** If an overlay renders in-game, injection works, the renderer is hooked, and everything
VR builds on top of that.

Source: [`../vrmod/mod/`](../vrmod/mod/) and [`../vrmod/injector/`](../vrmod/injector/).

## Injection

Two injectors, both ours:

- [`injector/inject.cpp`](../vrmod/injector/inject.cpp) — a plain **`LoadLibrary` injector**
  (`CreateRemoteThread` → `LoadLibraryW`). Used **after** the GameGuard kill flag, when the process
  is safe to touch. This is the normal path for loading mods onto the sitting-on-the-ship offline
  build.
- [`injector/watch_inject.cpp`](../vrmod/injector/watch_inject.cpp) — an **early injector** that
  watches for a new `helldivers2.exe` whose command line contains `--bundle-dir`, suspends it at
  creation, `LoadLibrary`-injects before the game has loaded its own modules, and resumes. The mod
  DLL's worker then waits for `d3d12.dll` to load and for the GameGuard kill flag before doing
  anything. Early injection means the hook DLL is present from the very start without a visible
  "attach" later.
- [`injector/scan_mem.cpp`](../vrmod/injector/scan_mem.cpp) — a small read-only memory scanner used
  for diagnostics (finding tables by signature from outside).

The mod entry point [`mod/dllmain.cpp`](../vrmod/mod/dllmain.cpp) keeps `DllMain` tiny (no heavy
work under loader lock): it spawns a worker thread that installs a file redirect, starts the camera
hook (which waits for GameGuard to be gone), waits for `d3d12.dll`, then installs the D3D12 hooks,
retrying a few times.

## The D3D12 hook

[`mod/d3d12_hook.cpp`](../vrmod/mod/d3d12_hook.cpp) uses **MinHook** on the DXGI swapchain vtable
and the command queue:

- **`IDXGISwapChain1::Present1`** (vtable slot **22**) — the path Helldivers 2 actually presents
  through (flip model). This is where the overlay is drawn each frame.
- **`IDXGISwapChain::Present`** (slot **8**) — hooked for diagnostics only.
- **`ID3D12CommandQueue::ExecuteCommandLists`** (slot **10**) — hooked to **capture the game's own
  command queue** (you need the real queue to submit your overlay's command list; you can't make
  your own queue and expect the swapchain to show it). The first thing the hook logs is
  "exec: queue captured".

### Getting the vtables without a device of your own

To find those vtable slots you need real COM objects. The standard trick: create a tiny throwaway
device + swapchain (a dummy window), read the function pointers out of its vtables to get the hook
targets, then release it. After that you hook the **game's** objects when they come through.

## The overlay (Dear ImGui on D3D12)

The ImGui DX12 backend needs its own descriptor heaps and careful synchronisation so it doesn't
fight the game's renderer:

- Its **own RTV and SRV descriptor heaps** (never the game's).
- **One command allocator per back buffer**, and a **fence** so the overlay's command list is only
  reset once the GPU is done with that frame's allocator.
- Submission on the **captured game queue** (from the `ExecuteCommandLists` hook), fenced.
- Everything wrapped in **SEH guards** (`SafeInit` / `SafeRender`) so a hiccup disables the overlay
  rather than crashing the game.
- `ImGui::GetIO().IniFilename = nullptr` — otherwise ImGui writes an `imgui.ini` into the game's
  working directory (the Steam install). Keep the install pristine.

The overlay is **hidden by default** and toggled with a key (Insert), so a normal-looking game is
the default and the debug UI only appears on demand.

### Verifying it worked

Success markers used during bring-up: the window title changed to include a live marker, a
`hook_live.txt` file written with "present1", and log lines in the mod log going
`present1: first frame, init` → `present1: overlay live`. Failure paths logged
`overlay init FAILED` or `render FAULT, overlay disabled` and left the game running.

## The test loop (what a full cycle looked like)

1. Kill every `helldivers2.exe`.
2. Run the elevated GameGuard neutraliser (chapter 03), then launch the game normally.
3. Play to the **ship**.
4. Drop the **kill flag** so the watchdogs are isolated and patches arm (game keeps running).
5. Verify no `GameMon*` processes remain.
6. Inject the mod DLL (`inject.exe helldivers2.exe <mod>.dll`), or rely on early injection.
7. Check the mod log / window title for the overlay-live markers.

Once this loop was reliable, the overlay was replaced by the real payload: the VR module (chapter
05) and the memory-mod engine (chapter 06).

## File redirect (keeping the install pristine while still "modding data")

[`mod/file_redirect.cpp`](../vrmod/mod/file_redirect.cpp) hooks file opens so that reads of certain
data files from the Steam install can be transparently served from a **modified copy elsewhere**,
without changing the install on disk. This was an avenue for data mods that never touches the
pristine install. (In practice most of the interesting modding ended up being **in-memory** —
chapter 06 — because the bundle data is packed, but the redirect is here as part of the kit.)

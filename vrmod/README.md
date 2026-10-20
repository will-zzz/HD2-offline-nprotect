# vrmod — the VR mod and modding tools

The injected mod for the offline build: D3D12 hook + ImGui overlay, OpenXR stereo, the native
render-camera hook, first-person, head-turns-body, the Lua probe, and memory tools. Full write-ups
in [`../docs/04-injection-and-overlay.md`](../docs/04-injection-and-overlay.md),
[`../docs/05-vr-stereo.md`](../docs/05-vr-stereo.md), and
[`../docs/06-memory-modding.md`](../docs/06-memory-modding.md).

## Layout

```
mod/
  dllmain.cpp        DLL entry; starts the worker (file redirect, camera hook, D3D12 hook)
  d3d12_hook.{h,cpp} DXGI Present1 + command-queue hook (MinHook); Dear ImGui overlay
  vr_host.{h,cpp}    hot-reload host: reloads hd2vr_vr.dll when it is rebuilt
  vr_session.{h,cpp} the OpenXR module: quad-layer screen, alternate-eye stereo, head tracking
  cam_hook.{h,cpp}   hook on UpdateRenderCamera (exe+0x4CFE40): head pose + first-person + yaw-lock
  file_redirect.{h,cpp} serve certain install data files from a copy (keeps the install pristine)
  lua_probe.cpp      hooks LuaJIT and runs a Lua file inside the game's Lua state
  cheat_scan.cpp     in-game memory search/patch, driven by a command file
  rename_label.cpp   in-memory label rename (sector/planet names) — example table is neutral
  hwbp.cpp           hardware-watchpoint (debug-register) tool — how the render camera was found
  input_probe.cpp    which input API the game reads the mouse through (diagnostic)
  input_test.cpp     send a mouse move from inside the game (diagnostic)
injector/
  inject.cpp         LoadLibrary injector (use after the GameGuard kill flag)
  watch_inject.cpp   early injector (suspend the new process at creation, inject, resume)
  scan_mem.cpp       read-only external memory scanner (diagnostics)
lua/                 the Lua scripts the probe runs (camera recon/tests, first-person, monitor)
vr_config.sample.txt example VR config the VR module reads live
```

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Needs Visual Studio Build Tools 2022 (MSVC v143 + Windows SDK) and CMake. Dependencies (MinHook,
Dear ImGui, OpenXR SDK) are fetched by CMake.

## Run (offline build only, after GameGuard is handled)

```powershell
# the game running on the offline build, sitting on the ship, GameGuard neutralised (see docs/03)
build\Release\hd2vr_inject.exe helldivers2.exe build\Release\hd2vr_mod.dll
```

Then toggle the overlay (Insert), put the headset on (OpenXR runtime running), and drive VR/mods
from the config file and the dashboard (`../dashboard/`).

## Notes for readers

- During development the DLLs carried **versioned output names** (`hd2vr_modNN`, `hd2vr_camNN`, …)
  so a new build could be injected beside a running one. This public CMake uses plain names
  (`hd2vr_mod`, `hd2vr_cam`, …). Some Lua loaders (`lua/fp_camera.lua`) list the old versioned
  names when they look up the hook by `ffi.load` — adjust to the name your build produces.
- The camera hook and mod wait for a **GameGuard kill flag** before patching game code, because
  patching while GameGuard is alive triggers a fail-fast crash (see `../docs/03`). On a build with a
  different (or no) anti-cheat that gating would need rethinking.
- Hardcoded log/command-file paths assume `C:\HD2VR\...`. They're how the tools talk to each other
  (drop a command file, read a log); adjust to your layout.

## Not included

The two GameGuard-specific experiments from the original project — a path-spoof hook
(`gg_pathspoof`) and an APC early-bird injector (`earlybird`) — are **described in the docs but not
shipped**, because they're anti-cheat-evasion tooling that generalises to other online games. See
`../docs/03-gameguard-nprotect.md`.

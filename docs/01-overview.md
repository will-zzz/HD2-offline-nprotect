# 01 — Overview

## The one-paragraph version

Helldivers 2 is an online-only, third-person co-op shooter on the Stingray (BitSquid) engine,
rendered with D3D12. To play it **offline and single-player**, we stood up a **local server
emulator** that answers all of Arrowhead's/PlayFab's/Sony's backends on `127.0.0.1`, with a
hosts-file redirect so the game can't reach the real internet. To **mod** it — the end goal was
VR — we neutralised the **nProtect GameGuard** anti-cheat **on an offline copy only**, injected a
DLL, hooked D3D12 `Present`, and drew an in-game overlay. From there we built true **stereo VR**
through a hook on the game's own render camera, a **first-person** view, **head-turns-body**
steering, and a **live memory-modding** dashboard. The real Steam/Arrowhead account was never put
at risk because no authenticated traffic ever reached the real servers.

## The build this applies to

- Helldivers 2, Steam AppID **553850**, build **25480438**, GameGuard engine dated **Apr 22 2025**.
- Render API: **D3D12** (`D3D12Core.dll`, `d3d12.dll`, `dxgi.dll`).
- Engine: **Stingray / BitSquid** (Lua scripting via LuaJIT; string IDs are 64-bit MurmurHash,
  seed 0).
- Packer: **WinLicense / Themida**-style (sections are ciphertext on disk, unpacked in memory
  ~0.4–1.4 s after launch; anti-debug, stripped `.pdata`).
- Anti-cheat: **nProtect GameGuard**, **user-mode** (processes `GameMon.des` / `GameMon64.des`;
  in-process modules `npggNT64.des` / `npsc64.des`). No persistent kernel driver was found at rest.

> After the EAC transition none of the GameGuard-specific details apply. See
> [`08-eac-transition.md`](08-eac-transition.md).

## Timeline (what was solved when)

| Date (2026) | Milestone |
|---|---|
| Oct 16 | Phase 0 recon: GameGuard footprint mapped, network map, clean baseline (observation only). |
| Oct 17 | **Offline boot solved** — game reaches the ship with a fake login and zero real-server contact ("Error 0, status -2" beaten by serving captured real GET bodies). |
| Oct 17 | Confirmed full **offline mission play** (drop → objectives → extract), all local. |
| Oct 18 | Toolchain up (MSVC + CMake). GameGuard recon deepened (error 114/124/153 meanings). Injection pipeline proven (early-inject loads our DLL; command-queue hook fires). Live memory modding begun (stratagem/attack table). |
| Oct 18 | **GameGuard neutralised on the offline build**; DLL injection + **D3D12/ImGui overlay** working in-game. |
| Oct 18–19 | Mod dashboard (all mods off by default). Custom stratagems, explosion swaps, in-memory text renames. |
| Oct 19 | **VR milestone 1**: the game on a virtual screen in the headset (OpenXR quad layer). Then **head tracking** (head→mouse). |
| Oct 19 | **Real stereo VR** via a native hook on the render camera; the Quest Link FOV double-vision bug found and fixed. **First-person** camera at the Helldiver's eyes. **Head-turns-body** steering. |
| ~late 2026 | Game swapped GameGuard → **EAC**. This archive becomes historical. |

## The phase structure

The project was organised as phases, each gating the next:

- **Phase 0 — observe.** Read-only study of the unmodified game: what GameGuard puts on disk and in
  memory, what the game talks to on the network, a clean baseline to diff against. No changes.
- **Phase 1 — offline.** Redirect the backends to a local emulator; prove **zero egress**; get the
  game to boot and play a solo mission with nothing reaching the real internet.
- **Phase 2 — inject.** Neutralise GameGuard on the offline build; inject a DLL; hook D3D12; draw
  an ImGui overlay in-game. This proves the whole modding pipeline.
- **Phases 3–5 — VR.** OpenXR session and per-eye swapchains (stereo); head tracking into the
  render camera; (aspirational) 6DOF motion controllers.

Memory modding (custom stratagems, etc.) grew alongside Phase 2 once injection worked.

## Where the detail lives

- Offline method → [`02-offline-gateway.md`](02-offline-gateway.md)
- GameGuard (era-specific) → [`03-gameguard-nprotect.md`](03-gameguard-nprotect.md)
- Injection + overlay → [`04-injection-and-overlay.md`](04-injection-and-overlay.md)
- VR → [`05-vr-stereo.md`](05-vr-stereo.md)
- Memory modding → [`06-memory-modding.md`](06-memory-modding.md)
- Step-by-step reproduction → [`07-reproduce.md`](07-reproduce.md)
- The grab-bag of everything else → [`99-misc-findings.md`](99-misc-findings.md)

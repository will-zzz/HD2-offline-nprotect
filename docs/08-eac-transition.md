# 08 — The EAC transition: why this archive is historical

Around late 2026, Helldivers 2 **replaced nProtect GameGuard with Easy Anti-Cheat (EAC)**. This was
anticipated during the project — an EAC build was reportedly in testing, which is why the modded
work was always pinned to the GameGuard build and never updated.

This chapter exists so nobody mistakes this archive for a current how-to.

## What stays true

The **ideas** that aren't anti-cheat-specific are still valid as concepts:

- **The offline server emulator** (chapter 02) — a local backend + hosts redirect + captured
  real GET bodies to satisfy a strict client. This is an anti-cheat-agnostic technique; it's how
  private-server / single-player projects work for many always-online games. The exact endpoints and
  body shapes may have changed with game updates, but the approach is unchanged.
- **D3D12 hooking, OpenXR stereo, the render-camera hook idea** (chapters 04–05) — these are about
  the **renderer and the engine**, not the anti-cheat. The function offsets are build-specific and
  will have moved, but "find `UpdateRenderCamera`, pre-multiply a head pose, resample each eye into
  its native FOV" is a transferable recipe for VR-modding a flat D3D12 game.
- **Memory modding methodology** (chapter 06) — signature-based table discovery, the Lua probe idea,
  in-memory text rewriting. The addresses are gone; the method isn't.

## What does **not** transfer

- **Everything in chapter 03.** GameGuard is user-mode; EAC is a **kernel-mode** anti-cheat with
  a completely different architecture, different detection, and a different threat model. The error
  codes (114/124/153), the wrapper status gate, the in-memory atomic-patch approach, the
  watchdog-Job-Object isolation — **none of it applies to EAC.** The GameGuard neutralisation is a
  dead chapter the moment the game is on EAC.
- **The injection/overlay/VR pipeline as a turnkey** — even where the renderer ideas transfer, the
  *delivery* (getting a DLL into the process and keeping it there) is exactly what a kernel anti-cheat
  is built to stop. On an EAC build this is not a user-mode weekend project, and attempting it on an
  **online** build is both against the rules and a ban risk.

## The honest takeaway

This project worked because, on the GameGuard build, the anti-cheat was **user-mode** and the whole
thing could be kept **offline and single-player**, so there was no server, no match, and no account
exposure. Those two facts — user-mode AC and a fully-offline target — are what made it tractable and
safe. On an EAC build, neither holds the same way.

If someone wants VR in the current game, the realistic, rules-respecting paths are different:
official VR support (if it ever ships), a vendor-sanctioned mod SDK (if one appears), or
flat-screen-to-VR wrappers that don't touch the game process at all (they capture the rendered
output and reproject it — no injection, no anti-cheat interaction). Those are out of scope for this
archive, which is specifically the record of what was done on the GameGuard build.

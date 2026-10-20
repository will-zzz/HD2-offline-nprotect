# 07 — How it all fit together (reproduction order)

This is the order things had to happen on the **GameGuard build** to get from "online-only game" to
"offline VR sandbox." It is a **historical** sequence — on today's **EAC** build the anti-cheat
steps do not apply (see [`08-eac-transition.md`](08-eac-transition.md)) — and it is deliberately not
a copy-paste script, because it depends on your own owned copy, your own captured data, and a
GameGuard-specific tool this archive does not ship.

## Prerequisites

- A legitimately-owned copy of the game (the GameGuard build, build ~25480438).
- Windows 10/11 x64, a C++ toolchain (**Visual Studio Build Tools 2022**, MSVC v143 + Windows SDK)
  and **CMake** to build the VR mod.
- For VR: a Meta Quest 2 and **Quest Link** (the Oculus OpenXR runtime), or another OpenXR runtime.
- Admin rights (hosts-file edits and the GameGuard step need elevation).

## Step 1 — Offline boot (chapter 02)

1. Build and run the **gateway** ([`../gateway/`](../gateway/)) — it listens on `127.0.0.1:80/443`
   with a multi-SAN cert.
2. Point the HD2 backend domains at `127.0.0.1` with the hosts script
   ([`../scripts/hosts-offline.ps1`](../scripts/hosts-offline.ps1), elevated).
3. **Capture your own replay data**: run the gateway in capture mode, do one real login so the
   client holds a session, read the session token from the gateway log, and fetch the exact real GET
   bodies for every polled endpoint into the gateway's `replay/` folder. (This is the one step that
   briefly uses the real server — a normal login and some GETs. See chapter 02.)
4. Turn capture off, launch the game. With the fake login + your captured GETs, it boots to the
   ship with **zero** real-server connections. Verify with `netstat` (only `127.0.0.1`).

At this point you have **offline single-player**: drop into a solo mission, play it, extract, all
local. The free-text Major Order / dispatch mods (chapter 02) work here with no code at all.

## Step 2 — Neutralise GameGuard on the offline build (chapter 03)

> Documented, not shipped. This is the GameGuard-era step that does not transfer to EAC.

1. Run the elevated **in-memory GameGuard neutraliser** in passive mode, then launch the game
   normally.
2. Play all the way to the **ship** with GameGuard alive and happy.
3. Drop the **kill flag**. The neutraliser isolates the watchdog processes (Job Object) and arms its
   in-memory, atomic patches; the game keeps running.
4. Verify: no `GameMon*` processes remain, the game is still running, and the neutraliser's log
   ends with success.

No files on disk are modified — the install and GameGuard's own files are pristine; only runtime
memory is touched, for this session.

## Step 3 — Inject the mod and prove the overlay (chapter 04)

1. Build the VR mod with CMake (`cmake -S vrmod -B vrmod/build -A x64 && cmake --build … Release`).
2. Inject the mod DLL (`inject.exe helldivers2.exe hd2vr_mod.dll`), or use the early injector so the
   hook is present from launch (it waits for the kill flag internally).
3. Confirm the **ImGui overlay** draws in-game (Insert toggles it). That proves injection + the
   D3D12 hook.

## Step 4 — VR (chapter 05)

1. Put the headset on in Quest Link; the OpenXR session wakes from IDLE.
2. Milestone 1: the game appears on a **virtual screen** in the headset (quad layer).
3. Enable **stereo** in `vr_config.txt`. The camera hook drives alternate-eye rendering; `End`
   recenters. (If you see double vision on a different runtime, it's almost certainly the
   asymmetric-FOV issue from chapter 05 — resample each eye into its native FOV.)
4. Enable **first-person** and **body-follow** from the dashboard. Tune eye height / steering gain.

## Step 5 — Memory mods (chapter 06), optional

Start the dashboard, turn on whatever mods you want (all off by default). The engine re-discovers
the tables each launch.

## Step 6 — Going back to normal online play (important)

Before playing the real game online again:

1. **Remove the offline hosts redirects** ([`../scripts/hosts-online.ps1`](../scripts/hosts-online.ps1),
   elevated) — it backs up the hosts file, removes only the HD2 lines, and flushes DNS. Without this
   the real game can't reach its servers.
2. **Stop every mod/injector/dashboard** and make sure no modded `helldivers2.exe` is running.
3. **Move the modded-session crash reports out of the game's upload queue** (the game queues crash
   dumps that, on a later normal launch, could be uploaded). Move them to a backup folder so a
   normal launch carries nothing from the offline work.
4. The Steam install was never modified, and no modded session ever authenticated to the real
   servers, so nothing persists — but do the three steps above so the **next** online launch is
   clean.

GameGuard may show its generic "send us your log files" help page once after heavy offline
sessions (it noticed earlier runs ended abnormally and it self-updated) — that is not a detection or
ban; if it shows a numeric error code, that's worth looking into, a help-page pop is not.

# 99 — Everything else (the grab-bag)

Unorganised on purpose. This is the pile of smaller findings, dead ends, oddities, and
not-very-important things learned along the way, kept so the archive is complete. Some of it is
useful, some is trivia, some is "we spent a day on this so here it is."

## The "Error 0, status -2" investigation went through several wrong theories

Before captured GET bodies solved the offline boot (chapter 02), the `-2` was chased through a long
list of dead ends. Kept here because the wrong turns are instructive:

- **Login body shape.** Tried: a flat `sessionId` marker string, a JWT, a full PlayFab-shaped flat
  body, adding `code:200`/`status:"OK"` siblings, a wrapped `{code,status,data}` envelope (this one
  actively **breaks** the session echo — Arrowhead's real login response is **flat**). All still
  `-2`. The login body was never the gate; the client accepts a fake flat `sessionId` fine.
- **The pubsub websocket.** A strong lead for a while: the client opens a SignalR websocket and the
  theory was it waits for a server **push** (`ReceiveMessage` / `ReceiveSubscriptionChangeMessage`)
  that our mock never sends. We worked out the SignalR framing (server→client pushes are Invocation
  frames, `type 1`, nil invocationId; client invocations need a well-formed Completion
  `[3,{},"<invocationId>",3,<result>]`). This may well have been *a* contributor, but the thing that
  actually cleared `-2` was serving the right **GET bodies** — the client strictly validates those
  and backs off when they're the wrong shape.
- **The real lesson:** for an online game that stalls offline, the gate is often **strict
  validation of specific GET responses**, not login and not the realtime channel. Capture the real
  bodies; don't guess shapes.

## External memory forensics is dead while GameGuard runs

A lot of early effort tried to read the game's memory from **outside** the process (external
scanners, full process dumps via comsvcs). **GameGuard hides game-private memory from external
readers** — a 7.7 GB full dump yielded **zero** plaintext game strings; a 730 MB "readable" region
was all non-game. Everything useful had to be done **from inside** (injected), after GameGuard was
handled. This is why the whole modding effort is injection-based rather than external-tool-based.

## RE artifacts from the packed exe

- The exe is **WinLicense/Themida**-style packed: code sections are ciphertext on disk, real
  instructions only exist in memory after the unpacker runs (~0.4–1.4 s post-launch). Strings don't
  exist statically; dump from live memory.
- Stingray/BitSquid string IDs are **MurmurHash-64, seed 0** (the BitSquid `murmur_hash_64`). This
  is how resource/name hashes like `71e52f800636b336` relate to names.
- During the login-parser RE: `json_object_get` and a `memcmp` loop and a `PlayerProfile` compare
  were located in the unpacked image; needle strings for `code`/`data`/`InfoResultPayload`-ish keys
  were found at their RVAs. (Build-specific; listed in the original notes, omitted here as they're
  only meaningful against that exact image.)

## The copied-install dead end (error 114) — see chapter 03

Briefly: running a **copy** of the install from another drive fails GameGuard's path/identity check
(114). We proved it isn't the obvious Win32 path APIs (GameGuard uses direct syscalls), and gave up
on the copy route in favour of running the **registered install** and neutralising GameGuard's
runtime gate instead (files never modified). Full story in chapter 03.

- The **D: copy also wouldn't launch standalone** for a different reason: the Steam DRM stub exits
  `0x1` when the exe is launched directly outside Steam. We deliberately **did not** bypass the DRM
  stub — that's a DRM-circumvention line we chose not to cross; the registered-install route avoids
  it entirely (Steam launches it normally).

## GameGuard deployment trivia (GameGuard build)

- Installed by `tools\GGSetup.exe` at game install: deploys `GameMon.des` to `C:\Windows\SysWOW64\`
  and registers an **`npggsvc`** service ("nProtect GameGuard Service", **user-mode own-process**,
  start=on-demand, LocalSystem). Not running at rest.
- Core package in `<game>\bin\GameGuard\`: `GameGuard.des`, `GameMon.des` (32-bit),
  `GameMon64.des` (64-bit), `npggNT.des`/`npggNT64.des` (hook modules), `GameGuard.ver`.
- **Per-session `.erl` logs**, **3 generations kept** (current + rotated `0*.erl`, `1*.erl`). These
  rewrite themselves every launch — **normal GameGuard behaviour**, not evidence of anything you
  did. Don't "clean" or revert them; it's churn.
- **No persistently-registered kernel driver** was found at rest (despite Arrowhead publicly calling
  GameGuard "kernel-level"). On this build, at rest and in the sessions observed, it behaved as
  user-mode. (We did not exhaustively prove the absence of a dynamically-loaded driver during online
  play — the offline work never needed to.)
- **Error 114/500 = "colliding program"**: GameGuard's documented behaviour is to detect and close
  programs it distrusts (debuggers, some overlays) **locally** — the failure mode is the *game*
  erroring/closing, **not a ban**.

## Engine / rendering oddities

- **The game resets the gameplay camera every frame.** Writing the camera transform from Lua (even
  in `render()`) doesn't stick — the engine overwrites it. This is why VR needed the native
  `UpdateRenderCamera` hook (chapter 05) rather than a Lua camera driver.
- **The engine's stereo mode is stubbed.** `Camera.STEREO == Camera.MONO == 0` — there's no built-in
  stereo path to light up; stereo had to be done externally via alternate-eye rendering + OpenXR.
- **Swapped explosions have no sound.** Giving a weapon/stratagem a different explosion ID brings the
  new blast and damage but not the sound — the sound belongs to the source object, not the explosion
  table entry.
- There's a **"DIVE FOR LIBERTY"** bit of flavour text embossed on the ship's hangar floor — you
  notice these things when you're staring at the floor in VR debugging the camera.

## Input plumbing

- The game reads the mouse through the usual Windows stack (raw input / DirectInput / GameInput all
  present in-process). **Mouse movement injected from *another* process does not turn the camera** —
  which is why "head turns the body" (chapter 05) sends its steering `SendInput` **from inside** the
  injected mod, and the low-level mouse hook that distinguishes the player's real mouse from the
  mod's own injected moves also lives in-process.

## Things we never fully solved

- **The 5×Down stratagem input code.** We could rename a stratagem and change its effects, but never
  found where the **input sequence** (the arrow-key code) for a stratagem is stored, so "make the
  code just five Downs" was never done.
- **Eagle rearm / uses hack.** Changing arrival delay and explosions worked; changing the *number of
  uses / rearm time* was not located.
- **Exact bomb count** for the strafing runs (stuck around 5–7; the field that controls the count
  wasn't pinned — likely behind the `+0x68` run-settings reference).
- **Explosion sound transfer** (see above) — not solved.

## Tooling niceties

- **Backgrounded screenshots.** [`../scripts/shot.ps1`](../scripts/shot.ps1) grabs the game window
  via `PrintWindow`, so it works even when the game isn't foreground — essential for a
  headset-driven workflow where the desktop window is behind everything.
- **Killing a WerFault-frozen process.** A crashed game sometimes freezes under WerFault and won't
  die; `NtResumeProcess` then `TerminateProcess` gets it.
- **The unit monitor could hang the game** when leaving a mission if it kept iterating units across
  the transition — bound its work and only run it during tests.

## A tangential side-idea (not pursued far): Elden Ring × HD2 "passthrough"

A separate experiment imagined playing as the Tarnished inside HD2 (one game owning the
player/combat, the other the world) via a bridge DLL in each game and a compositor. It was
offline/EAC-off only and got as far as a code scaffold and some RE of the other game's frame hook.
It's unrelated to the VR work and is mentioned only for completeness; no account data or game files
from it are in this archive.

## Overall honest assessment

The project succeeded because of two facts specific to the moment: the anti-cheat was **user-mode**,
and the target was kept **fully offline and single-player** so there was never a server, a match, or
an account to endanger. Remove either and it would have been a different, much harder, and much less
defensible thing. With the EAC transition, both facts changed — which is the right place for the
story to end. See [`08-eac-transition.md`](08-eac-transition.md).

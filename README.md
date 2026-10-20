# HD2 Offline + VR — nProtect/GameGuard-era archive

An archive of how one player took a copy of **Helldivers 2** fully **offline** and then built a
real **first-person VR mod** for it, during the period when the game shipped with **nProtect
GameGuard** as its anti-cheat.

This repository is **documentation and our own tooling**. It contains **no Helldivers 2 game
files** (no executables, bundles, GameGuard binaries, or captured account data) and nothing that
touches online multiplayer. It is a write-up of a reverse-engineering and accessibility project,
preserved so others can understand how it was done.

> ### ⏰ This describes the GameGuard era. The current game uses EAC.
> Around late 2026, Helldivers 2 replaced nProtect GameGuard with **Easy Anti-Cheat (EAC)**.
> **Everything in the "get it running offline" and "neutralise the anti-cheat" parts of this
> archive is specific to the GameGuard build and will not apply as-is to an EAC build.** EAC is a
> different, kernel-mode anti-cheat with different behaviour. This is a *historical* record of a
> method that worked on a specific build at a specific time, not a current how-to. The
> offline-server and VR-rendering ideas are still interesting; the anti-cheat specifics are not
> transferable. See [`docs/08-eac-transition.md`](docs/08-eac-transition.md).

---

## What this project actually was

The goal was **single-player, offline, ban-safe** play — and then **VR** on a Meta Quest 2 — of a
game that is normally online-only. Two hard problems had to be solved:

1. **Make the game boot and play with no real servers.** Helldivers 2 stalls at startup if it
   can't reach Arrowhead/PlayFab/Sony. The fix was a **local server emulator** ("gateway") plus a
   hosts-file redirect, so the game talks only to `127.0.0.1`. See
   [`docs/02-offline-gateway.md`](docs/02-offline-gateway.md).

2. **Get a mod into the game.** The VR work needed to inject code and hook the renderer. On the
   GameGuard build that required neutralising the anti-cheat **for the offline, single-player
   build only**. See [`docs/03-gameguard-nprotect.md`](docs/03-gameguard-nprotect.md).

On top of that foundation: a **D3D12 + ImGui overlay**, then true **stereo VR via OpenXR** driven
by a hook on the game's own render camera, a **first-person** camera, head-turns-body steering,
and a **live memory-modding** dashboard (custom stratagems, explosion swaps, in-memory text
renames). All of it offline.

### The ban-safety principle (why this never endangered an account)

Every part of this was built around one rule: **nothing authenticated ever reached Arrowhead's
servers.**

- Login is **faked locally**; no real session token is ever presented to the real backend.
- The endpoints that would **write** to an account (progress, rewards, purchases) are
  **cryptographically signed** by the real server and cannot be forged — and they were pointed at
  `127.0.0.1`, so they went nowhere.
- Progression is **server-authoritative**: with no authenticated session, the real backend has no
  record the offline session happened. Nothing persists upstream.
- The anti-cheat was only ever neutralised on an **offline copy with no multiplayer** — there is no
  other player and no match to affect.
- The original Steam install was **never modified**; mods are injected into memory at runtime.

This is the same reason private-server / single-player-server-emulator projects for other
always-online games are a different thing from online cheating: there is no server, no match, and
no other player involved.

---

## What's in here (and what's deliberately not)

### Included — our own work

| Path | What |
|---|---|
| [`docs/`](docs/) | The full write-up: offline method, GameGuard-era findings, injection, VR, memory modding, reproduction steps, and a big "misc findings" grab-bag. |
| [`gateway/`](gateway/) | The C# offline server emulator (`gateway.cs`) and how to capture your own data. **No captured account data is included.** |
| [`scripts/`](scripts/) | hosts-file switch scripts, a backgrounded window-screenshot helper, launch helpers. |
| [`vrmod/`](vrmod/) | The VR mod source: D3D12 hook + ImGui overlay, the OpenXR stereo module, the native render-camera hook, first-person + body-follow, the Lua probe, memory tools, and the injector. |
| [`dashboard/`](dashboard/) | The Python/HTML mod dashboard (toggles every mod, all off by default). |

### Not included — on purpose

- **No Helldivers 2 game files.** No `helldivers2.exe`, no bundles, no GameGuard binaries. You need
  your own legitimately-owned copy for any of this to mean anything.
- **No captured account/server data.** The gateway needs real server responses to satisfy the
  client; those were captured once from a live session and contain account identifiers. They are
  **not** in this repo. The method to capture your own is documented instead.
- **No GameGuard-defeat tool or its exact patch offsets.** Neutralising GameGuard was done with a
  **community-authored, user-mode, in-memory patcher** — not our code. Because a turnkey GameGuard
  defeat applies to *other* live online games that still use GameGuard, this archive **documents
  the approach, the findings, and why it was offline-only**, but does not redistribute the tool or
  its exact mutation offsets. Two of our own GameGuard-specific hook experiments
  (`gg_pathspoof`, `earlybird`) are described but not shipped for the same reason.
- **No personal data.** Paths are genericised to `C:\HD2VR`; no account names, Steam IDs, or
  tokens appear anywhere.

---

## Documentation map

1. [`docs/01-overview.md`](docs/01-overview.md) — the whole picture, timeline, and status.
2. [`docs/02-offline-gateway.md`](docs/02-offline-gateway.md) — the offline server emulator: the
   DNS redirect, the gateway, the "Error 0, status -2" boot fix, the capture method.
3. [`docs/03-gameguard-nprotect.md`](docs/03-gameguard-nprotect.md) — GameGuard recon and the
   offline-build neutralisation (documented, not shipped); why it was ban-safe.
4. [`docs/04-injection-and-overlay.md`](docs/04-injection-and-overlay.md) — DLL injection and the
   D3D12 + Dear ImGui in-game overlay (the "injection works" milestone).
5. [`docs/05-vr-stereo.md`](docs/05-vr-stereo.md) — OpenXR, the render-camera hook, alternate-eye
   stereo, the Quest Link FOV fix, first-person, and head-turns-body.
6. [`docs/06-memory-modding.md`](docs/06-memory-modding.md) — the stratagem/attack table, explosion
   swaps, in-memory text renames, and the dashboard.
7. [`docs/07-reproduce.md`](docs/07-reproduce.md) — end-to-end, the order things had to happen.
8. [`docs/08-eac-transition.md`](docs/08-eac-transition.md) — what changed with the EAC swap and
   why this archive is now historical.
9. [`docs/99-misc-findings.md`](docs/99-misc-findings.md) — everything else we found, including the
   unimportant and the dead ends, unorganised on purpose.
10. [`docs/reference/gameguard-technical-notes.md`](docs/reference/gameguard-technical-notes.md) —
    a longer, defensively-framed technical study of how GameGuard-class anti-cheats work.

---

## Paths and placeholders

Our tooling was developed under a project root of `C:\HD2VR` in this archive (the examples assume
it). Many source files have that path hard-coded for logs and command files — adjust to wherever
you put things. Nothing here is plug-and-play; it's a record of a working setup, not a product.

## Legal / ethical note

This documents reverse-engineering of software the author owned, to run it **offline and
single-player** and to build a **VR accessibility mod**, with explicit care that it never affected
online play or any account. Don't use any of this to cheat in multiplayer, to evade anti-cheat on
a live online game, or against software you don't own. Anti-cheat vendors and game publishers
prohibit tampering with online builds; keep this where it belongs — offline, single-player, your
own copy.

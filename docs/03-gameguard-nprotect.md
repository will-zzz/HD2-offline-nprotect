# 03 — nProtect GameGuard (era-specific) and the offline neutralisation

> **Read this first.** This chapter is about the anti-cheat that Helldivers 2 shipped with **at the
> time** (nProtect GameGuard). The game has since moved to **EAC**, so none of the GameGuard
> specifics below apply to the current game. This is a historical record of what was true on the
> GameGuard build, and of **why neutralising it on an offline, single-player copy was ban-safe**.
>
> This chapter **documents the approach and the findings** but deliberately **does not ship a
> GameGuard-defeat tool, its exact patch offsets, or its shellcode**. GameGuard protects *other*
> live online games; a turnkey defeat is not something to hand out publicly. For a deeper,
> defensively-framed study of how GameGuard-class anti-cheats work in general, see
> [`reference/gameguard-technical-notes.md`](reference/gameguard-technical-notes.md).

## Why the anti-cheat was in the way at all

The offline gateway (chapter 02) gets the game **running** offline. But the VR goal needed to
**inject code and hook the renderer**. On the GameGuard build, GameGuard actively resisted that:
it monitors the game process for tampering and refuses to let a modified/injected process run
normally. So to mod the offline build, GameGuard had to be handled — **only** on the offline copy,
which has no multiplayer and therefore no match and no other player to affect.

## Recon — what GameGuard was on this build

- **User-mode only.** External watchdog processes `GameMon.des` / `GameMon64.des`, plus in-process
  modules `npggNT64.des` (init/detection) and `npsc64.des` (scanner). **No persistent kernel driver
  at rest** — a big deal, because user-mode anti-cheat is far more approachable than kernel
  anti-cheat (Vanguard/EAC/BattlEye).
- **Packing / obfuscation (module entropy as a proxy):**
  - `npggNT64.des` ~3.2 MB — mostly plain (RE-able); in-game init/detection. Later found to carry a
    large code-virtualised (`.vlizer`) section, so its *init branch* is impractical to RE statically.
  - `npsc64.des` ~2.2 MB — mostly plain (RE-able); the scanner.
  - `GameMon64.des` ~13.7 MB — packed; the external monitor.
  - `GameGuard.des` ~2.4 MB — packed; the launcher.
- **The game embeds the whole GameGuard payload and self-extracts** `bin\GameGuard\` if missing —
  so deleting GameGuard files never removes it.
- **The game authenticates `GameGuard.des` before running it** — a stub/fake GameGuard binary can
  never pass (see error 124 below).

## The error codes we mapped (GameGuard startup failures)

These were the decision tree for what GameGuard would and wouldn't tolerate:

| Code | Meaning (observed) |
|---|---|
| **153** | GameGuard files missing → the game requires GameGuard; it re-extracts and refuses to run without it. You can't just delete it. |
| **124** | `GameGuard.des` failed authentication → stub/replaced GameGuard binaries can never pass. |
| **114** | A **path/identity check failed** → fired specifically when running a *copied* install from a non-registered location. (Details below.) |
| **116** | Duplicate execution → two `helldivers2.exe` running at once. Always kill all instances before launch. |
| **340 / 380** | Phone-home / network-stage failures observed when the backend was blocked before GameGuard was handled. |

## The "copy won't run" saga (error 114) — a documented dead end

An early plan was to run a **copied** install from another drive (to keep the Steam install
pristine and firewalled). The copy died with **error 114** even from a perfectly clean GameGuard
state. What we learned:

- 114 behaves like a **path/identity check**: GameGuard reads the running image path and compares it
  against the Steam-registered install path (from the registry); a mismatch fails init. The real
  install at its registered path runs; a copy at `D:\...` does not. Junction/symlink tricks don't
  help — Windows resolves them, so GameGuard still sees the real target path.
- We tried to **spoof the path** GameGuard reads by hooking the obvious Win32 APIs
  (`GetModuleFileNameW/A`, `GetModuleFileNameExW`, `QueryFullProcessImageNameW`,
  `GetMappedFileNameW`, `RegOpenKeyExW`, `CreateProcessW`) and even repointing the PEB
  `ImagePathName`/`CommandLine`. **All of it failed to move 114.** Logging every call showed **zero
  GameGuard callers** going through those user-mode APIs.
- **Conclusion:** GameGuard performs its identity check via **direct syscalls** (ntdll `Nt*` stubs
  or raw `syscall`), bypassing user-mode API hooks — a known GameGuard anti-hook technique. Beating
  114 on a copy would have needed **syscall-layer interception** plus handling GameGuard's
  self-integrity. That's a deeper multi-session RE effort.
- Two of our own tools came out of this and are **described but not shipped**: `gg_pathspoof`
  (the MinHook path-spoof that proved the APIs weren't the vector) and `earlybird` (an APC injector
  that loads a DLL into the process very early — it *did* load our DLL before GameGuard init, which
  validated the early-injection pipeline, but couldn't beat 114 by itself).

**The resolution was to stop fighting 114**: run from the **original registered install** (which
passes the path check) and neutralise GameGuard's *runtime* gate instead, on the understanding that
this is an offline, single-player, firewalled session — the Steam install's **files** are never
modified; only in-memory, runtime state is touched, and only for that session.

## The neutralisation approach (documented, not shipped)

The method that worked is the one a **community reverse-engineering artifact** implements — a
**user-mode, in-memory, atomic patcher**. We used that artifact; it is not our code and is not
included here. At the level of *what it does* (which is the archival value):

- The game's code sections are **ciphertext on disk** and only become real instructions **after the
  in-memory unpacker runs** (~0.4–1.4 s after launch). So any patch must happen in a **narrow
  window**: *after* unpacking, *before/around* the watchdogs getting strict.
- GameGuard's result is funneled through a small set of **status checks** in the game's
  "GameGuard wrapper" — a success status (`0x755` / 1877, `NPGAMEMON_SUCCESS`) lets the game proceed;
  anything else routes to a fatal error. The neutralisation forces those checks to read *success*,
  in memory, for that run.
- Patches are applied with **hardware-atomic compare-and-swap** (`LOCK CMPXCHG` / `CMPXCHG16B`)
  against a live-captured baseline, so a thread on another core can never execute a half-written
  instruction (no torn-instruction crash). Page protections are flipped with **direct NT syscalls**
  (to avoid the hooked user-mode layer) and the instruction cache is flushed afterward.
- The external **watchdog processes** are first enrolled into a Win32 **Job Object** (so they can be
  terminated cleanly and together), then isolated/terminated right before the handoff.
- In our workflow the whole thing was **gated on a flag file**: the game was allowed to boot with
  GameGuard **alive and happy** all the way to the ship, and only then (by dropping a
  `kill-gg.flag`) were the watchdogs isolated and the patches armed. That meant the fragile online
  checks were never in play; we only touched state once we were sitting safely offline on the ship.

Why no exact offsets or tool here: that combination (post-unpack atomic patch of the status gate +
watchdog isolation) is a general GameGuard defeat. It is specific to a build, but the **technique**
is reusable against other GameGuard-protected online titles, which is exactly what this archive
should not hand out. The findings above are enough to understand the history; the turnkey is not.

### What "failing fast" taught us

Naïve static patches (NOP-ing the error jumps, or stubbing the status call) made the game **exit in
seconds with `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN / fail-fast)** — the packer's/anti-tamper's
integrity watchdog catching modified bytes, or GameGuard-success code running without a real
GameGuard present. That's why the working approach is **atomic, post-unpack, watchdog-isolated, and
timed** rather than a simple byte patch. During VR development we saw the same `0xC0000409`
whenever our own hook patched game code **while GameGuard was still alive** — so our mod's camera
hook learned to **wait for the kill flag** before touching anything.

## Why this was ban-safe (the whole point)

- It happened **only on an offline, firewalled, single-player session**. There is no server, no
  match, no other player — nothing to cheat *at*.
- **No authenticated traffic ever reached Arrowhead** (chapter 02). The real backend has no record
  the session existed.
- **The Steam install's files were never modified.** GameGuard's own files were never modified on
  disk. The patches are **in-memory, runtime-only**, and vanish when the process exits.
- Before ever playing online again, the offline redirects are removed (hosts-online script) and the
  modded-session crash reports are moved out of the game's upload queue, so a later normal launch
  carries nothing from the offline work. See [`07-reproduce.md`](07-reproduce.md).

## Bottom line

On the GameGuard build, the critical unlock for all modding was **getting the offline build past
GameGuard's runtime gate without modifying any files and without any online exposure**. That gave a
running, injectable, moddable build — the foundation for everything in chapters 04–06. With the EAC
transition this entire chapter is historical; see [`08-eac-transition.md`](08-eac-transition.md).

# 06 — Live memory modding (stratagems, explosions, text) and the dashboard

Once a DLL could be injected (chapter 04), most of the fun modding was done by **reading and
writing the game's own memory** on the offline build. These edits are **live-memory only** — they
reset when the game restarts. Addresses are per-run (heap); what carries over is the **layout**,
which is why the mod engine re-discovers tables each launch by signature.

Source: [`../vrmod/mod/cheat_scan.cpp`](../vrmod/mod/cheat_scan.cpp) (in-game search/patch tool),
[`../vrmod/mod/lua_probe.cpp`](../vrmod/mod/lua_probe.cpp) (runs Lua inside the game),
[`../vrmod/mod/rename_label.cpp`](../vrmod/mod/rename_label.cpp) (in-memory text),
[`../dashboard/`](../dashboard/) (the control panel).

## The Lua probe (the single most useful tool)

[`lua_probe.cpp`](../vrmod/mod/lua_probe.cpp) hooks LuaJIT (`lua_pcall` + `luaL_loadbufferx`) and
runs a Lua file you drop on disk, **inside the game's own Lua state**. That gives you the Stingray
API from the inside: worlds, units, cameras, resources. Notes learned:

- Resources are **64-bit hashes**; `IdString64.from_hex` builds one from a hex string.
- Unit handles are small integers, read via FFI from the unit pointer.
- `Unit.world_position` **crashes** on a unit with 0 scene-graph nodes — guard with
  `num_scene_graph_items(u) > 0`.
- The gameplay camera is world 1, unit handle 5 (per-run, but stable within a session).

The Lua scripts we used are in [`../vrmod/lua/`](../vrmod/lua/): camera recon/tests, a unit monitor,
the first-person camera, and recon helpers.

## The attack table (stratagems and weapons)

Found 2026-10-18 while building a "Hellbomb Strafing Run" mod. The game keeps **attacks** (Eagle
stratagems, and weapon attacks) in a packed table of **0x110-byte rows**. It starts exactly at the
start of its own 128 KB allocation (481 rows fit), with the **Eagle Airstrike as row 2**.

### How it was found

- Recon in Lua logged the Eagle jet / bomb-model / ground-impact resource hashes for a live strike.
- Scanning memory for the **bomb-model hash** (`71e52f800636b336`) at row offset `+0xAC` located the
  table. Because the base changes every launch, the mod engine probes **allocation starts** for the
  bomb hash at row 2 `+0xAC` — **0.1 s** instead of a ~3-minute full scan.
- Individual rows were identified by giving each a distinct arrival-delay multiplier and reading the
  in-game ETA.

### Row layout (0x110 bytes) — known fields

| Offset | Type | Meaning |
|---|---|---|
| `+0x04` | int | category (2 = Eagle, 3 = 500kg-class) |
| `+0x2C` | int | stratagem / attack ID |
| `+0x30` | u64 | name hash |
| `+0x44` | float | **arrival delay** (scales ETA ~1:1) |
| `+0x50` | float | unknown (100 / 500) |
| `+0x68` | int | ID ref (flight-run settings?) |
| `+0xAC` | u64 | bomb model hash |
| `+0xBC` | int | **bomb body / projectile type** |
| `+0xC4` | float | **trigger** for the extra explosion (0 = never fires; 0.8 works) |
| `+0xC8` | int | **extra explosion ID** |

### Confirmed in game

- `+0x44` ×10 → airstrike ETA 52 s. (Editing a strike already in flight loses it.)
- Airstrike `+0xBC = 193, +0xC4 = 0.8, +0xC8 = 277` → **five 500kg blasts in a row**.
- Airstrike `+0xBC = 193, +0xC4 = 0.8, +0xC8 = 111` → a run of **Hellbomb** blasts ("Hellbomb
  Strafing Run").

### Weapons live in the same table

Found by giving groups of rows different explosions and firing the weapon (a three-way search:
500kg / Hellbomb / normal per shot):

| Weapon | Attack ID (`+0x2C`) | Model (`+0xAC`) | Body (`+0xBC`) |
|---|---|---|---|
| GP-31 Grenade Pistol | 263 | `a6cd8a90185b6567` (grenade) | 327 |
| AR-2 Coyote | 138 | none | 0 (its bullets are not world units) |

**`+0xC4` is the trigger:** at 0 the `+0xC8` explosion never fires; set it to 0.8 and the swap
works. A swapped explosion brings the blast and damage but **no sound** — the original weapon's
sound belongs to the weapon object, not its explosion entry.

## The projectile/explosion table

A separate table of **0x98-byte rows** (keyed by the same IDs) holds blast radii, found by
searching near the attack table for a known radius float.

| Explosion ID | Radii (inner/mid/outer) | Notes |
|---|---|---|
| 194 | 5 / 10 / 14 | airstrike bomb |
| 277 | 10 / 25 / 35 | 500kg blast (confirmed in game) |
| 102, 169 | ~7/22/40, ~8/25/35 | 500kg-class |
| 111, 125, 242 | 17 / 25 / 45 | identical triple — the three Hellbomb variants |
| 293 | 50 / 100 / 100 | the largest in the table |

**ID 111 is the Hellbomb explosion** (confirmed). IDs 180/181 produced no blast.

## In-memory text renames (sector / planet / item names)

Planet and sector names are localised **client-side from numeric IDs**, so the offline gateway
can't rename them (chapter 02). But the strings exist in memory, and you can rewrite them there:

- [`rename_label.cpp`](../vrmod/mod/rename_label.cpp) applies a rename table to live memory:
  null-terminated, **compacted in place** (the replacement can't be longer than the original, and
  you keep the terminator to avoid a wide gap).
- Pitfalls: writing with trailing spaces leaves a visible gap ("NOVA  SECTOR"); compact in place
  instead. A naïve memory scan hits your **own** DLL's string tables — skip your own module and
  allocations. Once you've renamed a string once, you know its exact address, so re-applies are
  cheap.

This is how sector/planet display names were changed offline — purely cosmetic, in-memory, local.

## The dashboard

[`../dashboard/`](../dashboard/) is a small Python backend (`server.py` + `mod_engine.py`) and an
HTML front end (`dashboard.html`) that exposes **every mod as a card**, with:

- **Every mod off by default.** You turn things on explicitly.
- A **Stop** button per action, and an engine that **re-discovers** the tables each launch (fast
  signature probe, with a slow full-scan fallback).
- **Originals persisted** per game PID, so a dashboard restart doesn't mistake already-modded memory
  for the game's own values.
- Mods included: the VR settings writer (writes `vr_config.txt`), the airstrike/500kg/GP-31/Coyote
  explosion swaps, Eagle arrival-delay, a stratagem renamer, sector/planet renames, planet
  description text, free-form custom renames, and a raw "explosion overrides" rule box
  (`attackId = explosionId [trigger]`).

The engine (`mod_engine.py`) does the `ReadProcessMemory`/`WriteProcessMemory`, enumerates writable
regions, scans with a cancellable search, and patches/restores with per-PID original bytes saved to
disk. It's a clean reference for "drive a game's memory from Python."

## Why all of this is offline-only

Every byte written here goes into a **single-player, offline, firewalled** process. None of it
persists, none of it is visible to any server, and the Steam install's files are never touched. On
an online build, writing game memory is exactly what anti-cheat exists to stop — so this stays where
it belongs.

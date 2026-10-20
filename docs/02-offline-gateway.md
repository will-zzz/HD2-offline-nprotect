# 02 — The offline server emulator (the "gateway")

This is the part that makes an online-only game boot and play with **no real servers**. It is the
most broadly reusable idea in the archive and the least anti-cheat-specific.

## The problem

Helldivers 2 is online-only. At boot it must reach several backends or it stalls after the intro
cinematic with **"Error 0, status -2"** and never reaches the menu. It then polls forever (friends
~every 8 s, operation/newsfeed ~every 50 s).

## The core trick: DNS redirect + a local fake backend

1. The **hosts file** maps every HD2 backend domain → `127.0.0.1`: Arrowhead, PlayFab, the Azure
   SignalR pubsub hosts, PSN/Sony, and the `*.wwsga.me` telemetry hosts. (Scripts in
   [`../scripts/`](../scripts/): `hosts-offline.ps1` adds them, `hosts-online.ps1` removes them,
   each backs up the hosts file and flushes DNS.)
2. A local **gateway** program ([`../gateway/gateway.cs`](../gateway/gateway.cs), a single-file C#
   app) listens on `127.0.0.1:443` (TLS) and `:80` with a multi-SAN certificate for those domains,
   and answers every request locally.

So the game *thinks* it is talking to Arrowhead / PlayFab / Sony — it is actually talking to a C#
program on your own PC. Proof: `netstat` shows `helldivers2` with connections only to `127.0.0.1`
and **zero** to the real internet.

### The backend domains (public endpoints, redirected to 127.0.0.1)

| Backend | Host | How it's answered |
|---|---|---|
| Arrowhead | `api.live.prod.thehelldiversgame.com` | fake login + captured real GET bodies; signed POSTs → empty `200` (non-blocking) |
| PlayFab | `<title-id>.playfabapi.com` | fake Steam→PlayFab login, profiles, title config |
| PubSub | Azure SignalR hosts (`pubsub-signalr-prod-*.service.signalr.net`) | local websocket handshake + a faked "subscribed" push |
| PSN / Sony | `pss-cloud.net` | **proxied to the real Sony** (platform layer; not account-critical) |
| Telemetry | `*.api.wwsga.me` | swallowed locally (`200`), never leaves the machine |

## The "-2" fix (the hard part, solved 2026-10-17)

The client does not just need *a* response from Arrowhead — it **strictly validates the shape** of
a chain of **GET** endpoints. Answering them with an empty `{}` or `[]` makes the client reject the
response, back off, and eventually land on `-2`. The real bodies have specific shapes — for
example FriendsV2/Block is
`{"totalEntries":0,"currentLimit":100,"currentOffset":0,"entries":[]}`, not `[]` and not a
`{code,status,data}` envelope.

The clean, deterministic solution:

1. Add a **capture mode** to the gateway. In this mode, Arrowhead GETs are **proxied to the real
   server** (forwarding the client's `authorization: session <token>` header — crucially, **GETs
   are not X-Signature-signed**), and each response is **saved** to a replay file.
2. Do a one-time **real login** so the client holds a real session; read that real session **GUID
   token** out of the GET headers in the gateway log.
3. With the token, hit the real server directly (e.g. `curl --resolve host:443:<real-ip>`) to fetch
   the **exact real body** for every polled GET, and save each into the gateway's `replay/` folder.
4. Switch all flags off and serve those captured bodies behind the **fake** login.

With real-shaped data for every GET the client polls, it is satisfied and boots to the ship.

### The endpoints that had to be real (served from captured replays)

FriendsV2/Block, FriendsV2/Request, FriendsV2/Recent, Operation, NewsFeed/801,
WarSeason/{WarId, WarInfo, Status, GalacticWarEffects, NewsTicker, timeSinceStart},
Configuration/GameClient, Stats/war/801/summary, Stats/profile[/801]/summary, SeasonPass,
Mission/RewardEntries, and the whole Progression chain (Progression, Progression/ItemPackages,
ProgressionPackages, items, levelspec, inventory, Items/Customize/v2).

> **Why the captured data is not in this repo:** those replay bodies were fetched from a live
> account session and contain account identifiers (friends list shape, progression, profile IDs).
> They are account data, not ours to publish. The **method** above lets anyone capture their own
> from their own session. A set of **empty-but-correctly-shaped** example skeletons would need to
> be rebuilt by hand per endpoint; we did not include fabricated ones to avoid shipping something
> that looks authoritative but isn't.

## Why it boots and why it's ban-safe

- Login is **fake** — the gateway returns a flat body with a `sessionId` string (any value; the
  client echoes it in `Authorization: session <v>` on later requests, and it is **not**
  cryptographically verified for GETs). No real authenticated session reaches Arrowhead.
- The endpoints that would **write** to the account (progress, rewards, purchases, monetisation)
  are **signed POSTs** carrying an `X-Signature` we cannot forge — and they are pointed at
  `127.0.0.1`, so they go nowhere. They return `DEFAULT {}` and are **non-blocking** (the ship
  loads fine without them; some UI widgets just show empty — dispatches/effects showed 0).
- Progression is **server-authoritative**. With no authenticated session, the real backend has no
  record the session happened; nothing persists upstream and nothing looks anomalous next time you
  play online normally.
- Steam is untouched (only playtime hours tick up). No multiplayer, no other players.

## Boot → mission data flow

```
launch → Steam (local) → PlayFab login (fake) → Arrowhead login (fake)
  → poll war / progression / friends / stats GETs (served from captured replays)
  → "PRESS ANY BUTTON" → ship → pick mission
  → drop (the mission runs entirely locally) → extract (reward POSTs go nowhere) → ship
```

Solo missions are hosted locally once launched (the normal game is hybrid-P2P with the squad
leader hosting), so a solo drop needs no server once it has started.

## The moddable layer (free-text, no code)

The gateway re-reads its data files **per request**, so edits take effect on the **next poll — no
restart**. Some *server-authored* fields are free text and render in-game:

- ✅ **Major Order** — the War Assignment body → `setting.overrideTitle` / `setting.overrideBrief`
  (proven to render in-game).
- ✅ **Personal Order** — the Player Assignment body (same fields).
- ✅ **News / dispatches** — the NewsFeed entries' `Message` field.
- ❌ **Not editable this way** — anything the **client** localises from numeric IDs: planet & sector
  names (`planetNameId32`, `sector`), item/stratagem/armour names, the scrolling NewsTicker. Those
  live in the game's own install files (which we never modify), so there is no server field to
  override. (Renaming them needed the in-memory string approach in
  [`06-memory-modding.md`](06-memory-modding.md).)
- ❌ **Not possible offline** — interactive, server-gated screens (Control Center, DSS). They need a
  live two-way session, so they throw "couldn't communicate with our server" no matter what.

## Operating modes (flag files)

The gateway switched behaviour via the presence of flag files in its folder:

- **(no flags)** = pure offline: fake login + served replays. ← the normal play mode.
- **`capture.flag`** = proxy Arrowhead GETs to the real server and **save** the bodies (how the
  replays were harvested; needs a live session + real token).
- **`passthrough.flag` / `rawpipe.flag`** = forward login to the real server (diagnostics only;
  these *do* talk to the real server, so they were used sparingly and never for play).

## Gotchas learned the hard way

- **Two game instances = GameGuard error 116.** Always kill every `helldivers2.exe` before
  launching.
- **PlayFab token expiry.** The captured PlayFab login's `TokenExpiration` is absolute; if it
  lapses, the game machine-gun-retries `LoginWithSteam` (stuck on "defrosting helldiver"). Fix by
  bumping the expiration far into the future in the captured login body.
- **External memory forensics is dead while GameGuard runs.** GameGuard hides game-private memory
  from outside readers (a full process dump yielded zero plaintext game strings). All the useful
  memory work had to be done **from inside** the process (injected), after GameGuard was handled.
- **The gateway certificate** must cover every redirected SAN, or the game's TLS validation fails
  and you get a different stall than `-2`.

See [`../gateway/README.md`](../gateway/README.md) for the gateway source and how to build/run it.

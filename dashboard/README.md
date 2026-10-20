# dashboard — the mod control panel

A small Python backend + HTML front end that exposes every memory mod as a card, with **every mod
off by default**, a Stop button per action, and an engine that re-discovers the game's tables each
launch by signature. Full explanation in
[`../docs/06-memory-modding.md`](../docs/06-memory-modding.md).

## Files

| File | What |
|---|---|
| `mod_engine.py` | The engine: `ReadProcessMemory`/`WriteProcessMemory`, writable-region enumeration, cancellable signature scan, the table knowledge (attack/explosion tables), patch/restore with per-PID original bytes, and the mod definitions. |
| `server.py` | A tiny HTTP server exposing `/api/mods`, toggle/apply/stop endpoints, and the gateway status. |
| `dashboard.html` | The UI: cards built from the mod schema (bool / select / int / float / text / textarea), Apply + Stop, engine bar, log. |
| `watch_mission.py`, `extract_watch.py` | Log-tailing helpers used during development (gateway/mission activity). |

## Run

```powershell
python server.py
# then open http://127.0.0.1:8777
```

The engine attaches to the running `helldivers2.exe` on the offline build. Mods include the VR
settings writer, the airstrike/500kg/GP-31/Coyote explosion swaps, Eagle arrival-delay, stratagem
and sector/planet renames (example values are neutral), planet description text, free-form custom
renames, and a raw explosion-override rule box.

> Memory modding like this is strictly for the offline, single-player build. On an online build it
> is exactly what anti-cheat stops. Keep it offline.

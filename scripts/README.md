# scripts

Small Windows helpers. Paths inside assume a project root of `C:\HD2VR` — adjust to taste. Run the
hosts scripts **as administrator**.

| Script | What |
|---|---|
| `hosts-offline.ps1` | Adds the Helldivers 2 backend domains → `127.0.0.1` so the game uses the local gateway. Backs up the hosts file, writes with retries (antivirus often locks it), verifies, flushes DNS. |
| `hosts-online.ps1` | Removes **only** the HD2 offline lines again (keeps everything else), with the same backup/retry/verify. Run this before playing the real game online. |
| `set-hosts.ps1` | The original atomic hosts setter used during development (preserves existing user entries). |
| `shot.ps1` | Screenshots the game window via `PrintWindow`, so it works even when the game is **not** foreground — essential for a headset-driven workflow. `-out <path>` to choose the file. |

The two `hosts-*.ps1` scripts are the online/offline switch: `hosts-offline.ps1` to go into the
offline sandbox, `hosts-online.ps1` to return to normal online play. Each one makes a timestamped
backup of your hosts file before changing it.

The backend domains these scripts manage are public (they're what the game connects to); redirecting
them to `127.0.0.1` is the whole offline trick (see `../docs/02-offline-gateway.md`).

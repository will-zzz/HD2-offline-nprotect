# gateway — the offline server emulator

`gateway.cs` is a single-file C# program that pretends to be the Helldivers 2 backends on
`127.0.0.1`, so the game boots and plays with no real-server contact. Full explanation in
[`../docs/02-offline-gateway.md`](../docs/02-offline-gateway.md).

## What it does

- Listens on `127.0.0.1:80` and `:443` with a multi-SAN TLS cert for the HD2 backend domains
  (you redirect those domains to `127.0.0.1` with the hosts scripts in `../scripts/`).
- Answers Arrowhead / PlayFab / PubSub(SignalR) / telemetry locally; proxies only PSN/Sony to the
  real platform.
- Serves captured real GET bodies from a `replay/` folder (not included — see below), with a fake
  login in front. Flag files (`capture.flag`, `passthrough.flag`, `rawpipe.flag`) switch modes.

## What you must supply yourself

- **A TLS certificate** covering the redirected SANs (the program loads one at start; generate your
  own self-signed multi-SAN cert and trust it locally).
- **Captured replay data** — the exact real GET bodies the client validates. These are **not** in
  this repo because they come from a live account session and contain account data. Capture your own
  with the gateway's capture mode (see the doc). Put them in a `replay/` folder beside the gateway.
- **Your own fake login body** — `arrowhead-login.json` loaded at startup. The embedded PlayFab
  login template already uses a neutral "John Helldiver" cover persona; the Steam ID in it is a
  placeholder (`76561190000000000`) — set your own if the client needs a matching one.

## Build & run (Windows, .NET)

```powershell
# compile (example; use your .NET SDK / csc of choice)
csc gateway.cs
# run elevated (needs to bind :80/:443 and uses the local cert)
.\gateway.exe
```

It logs every request; reading the log is how you learn what the client wants and how you capture
the session token for the one-time real-data capture.

> This is a private-server / single-player server emulator for a game you own. It never touches
> another player or a real match. Keep it offline.

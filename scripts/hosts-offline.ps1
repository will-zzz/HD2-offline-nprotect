# hosts-offline.ps1 — back to OFFLINE modding: re-adds the Helldivers 2 hosts lines that point the game
# at the local gateway (127.0.0.1), without touching anything else in the hosts file. Writes with
# retries (antivirus often holds the file open) and verifies. Run as administrator.
# Undo for normal online play: hosts-online.ps1
$ErrorActionPreference = 'Stop'
$hosts = 'C:\Windows\System32\drivers\etc\hosts'
$entries = @(
  '# HD2-VR Phase1 (game endpoints to local gateway)',
  '127.0.0.1 974A9.playfabapi.com',
  '127.0.0.1 api.live.prod.thehelldiversgame.com',
  '127.0.0.1 clientconfig.api.wwsga.me',
  '127.0.0.1 testament.api.wwsga.me',
  '127.0.0.1 crs.api.wwsga.me',
  '127.0.0.1 pubsub-signalr-prod-westus3-001.service.signalr.net',
  '127.0.0.1 pubsub-signalr-prod-westus-001.service.signalr.net',
  '127.0.0.1 pss-cloud.net',
  '127.0.0.1 dns-ir0-81e5-b3b9a58f-384e-45ff-b391-6355862e69ea.eastus.cloudapp.azure.com'
)
function Read-Retry($path) {
    for ($i = 0; $i -lt 20; $i++) { try { return [IO.File]::ReadAllLines($path) } catch { Start-Sleep -Milliseconds 300 } }
    throw "could not read $path"
}
$lines = @(Read-Retry $hosts)
$add = @($entries | Where-Object { $lines -notcontains $_ })
if ($add.Count) {
    $text = (($lines + $add) -join "`r`n") + "`r`n"
    $written = $false
    for ($i = 0; $i -lt 20 -and -not $written; $i++) {
        try { [IO.File]::WriteAllText($hosts, $text, [Text.Encoding]::ASCII); $written = $true } catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $written) { throw "could not write the hosts file (locked); run again" }
}
$check = @(Read-Retry $hosts)
$missing = @($entries | Where-Object { $check -notcontains $_ })
if ($missing.Count -or $check.Count -lt $lines.Count) { throw "verification failed: $($missing.Count) line(s) missing" }
ipconfig /flushdns | Out-Null
Write-Host "OK: added $($add.Count) line(s), $($check.Count) lines total. Helldivers 2 now goes to the local gateway (offline)."

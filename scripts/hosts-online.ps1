# hosts-online.ps1 — get ready for normal ONLINE Helldivers 2: removes only the offline-gateway lines
# (Helldivers/PlayFab/PubSub/crash-report hosts pointed at 127.0.0.1), keeps everything else
# (e.g. the fake-FitGirl blocklist), backs the hosts file up first and flushes DNS.
# If the hosts file is empty (an earlier failed write), it rebuilds from the newest non-empty backup.
# Writes with retries (antivirus often holds the file open) and verifies the result.
# Run as administrator. Undo for offline modding: hosts-offline.ps1
$ErrorActionPreference = 'Stop'
$hosts = 'C:\Windows\System32\drivers\etc\hosts'
$backups = 'C:\HD2VR\backups'
$hd2 = '974A9\.playfabapi\.com|api\.live\.prod\.thehelldiversgame\.com|clientconfig\.api\.wwsga\.me|testament\.api\.wwsga\.me|crs\.api\.wwsga\.me|pubsub-signalr-prod-[a-z0-9-]+\.service\.signalr\.net|pss-cloud\.net|dns-ir0-81e5-b3b9a58f-384e-45ff-b391-6355862e69ea\.eastus\.cloudapp\.azure\.com'

function Read-Retry($path) {
    for ($i = 0; $i -lt 20; $i++) {
        try { return [IO.File]::ReadAllLines($path) } catch { Start-Sleep -Milliseconds 300 }
    }
    throw "could not read $path (another program keeps it open)"
}

$lines = Read-Retry $hosts
if (-not $lines -or $lines.Count -eq 0) {
    $src = Get-ChildItem "$backups\hosts-*.bak" | Where-Object Length -gt 0 | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $src) { throw "hosts file is empty and there is no non-empty backup in $backups" }
    Write-Host "Hosts file is empty; rebuilding from $($src.FullName)"
    $lines = Read-Retry $src.FullName
} else {
    New-Item -ItemType Directory -Force $backups | Out-Null
    $backup = "$backups\hosts-$(Get-Date -Format yyyyMMdd-HHmmss).bak"
    [IO.File]::WriteAllLines($backup, $lines)
    Write-Host "Backup: $backup"
}

$keep = @($lines | Where-Object { -not ($_ -match "^\s*127\.0\.0\.1\s+($hd2)\s*$" -or $_ -match '^# HD2-VR Phase1') })
$text = ($keep -join "`r`n") + "`r`n"

$written = $false
for ($i = 0; $i -lt 20 -and -not $written; $i++) {
    try { [IO.File]::WriteAllText($hosts, $text, [Text.Encoding]::ASCII); $written = $true }
    catch { Start-Sleep -Milliseconds 500 }
}
if (-not $written) { throw "could not write the hosts file (another program keeps it locked); close antivirus scans and run again" }

$check = Read-Retry $hosts
$left = @($check | Where-Object { $_ -match $hd2 -and $_ -notmatch '^\s*#' })
if ($check.Count -ne $keep.Count -or $left.Count) {
    throw "verification failed: hosts has $($check.Count) lines (expected $($keep.Count)), $($left.Count) Helldivers line(s) left"
}
ipconfig /flushdns | Out-Null
Write-Host "OK: hosts has $($check.Count) lines, no Helldivers redirects left; other entries kept. Helldivers 2 will talk to the real servers."

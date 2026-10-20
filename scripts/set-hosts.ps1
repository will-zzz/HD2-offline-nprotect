# set-hosts.ps1 - reliably write the hosts file with the HD2 gateway block.
# Base = pristine backup (hosts.hd2backup, pre-HD2) so user's own entries (FitGirl etc.) survive.
# Idempotent: strips any previous HD2 block first. Run elevated.
$ErrorActionPreference = 'Stop'
$hosts  = "$env:WINDIR\System32\drivers\etc\hosts"
$bak    = "$env:WINDIR\System32\drivers\etc\hosts.hd2backup"
$marker = '# HD2-VR Phase1 (game endpoints to local gateway)'

$entries = @(
    '974A9.playfabapi.com',
    'api.live.prod.thehelldiversgame.com',
    'clientconfig.api.wwsga.me',
    'testament.api.wwsga.me',
    'crs.api.wwsga.me',
    'pubsub-signalr-prod-westus3-001.service.signalr.net',
    'pubsub-signalr-prod-westus-001.service.signalr.net',
    'pss-cloud.net',
    'dns-ir0-81e5-b3b9a58f-384e-45ff-b391-6355862e69ea.eastus.cloudapp.azure.com'
)

# base content: pristine backup > current file > empty
$base = @()
if (Test-Path $bak)       { $base = Get-Content $bak }
elseif (Test-Path $hosts) { $base = Get-Content $hosts }

# strip previous HD2 marker + any of our exact entries (whatever their state)
$rx = '127\.0\.0\.1\s+(' + (($entries | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')\s*$'
$keep = @($base | Where-Object { $_ -ne $marker -and $_ -notmatch $rx -and $_ -notmatch ('^\s*' + [regex]::Escape($marker)) })

$block = @($marker) + ($entries | ForEach-Object { "127.0.0.1 $_" })
$out = @($keep | Where-Object { $_ -ne '' }) + @('') + $block

# atomic write: temp file + move (GameGuard can lock hosts mid-write -> truncated file)
$tmp = "$env:WINDIR\System32\drivers\etc\hosts.hd2new"
Set-Content -Path $tmp -Value ($out -join "`r`n") -Encoding ASCII -Force
[IO.File]::Copy($tmp, $hosts, $true)
Remove-Item $tmp -Force -ErrorAction SilentlyContinue
ipconfig /flushdns | Out-Null

$verify = Get-Content $hosts
$lines  = ($verify | Measure-Object -Line).Lines
$found  = ($verify | Where-Object { $_ -match $rx }).Count
Write-Host "HOSTS-OK lines=$lines hd2entries=$found"
if ($found -ne $entries.Count) { Write-Host "HOSTS-VERIFY-FAILED (expected $($entries.Count) HD2 entries, found $found)"; exit 1 }

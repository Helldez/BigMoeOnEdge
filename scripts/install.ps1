# BigMoeOnEdge's command-line tools for this user on Windows, from the latest release: bmoe-cli,
# and bmoe-server with its web UI (docs/desktop-app.md). No administrator rights.
#
#   irm https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.ps1 | iex
#
# It reads the release's feed, picks the archive for this machine (x64 or ARM64), checks its size
# and SHA-256 against the feed, unpacks it into %LOCALAPPDATA%\Programs\bmoe-cli (replacing an
# earlier install) and puts that folder on the user's PATH. Run again to update. The feed's
# Ed25519 signature is checked by the server and by install.sh where OpenSSL 3 is present;
# PowerShell has no Ed25519 of its own, so here the feed is trusted as HTTPS delivered it, and the
# archive is held to the digest it names. Overrides: $env:BMOE_PREFIX, $env:BMOE_FEED_URL.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # the progress bar makes Invoke-WebRequest many times slower

$feedUrl = if ($env:BMOE_FEED_URL) { $env:BMOE_FEED_URL } else {
    'https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/feed.json'
}
$prefix = if ($env:BMOE_PREFIX) { $env:BMOE_PREFIX } else { Join-Path $env:LOCALAPPDATA 'Programs\bmoe-cli' }

# A 32-bit or emulated PowerShell reports the machine's own architecture in PROCESSOR_ARCHITEW6432.
$arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
$target = switch ($arch) {
    'AMD64' { 'windows-x86_64' }
    'ARM64' { 'windows-arm64' }
    default { throw "There is no release for Windows on $arch." }
}

Write-Host 'Reading the release feed...'
if (-not $feedUrl.StartsWith('https://')) { throw 'The feed URL must be https.' }
# Release assets come as application/octet-stream, which Windows PowerShell hands back as bytes.
$resp = Invoke-WebRequest -Uri $feedUrl -UseBasicParsing
$text = if ($resp.Content -is [byte[]]) { [Text.Encoding]::UTF8.GetString($resp.Content) } else { $resp.Content }
$feed = $text | ConvertFrom-Json
$entry = $feed.archives.$target
if (-not $entry) { throw "The latest release has no archive for $target." }
if (-not ([string]$entry.url).StartsWith('https://')) { throw "The feed's archive URL is not https." }
if ($entry.sha256 -notmatch '^[0-9a-f]{64}$') { throw "The feed's digest is not a SHA-256." }

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("bmoe-install-" + [guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
try {
    Write-Host "Downloading BigMoeOnEdge $($feed.version) for $target..."
    $zip = Join-Path $tmp 'archive.zip'
    Invoke-WebRequest -Uri $entry.url -OutFile $zip -UseBasicParsing
    $size = (Get-Item $zip).Length
    if ($size -ne [int64]$entry.size) { throw "The download is $size bytes, the feed says $($entry.size)." }
    $sha = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($sha -ne $entry.sha256) { throw "The download's SHA-256 is not the one the feed names." }

    $x = Join-Path $tmp 'x'
    Expand-Archive -Path $zip -DestinationPath $x
    $top = Get-ChildItem $x -Directory | Select-Object -First 1
    if (-not $top -or -not (Test-Path (Join-Path $top.FullName 'bmoe-cli.exe'))) {
        throw 'The archive does not hold bmoe-cli.exe.'
    }
    # The new copy beside the old, then swapped: a failure half-way leaves the old one working.
    $parent = Split-Path $prefix -Parent
    New-Item -ItemType Directory -Force $parent | Out-Null
    $new = "$prefix.new"; $old = "$prefix.old"
    foreach ($p in @($new, $old)) { if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
    Move-Item $top.FullName $new
    if (Test-Path $prefix) { Move-Item $prefix $old }
    Move-Item $new $prefix
    if (Test-Path $old) { Remove-Item -Recurse -Force $old }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$parts = @($userPath -split ';' | Where-Object { $_ })
if ($parts -notcontains $prefix) {
    [Environment]::SetEnvironmentVariable('Path', (@($parts) + $prefix) -join ';', 'User')
    $env:Path = "$env:Path;$prefix"
    Write-Host "Added $prefix to your PATH: new terminals find bmoe-cli and bmoe-server."
}

Write-Host "Installed in ${prefix}:"
foreach ($c in 'bmoe-cli', 'bmoe-server') {
    $exe = Join-Path $prefix "$c.exe"
    if (Test-Path $exe) { Write-Host "  $c $(& $exe --version)" }
}
Write-Host 'Start the web UI with: bmoe-server --open'

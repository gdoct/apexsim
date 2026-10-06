<#
.SYNOPSIS
    Deploy the ApexSim server (and its content) to a remote Docker host over ssh.

.DESCRIPTION
    Reads deploy/deploy.user.psd1 (gitignored; start from deploy/deploy.sample.psd1),
    builds the server and content images, gets them to the host (docker save over
    ssh, or a registry), renders a docker-compose.yml and server.toml from the
    settings, uploads them with your TLS certificate and runs `docker compose up`.

    Nothing in the settings file ends up in git, and the rendered files are
    written to build/deploy/ so you can see exactly what went to the host.

.PARAMETER Action
    deploy (default), status, logs, restart, stop.

.PARAMETER Config       Settings file (default deploy/deploy.user.psd1).
.PARAMETER SkipBuild    Reuse the images already built locally.
.PARAMETER SkipServer   Leave the server image alone (content / config only).
.PARAMETER SkipContent  Leave the content image and volume alone.
.PARAMETER DryRun       Validate and render to build/deploy/, touch nothing.

.EXAMPLE
    ./scripts/deploy_server.ps1 -DryRun
    ./scripts/deploy_server.ps1
    ./scripts/deploy_server.ps1 -SkipServer      # new content only
    ./scripts/deploy_server.ps1 -Action logs
#>
[CmdletBinding()]
param(
    [ValidateSet('deploy', 'status', 'logs', 'restart', 'stop')]
    [string]$Action = 'deploy',
    [string]$Config,
    [switch]$SkipBuild,
    [switch]$SkipServer,
    [switch]$SkipContent,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Config) { $Config = Join-Path $repo 'deploy\deploy.user.psd1' }

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Fail($text) { throw $text }

# ---- settings --------------------------------------------------------------

if (-not (Test-Path $Config)) {
    Fail "No settings file at $Config. Copy deploy\deploy.sample.psd1 to deploy\deploy.user.psd1 and edit it."
}
$fromFile = Import-PowerShellDataFile -Path $Config

$defaults = [ordered]@{
    Host = ''; User = ''; SshPort = 22; SshKey = ''; RemoteDir = '~/apexsim'; Project = 'apexsim'
    Transfer = 'save'; Registry = ''; ImageTag = 'latest'; ContentTag = ''
    TlsCert = ''; TlsKey = ''; TlsFromHost = ''; TlsSyncCalendar = '*-*-* 04:30:00'; RequireTls = $true
    AuthMode = 'dev'; Tokens = @()
    BindAddress = '0.0.0.0'; TcpPort = 9000; UdpPort = 9001; HealthPort = 9002; HealthBind = ''
    TickRateHz = 240; MaxSessions = 32; TelemetryDivisor = 4; LogLevel = 'info'
    RoadContact = 'mesh'; ShowcaseEnabled = $true
    ContainerUser = 'auto'; Restart = 'unless-stopped'; MemoryLimit = ''; Cpus = ''
    Env = @{}; ComposeExtra = @()
}
$s = @{}
foreach ($k in $defaults.Keys) { $s[$k] = $defaults[$k] }
foreach ($k in $fromFile.Keys) {
    if (-not $defaults.Contains($k)) { Fail "Unknown setting '$k' in $Config (see deploy.sample.psd1)." }
    $s[$k] = $fromFile[$k]
}

if (-not $s.Host) { Fail 'Host is required.' }
if ($s.Transfer -notin 'save', 'registry') { Fail "Transfer must be 'save' or 'registry'." }
if ($s.Transfer -eq 'registry' -and -not $s.Registry) { Fail "Transfer = 'registry' needs Registry." }
if ($s.AuthMode -notin 'dev', 'token') { Fail "AuthMode must be 'dev' or 'token'." }
if ($s.AuthMode -eq 'token' -and @($s.Tokens).Count -eq 0) { Fail "AuthMode = 'token' needs at least one entry in Tokens." }
if ($s.RoadContact -notin 'mesh', 'centerline') { Fail "RoadContact must be 'mesh' or 'centerline'." }
if ([bool]$s.TlsCert -ne [bool]$s.TlsKey) { Fail 'Give both TlsCert and TlsKey, or neither.' }
if ($s.TlsFromHost -and $s.TlsCert) { Fail 'Use TlsFromHost or TlsCert/TlsKey, not both.' }
$tls = [bool]$s.TlsCert -or [bool]$s.TlsFromHost
if ($s.TlsCert) {
    foreach ($f in $s.TlsCert, $s.TlsKey) { if (-not (Test-Path $f)) { Fail "Certificate file not found: $f" } }
} elseif ($s.AuthMode -eq 'token') {
    Write-Warning 'Token auth without TLS sends the tokens in plaintext.'
}
if ($s.ContainerUser -ne 'auto' -and $s.ContainerUser -notmatch '^\d+:\d+$') { Fail "ContainerUser must be 'auto' or 'uid:gid'." }
if ($s.Project -notmatch '^[a-z0-9][a-z0-9_-]*$') { Fail 'Project must be lowercase letters, digits, - and _.' }

$healthBind = $s.HealthBind
if (-not $healthBind) { $healthBind = $s.BindAddress }
$contentTag = $s.ContentTag
if (-not $contentTag) { $contentTag = Get-Date -Format 'yyyyMMdd-HHmm' }
$prefix = ''
if ($s.Registry) { $prefix = $s.Registry.TrimEnd('/') + '/' }
$serverImage = "${prefix}apexsim-server:$($s.ImageTag)"
if ($SkipContent -and -not $s.ContentTag) {
    # Not rebuilding: keep pointing at the content already shipped (newest local tag).
    $latest = & docker images "${prefix}apexsim-content" --format '{{.Tag}}' 2>$null | Where-Object { $_ -match '^\d{8}-\d{4}$' } | Sort-Object -Descending | Select-Object -First 1
    if ($latest) { $contentTag = $latest }
}
$contentImage = "${prefix}apexsim-content:$contentTag"

# Native tools (docker, ssh) write progress to stderr, which Windows PowerShell
# 5.1 turns into terminating errors under 'Stop'. Every native call below checks
# $LASTEXITCODE instead, so from here on errors are not fatal by themselves.
$ErrorActionPreference = 'Continue'

# ---- ssh helpers -----------------------------------------------------------

$target = $s.Host
if ($s.User) { $target = "$($s.User)@$($s.Host)" }
$sshArgs = @('-p', "$($s.SshPort)")
$scpArgs = @('-P', "$($s.SshPort)")
if ($s.SshKey) { $sshArgs += @('-i', $s.SshKey); $scpArgs += @('-i', $s.SshKey) }

function Invoke-Remote([string]$command) {
    & ssh @sshArgs $target $command
    if ($LASTEXITCODE -ne 0) { Fail "Remote command failed ($LASTEXITCODE): $command" }
}
function Get-Remote([string]$command) {
    $out = & ssh @sshArgs $target $command
    if ($LASTEXITCODE -ne 0) { Fail "Remote command failed ($LASTEXITCODE): $command" }
    return ($out -join "`n").Trim()
}

# The shell expands ~; scp wants the same place as a path relative to home.
$remote = $s.RemoteDir.TrimEnd('/')
$remoteScp = $remote
if ($remote -eq '~') { $remoteScp = '.' } elseif ($remote.StartsWith('~/')) { $remoteScp = $remote.Substring(2) }
$cd = "cd $remote"
$compose = "docker compose -p $($s.Project) -f docker-compose.yml"

# ---- the simple actions ----------------------------------------------------

if ($Action -ne 'deploy') {
    switch ($Action) {
        'status'  { Invoke-Remote "$cd && $compose ps" }
        'logs'    { Invoke-Remote "$cd && $compose logs --tail 200 -f apexsim" }
        'restart' { Invoke-Remote "$cd && $compose restart apexsim" }
        'stop'    { Invoke-Remote "$cd && $compose down" }
    }
    return
}

# ---- render ----------------------------------------------------------------

function Yaml([string]$v) { return "'" + $v.Replace("'", "''") + "'" }
function Toml([string]$v) { return '"' + $v.Replace('\', '\\').Replace('"', '\"') + '"' }

$outDir = Join-Path $repo 'build\deploy'
New-Item -ItemType Directory -Force $outDir -ErrorAction Stop | Out-Null

# server.toml: the container's config with auth filled in (the one thing the
# server has no environment override for).
$toml = Get-Content -Raw -ErrorAction Stop (Join-Path $repo 'server\server.docker.toml')
$tokenList = '[' + ((@($s.Tokens) | ForEach-Object { Toml $_ }) -join ', ') + ']'
$authMode = Toml $s.AuthMode
$toml = [regex]::Replace($toml, '(?m)^mode = "dev"', { param($m) "mode = $authMode" })
$toml = [regex]::Replace($toml, '(?m)^tokens = \[\]', { param($m) "tokens = $tokenList" })
[IO.File]::WriteAllText((Join-Path $outDir 'server.toml'), $toml.Replace("`r`n", "`n"))

$vars = [ordered]@{
    APEXSIM_SERVER_TICK_RATE_HZ       = "$($s.TickRateHz)"
    APEXSIM_SERVER_MAX_SESSIONS       = "$($s.MaxSessions)"
    APEXSIM_NETWORK_TELEMETRY_DIVISOR = "$($s.TelemetryDivisor)"
    APEXSIM_LOGGING_LEVEL             = $s.LogLevel
    APEXSIM_PHYSICS_ROAD_CONTACT      = $s.RoadContact
    APEXSIM_SHOWCASE_ENABLED          = "$($s.ShowcaseEnabled)".ToLower()
}
if ($tls) {
    $vars['APEXSIM_NETWORK_TLS_CERT_PATH'] = '/certs/fullchain.pem'
    $vars['APEXSIM_NETWORK_TLS_KEY_PATH']  = '/certs/privkey.pem'
    $vars['APEXSIM_NETWORK_REQUIRE_TLS']   = "$($s.RequireTls)".ToLower()
}
foreach ($k in $s.Env.Keys) { $vars[$k] = "$($s.Env[$k])" }

$y = New-Object System.Collections.Generic.List[string]
$y.Add('# Rendered by scripts/deploy_server.ps1 - edit deploy/deploy.user.psd1, not this.')
$y.Add('services:')
$y.Add('  content:')
$y.Add("    image: $(Yaml $contentImage)")
$y.Add('    restart: "no"')
$y.Add('    volumes:')
$y.Add('      - apexsim-content:/content')
$y.Add('  apexsim:')
$y.Add("    image: $(Yaml $serverImage)")
$y.Add("    container_name: $($s.Project)-server")
$y.Add("    restart: $(Yaml $s.Restart)")
$y.Add('    user: "${APEXSIM_UID}:${APEXSIM_GID}"')
$y.Add('    depends_on:')
$y.Add('      content:')
$y.Add('        condition: service_completed_successfully')
$y.Add('    ports:')
$y.Add("      - $(Yaml "$($s.BindAddress):$($s.TcpPort):9000/tcp")")
$y.Add("      - $(Yaml "$($s.BindAddress):$($s.UdpPort):9001/udp")")
$y.Add("      - $(Yaml "${healthBind}:$($s.HealthPort):9002/tcp")")
$y.Add('    environment:')
foreach ($k in $vars.Keys) { $y.Add("      ${k}: $(Yaml $vars[$k])") }
$y.Add('    volumes:')
$y.Add('      - apexsim-content:/content:ro')
$y.Add('      - ./data:/data')
$y.Add('      - ./server.toml:/etc/apexsim/server.toml:ro')
if ($tls) {
    $y.Add('      - ./certs/fullchain.pem:/certs/fullchain.pem:ro')
    $y.Add('      - ./certs/privkey.pem:/certs/privkey.pem:ro')
}
if ($s.MemoryLimit) { $y.Add("    mem_limit: $(Yaml $s.MemoryLimit)") }
if ($s.Cpus) { $y.Add("    cpus: $(Yaml $s.Cpus)") }
foreach ($line in @($s.ComposeExtra)) { if ($line) { $y.Add("    $line") } }
$y.Add('volumes:')
$y.Add('  apexsim-content:')
[IO.File]::WriteAllText((Join-Path $outDir 'docker-compose.yml'), (($y -join "`n") + "`n"))

Step 'Rendered build\deploy\docker-compose.yml and server.toml'
Write-Host "    server  $serverImage"
Write-Host "    content $contentImage"
Write-Host "    target  ${target}:$remote  (tls: $tls, auth: $($s.AuthMode), transfer: $($s.Transfer))"
if ($DryRun) { Write-Host 'Dry run: nothing built, sent or started.'; return }

# ---- build -----------------------------------------------------------------

if (-not $SkipBuild) {
    if (-not $SkipServer) {
        Step 'Building the server image'
        & docker build -t $serverImage (Join-Path $repo 'server')
        if ($LASTEXITCODE -ne 0) { Fail 'Server image build failed.' }
    }
    if (-not $SkipContent) {
        if (-not (Test-Path (Join-Path $repo 'build\showcase'))) { Fail 'build\showcase is missing: run scripts/initialize_content.ps1 first.' }
        Step 'Building the content image'
        Push-Location $repo
        try {
            & docker build -f server/Dockerfile.content -t $contentImage .
            if ($LASTEXITCODE -ne 0) { Fail 'Content image build failed.' }
        } finally { Pop-Location }
    }
}

# ---- ship ------------------------------------------------------------------

$images = @()
if (-not $SkipServer) { $images += $serverImage }
if (-not $SkipContent) { $images += $contentImage }

foreach ($img in $images) {
    if ($s.Transfer -eq 'registry') {
        Step "Pushing $img"
        & docker push $img
        if ($LASTEXITCODE -ne 0) { Fail "Push failed: $img" }
    } else {
        Step "Sending $img over ssh (docker save | docker load)"
        # cmd's pipe is binary-safe; PowerShell 5.1's would corrupt the tar.
        $sshCmd = 'ssh -C ' + (($sshArgs | ForEach-Object { '"' + $_ + '"' }) -join ' ') + " `"$target`" docker load"
        & cmd /c "docker save `"$img`" | $sshCmd"
        if ($LASTEXITCODE -ne 0) { Fail "Transfer failed: $img" }
    }
}

# ---- configure -------------------------------------------------------------

Step 'Uploading configuration'
Invoke-Remote "mkdir -p $remote/data $remote/certs"
& scp @scpArgs (Join-Path $outDir 'docker-compose.yml') (Join-Path $outDir 'server.toml') "${target}:$remoteScp/"
if ($LASTEXITCODE -ne 0) { Fail 'scp of the compose file failed.' }

if ($s.TlsCert) {
    & scp @scpArgs $s.TlsCert "${target}:$remoteScp/certs/fullchain.pem"
    if ($LASTEXITCODE -ne 0) { Fail 'scp of the certificate failed.' }
    & scp @scpArgs $s.TlsKey "${target}:$remoteScp/certs/privkey.pem"
    if ($LASTEXITCODE -ne 0) { Fail 'scp of the key failed.' }
    Invoke-Remote "chmod 700 $remote/certs && chmod 600 $remote/certs/privkey.pem && chmod 644 $remote/certs/fullchain.pem"
}

# ---- run -------------------------------------------------------------------

$uidgid = $s.ContainerUser
if ($uidgid -eq 'auto') { $uidgid = Get-Remote 'echo "$(id -u):$(id -g)"' }
$uid, $gid = $uidgid.Split(':')

if ($s.TlsFromHost) {
    # A certificate another program already keeps renewed on the host (certbot,
    # Nginx Proxy Manager). Its files are root-only, so a root job copies them
    # next to the service, owned by the container's user, and restarts the
    # server when they change (it reads the certificate once, at start).
    Step "Installing the certificate sync for $($s.TlsFromHost)"
    $src = $s.TlsFromHost.TrimEnd('/')
    # root's shell must not expand ~ to root's home: use the absolute path
    $abs = Get-Remote "cd $remote && pwd"
    $sync = @"
#!/bin/sh
# Rendered by scripts/deploy_server.ps1. Runs as root (systemd timer).
set -eu
SRC='$src'
DEST='$abs/certs'
mkdir -p "`$DEST"
changed=0
for f in fullchain privkey; do
  if ! cmp -s "`$SRC/`$f.pem" "`$DEST/`$f.pem"; then
    install -m 600 -o $uid -g $gid "`$SRC/`$f.pem" "`$DEST/`$f.pem.new"
    mv "`$DEST/`$f.pem.new" "`$DEST/`$f.pem"
    changed=1
  fi
done
if [ "`$changed" = 1 ]; then
  echo "certificate updated"
  if docker inspect $($s.Project)-server >/dev/null 2>&1; then docker restart $($s.Project)-server; fi
fi
"@
    $unit = @"
[Unit]
Description=Copy the renewed certificate to the ApexSim server

[Service]
Type=oneshot
ExecStart=/bin/sh $abs/sync-cert.sh
"@
    $timer = @"
[Unit]
Description=Daily certificate sync for the ApexSim server

[Timer]
OnCalendar=$($s.TlsSyncCalendar)
Persistent=true
RandomizedDelaySec=15min

[Install]
WantedBy=timers.target
"@
    foreach ($pair in @(@('sync-cert.sh', $sync), @("$($s.Project)-cert-sync.service", $unit), @("$($s.Project)-cert-sync.timer", $timer))) {
        [IO.File]::WriteAllText((Join-Path $outDir $pair[0]), $pair[1].Replace("`r`n", "`n"))
    }
    & scp @scpArgs (Join-Path $outDir 'sync-cert.sh') (Join-Path $outDir "$($s.Project)-cert-sync.service") (Join-Path $outDir "$($s.Project)-cert-sync.timer") "${target}:$remoteScp/"
    if ($LASTEXITCODE -ne 0) { Fail 'scp of the certificate sync failed.' }
    Invoke-Remote ("sudo -n sh -c 'cp $abs/$($s.Project)-cert-sync.service $abs/$($s.Project)-cert-sync.timer /etc/systemd/system/ " +
                   "&& systemctl daemon-reload && systemctl enable --now $($s.Project)-cert-sync.timer' " +
                   "&& sudo -n sh $abs/sync-cert.sh && chmod 700 $remote/certs")
}
Invoke-Remote "printf 'APEXSIM_UID=%s\nAPEXSIM_GID=%s\n' $uid $gid > $remote/.env"

Step 'Starting the stack'
if ($s.Transfer -eq 'registry') { Invoke-Remote "$cd && $compose pull" }
Invoke-Remote "$cd && $compose up -d --remove-orphans"

Step 'Waiting for the server to report healthy'
$wait = "for i in `$(seq 1 45); do st=`$(docker inspect -f '{{.State.Health.Status}}' $($s.Project)-server 2>/dev/null); " +
        "[ `"`$st`" = healthy ] && echo healthy && exit 0; sleep 2; done; echo `"status: `$st`"; exit 1"
& ssh @sshArgs $target $wait
if ($LASTEXITCODE -ne 0) {
    Invoke-Remote "$cd && $compose logs --tail 40 apexsim"
    Fail 'The server did not become healthy (log above).'
}
Write-Host "Deployed to $($s.Host): TCP $($s.TcpPort), UDP $($s.UdpPort)." -ForegroundColor Green

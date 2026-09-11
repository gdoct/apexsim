<#
.SYNOPSIS
    Play ApexSim straight from the editor build, without packaging.

.DESCRIPTION
    Runs the game through UnrealEditor.exe with -game, on the ApexSimEditor
    binaries in game-unreal/Binaries. There is no cook: a C++ change is one
    incremental build away from playable, where the standalone package takes a
    full BuildCookRun. The first launch after an engine or material change
    compiles shaders and runs slowly for a minute; later launches do not.

    If game-unreal/settings.yml points at this machine and nothing is
    listening on that port, a local server is started in its own minimized
    window, from server/target/release after an incremental
    `cargo build --release`, so it always matches the source.

    A server that is already running is used as it is. It may be an older
    build than the source: the script warns when it is, and -RestartServer
    replaces it.

.PARAMETER Build
    Build the ApexSimEditor target (Win64 Development) before playing. Close
    the editor and any running game first: Live Coding and a running game both
    hold the binaries the build replaces.

.PARAMETER NoServer
    Do not start a local server, e.g. to play on someone else's.

.PARAMETER RestartServer
    Stop the apexsim-server listening on the client's port and start a freshly
    built one.

.PARAMETER Log
    Open the log console window next to the game.

.PARAMETER CreateShortcut
    Put an "ApexSim (editor build)" shortcut on the desktop that runs this
    script, then exit without launching anything.

.PARAMETER DryRun
    Say what would be built, started and launched, and do none of it.

.PARAMETER Interactive
    Ask instead of warning: offer to build when the source is newer than the
    editor build and to restart a server older than the source, and keep the
    window open on an error. The desktop shortcut passes this, since its
    console closes the moment the script ends.

.PARAMETER EngineRoot
    Unreal Engine install directory (the folder containing Engine/). Falls back
    to $env:UE and the .uproject's EngineAssociation (see lib/ApexEngine.ps1).

.PARAMETER GameArguments
    Anything else is passed to the game, e.g. -ApexView=chase.

.EXAMPLE
    ./scripts/play_editor.ps1
    ./scripts/play_editor.ps1 -Build -RestartServer
    ./scripts/play_editor.ps1 -CreateShortcut
#>
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$NoServer,
    [switch]$RestartServer,
    [switch]$Log,
    [switch]$CreateShortcut,
    [switch]$DryRun,
    [switch]$Interactive,
    [string]$EngineRoot,
    [Parameter(ValueFromRemainingArguments)][string[]]$GameArguments
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

trap {
    Write-Host "ERROR: $_" -ForegroundColor Red
    if ($Interactive) { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}

# Yes/no question for -Interactive; anything but an explicit answer takes the default.
function Confirm-Step([string]$Question, [bool]$Default) {
    $hint = if ($Default) { '[Y/n]' } else { '[y/N]' }
    $answer = Read-Host "$Question $hint"
    if ($answer -match '^[yY]') { return $true }
    if ($answer -match '^[nN]') { return $false }
    return $Default
}
. (Join-Path $PSScriptRoot 'lib\ApexEngine.ps1')

$RepoRoot  = Split-Path $PSScriptRoot -Parent
$Uproject  = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
$ServerDir = Join-Path $RepoRoot 'server'
$ServerExe = Join-Path $ServerDir 'target\release\apexsim-server.exe'
$Settings  = Join-Path $RepoRoot 'game-unreal\settings.yml'

$Engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
    -Requires 'Engine\Binaries\Win64\UnrealEditor.exe'
$EditorExe = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor.exe'

function Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }

# --- Shortcut -----------------------------------------------------------------

if ($CreateShortcut) {
    $link = Join-Path ([Environment]::GetFolderPath('Desktop')) 'ApexSim (editor build).lnk'
    Step "Creating $link"
    if (-not $DryRun) {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($link)
        $shortcut.TargetPath = (Get-Command powershell.exe).Source
        $shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Interactive"
        $shortcut.WorkingDirectory = $RepoRoot
        $shortcut.IconLocation = "$EditorExe,0"
        $shortcut.Description = 'Play ApexSim from the editor build (starts a local server if needed)'
        $shortcut.Save()
    }
    return
}

# --- Where the client will connect -------------------------------------------

# settings.yml is what the game reads at startup; only a server on this machine
# is ours to start. The file does not exist until the game has run once, and
# the game then defaults to 127.0.0.1:9000.
$serverHost = '127.0.0.1'
$serverPort = 9000
if (Test-Path $Settings) {
    $text = Get-Content $Settings -Raw
    if ($text -match '(?m)^\s+host:\s*(\S+)') { $serverHost = $Matches[1] }
    if ($text -match '(?m)^\s+port:\s*(\d+)') { $serverPort = [int]$Matches[1] }
}
$isLocal = $serverHost -in @('127.0.0.1', 'localhost', '::1')

function Get-Listener([int]$Port) {
    $connection = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($connection) { Get-Process -Id $connection.OwningProcess -ErrorAction SilentlyContinue }
}

# Newest input to a build, to tell whether a running binary predates the source.
function Get-NewestWrite([string[]]$Paths) {
    Get-ChildItem $Paths -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty LastWriteTime
}

# --- Client build -------------------------------------------------------------

if (-not $Build) {
    $dll = Join-Path $RepoRoot 'game-unreal\Binaries\Win64\UnrealEditor-ApexSim.dll'
    $source = Get-NewestWrite (Join-Path $RepoRoot 'game-unreal\Source')
    if (-not (Test-Path $dll)) {
        Step 'No editor build yet; building one'
        $Build = $true
    }
    elseif ($source -and $source -gt (Get-Item $dll).LastWriteTime) {
        $message = 'game-unreal/Source has changed since the last editor build.'
        if ($Interactive) {
            $Build = Confirm-Step "$message Build it now (a minute or two)?" $true
        }
        else {
            Write-Warning "$message Pass -Build to include those changes."
        }
    }
}

if ($Build) {
    $running = Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*ApexSim.uproject*' }
    if ($running) {
        throw 'ApexSim is open in the editor or as a game; close it first (it holds the binaries the build replaces).'
    }
    Step 'Building ApexSimEditor (Win64 Development)'
    if (-not $DryRun) {
        & (Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat') ApexSimEditor Win64 Development "-Project=$Uproject" -WaitMutex
        if ($LASTEXITCODE -ne 0) { throw "client build failed (exit code $LASTEXITCODE)" }
    }
}

# --- Server -------------------------------------------------------------------

if (-not $NoServer -and $isLocal) {
    $listener = Get-Listener $serverPort

    if ($listener -and -not $RestartServer) {
        $newest = Get-NewestWrite @((Join-Path $ServerDir 'src'), (Join-Path $RepoRoot 'content'))
        if ($newest -and $listener.StartTime -lt $newest) {
            $message = "The server on port $serverPort was started $($listener.StartTime), before the latest server or content change."
            if ($Interactive) {
                $RestartServer = Confirm-Step "$message Restart it with a current build?" $true
            }
            else {
                Write-Warning "$message Pass -RestartServer to run a current build."
            }
        }
    }

    if ($listener -and $RestartServer) {
        if ($listener.ProcessName -ne 'apexsim-server') {
            throw "port $serverPort belongs to $($listener.ProcessName) (pid $($listener.Id)), not an ApexSim server; not stopping it."
        }
        Step "Stopping the server on port $serverPort (pid $($listener.Id))"
        if (-not $DryRun) {
            Stop-Process -Id $listener.Id -Force
            Start-Sleep -Seconds 1
        }
        $listener = $null
    }

    if ($listener) {
        Step "Using the server already on port $serverPort ($($listener.Path))"
    }
    else {
        Step 'Building the server (cargo build --release)'
        if (-not $DryRun) {
            Push-Location $ServerDir
            try {
                cargo build --release
                if ($LASTEXITCODE -ne 0) { throw "server build failed (exit code $LASTEXITCODE)" }
            }
            finally { Pop-Location }
        }

        Step "Starting the server on port $serverPort"
        if (-not $DryRun) {
            # server.toml binds 9000; follow settings.yml if it names another port.
            if ($serverPort -ne 9000) { $env:APEXSIM_NETWORK_TCP_BIND = "0.0.0.0:$serverPort" }
            # Its own window, so its log is there to read, and working directory
            # server/, where server.toml and ../content resolve.
            Start-Process -FilePath $ServerExe -WorkingDirectory $ServerDir -WindowStyle Minimized
            $deadline = (Get-Date).AddSeconds(30)
            while (-not (Get-Listener $serverPort)) {
                if ((Get-Date) -gt $deadline) { throw "the server did not start listening on port $serverPort" }
                Start-Sleep -Milliseconds 500
            }
        }
    }
}

# --- Game ---------------------------------------------------------------------

$launch = @("`"$Uproject`"", '-game')
if ($Log) { $launch += '-log' }
if ($GameArguments) { $launch += $GameArguments }

Step "Launching the game ($serverHost`:$serverPort)"
if ($DryRun) {
    Write-Host "    $EditorExe $($launch -join ' ')"
}
else {
    Start-Process -FilePath $EditorExe -ArgumentList $launch -WorkingDirectory $RepoRoot | Out-Null
}

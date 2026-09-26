<#
.SYNOPSIS
    Build every part of ApexSim and assemble a publishable release package.

.DESCRIPTION
    Runs the whole pipeline front to back and lays the results out as a folder
    a player can unzip and run:

        1. preflight  - the car and track data the build consumes must exist
        2. server     - cargo build --release -> apexsim-server.exe
        3. props      - ApexPropImport, the authored prop kit into /Game/Props
        4. tracks     - scripts/build_track_levels.ps1 (bake + import levels);
                        with -RuntimeTracks, bake only and ApexMaterialBake
        5. catalog    - build_track_catalog.py + ApexTrackCatalogSync, so the
                        track picker has names, metadata and preview art
        5. client     - scripts/build_game_standalone.ps1 (BuildCookRun)
        6. assemble   - server binary, server.toml and the content the server
                        reads at runtime, plus launchers and a manifest
        7. zip        - optional, with -Zip

    The package lands under artifacts/, which .gitignore already excludes, so
    nothing here is ever committed.

        artifacts/release/ApexSim-<Version>-Win64/
            Play.bat            start the server (if needed) and the game
            Start-Server.bat    server only, for hosting
            README.txt
            LICENSE
            release.json        version, commit, configuration, contents
            Game/               the packaged client + settings.sample.yml
            Game/Tracks/        -RuntimeTracks only: every circuit as its
                                export (.uescene.json + .uemesh + .png),
                                built by the game when it is raced
            Server/             apexsim-server.exe + server.toml + content/

    Every stage can be skipped so a broken piece does not block the rest; a
    skipped stage still has to find the output it would have produced, or the
    run aborts rather than shipping a package with a hole in it.

.PARAMETER Version
    Version stamped on the folder, the zip and release.json. Defaults to
    ProjectVersion in game-unreal/Config/DefaultGame.ini.

.PARAMETER Configuration
    Client build configuration. Shipping is the default for a release;
    Development is useful when you want the console and logs.

.PARAMETER EngineRoot
    Unreal Engine install directory (the folder containing Engine/). Falls back
    to $env:UE, $env:UE_ROOT, the registry entry for the .uproject's
    EngineAssociation, and then the default launcher install locations.

.PARAMETER OutputDirectory
    Directory that receives the versioned release folder. Defaults to
    artifacts/release.

.PARAMETER BuildEditor
    Compile the ApexSimEditor target before baking tracks. Needed after C++
    changes under game-unreal/Source; skip it for a content-only release.

.PARAMETER Zip
    Also compress the finished package to <OutputDirectory>/<name>.zip, ready
    to attach to a GitHub release.

.PARAMETER SkipServer
    Reuse the apexsim-server.exe already in server/target/release.

.PARAMETER SkipProps
    Reuse the prop kit already imported under game-unreal/Content/Props.

.PARAMETER SkipTracks
    Reuse the track levels already under game-unreal/Content/Tracks (or, with
    -RuntimeTracks, the exports already under content/tracks/export).

.PARAMETER RuntimeTracks
    Ship the circuits as data the game builds at runtime
    (docs/RUNTIME_CONTENT_LOADING.md) instead of cooked levels: the bake stops
    before the Unreal import, the shared track materials are baked with
    ApexMaterialBake so the cook carries them, and every export is copied into
    Game/Tracks. A circuit added later needs only its three files dropped in
    there. Levels already imported under game-unreal/Content/Tracks are still
    cooked and still preferred by the game (apexsim.track.Source auto); delete
    that folder first for a package that is runtime tracks only.

.PARAMETER SkipCatalog
    Reuse the DT_TrackCatalog rows and preview textures already imported.

.PARAMETER SkipClient
    Reuse a packaged client instead of running BuildCookRun. Uses the build
    already staged in this release's Game/ folder if present, otherwise falls
    back to -ClientArtifactDirectory (the output of build_game_standalone.ps1).
    Handy while the Unreal client is mid-refactor and will not compile.

.PARAMETER ClientArtifactDirectory
    Fallback client build for -SkipClient when the release folder does not
    already have one staged. Defaults to artifacts\ApexSim-Win64\Windows, the
    default output of build_game_standalone.ps1.

.EXAMPLE
    ./scripts/build_release.ps1 -Zip
    Full pipeline, Shipping client, ready-to-upload zip.

.EXAMPLE
    ./scripts/build_release.ps1 -SkipTracks -SkipCatalog -Configuration Development
    Rebuild the server and client only, against the content already imported.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [ValidateSet('DebugGame', 'Development', 'Shipping', 'Test')]
    [string]$Configuration = 'Shipping',
    [string]$EngineRoot,
    [string]$OutputDirectory,
    [switch]$BuildEditor,
    [switch]$Zip,
    [switch]$SkipServer,
    [switch]$SkipProps,
    [switch]$SkipTracks,
    [switch]$RuntimeTracks,
    [switch]$SkipCatalog,
    [switch]$SkipClient,
    [string]$ClientArtifactDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot     = Split-Path -Parent $PSScriptRoot
$Uproject     = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
$CarsDir      = Join-Path $RepoRoot 'content\cars'
$TrackDir     = Join-Path $RepoRoot 'content\tracks\real'
$LevelDir     = Join-Path $RepoRoot 'game-unreal\Content\Tracks'
$ExportDir    = Join-Path $RepoRoot 'content\tracks\export'
$MaterialsDir = Join-Path $RepoRoot 'game-unreal\Content\Materials\Track'
$PropsSrcDir  = Join-Path $RepoRoot 'content\props'
$PropsDir     = Join-Path $RepoRoot 'game-unreal\Content\Props'
$CatalogAsset = Join-Path $RepoRoot 'game-unreal\Content\Data\DT_TrackCatalog.uasset'
$ServerExe    = Join-Path $RepoRoot 'server\target\release\apexsim-server.exe'
if (-not $ClientArtifactDirectory) {
    $ClientArtifactDirectory = Join-Path $RepoRoot 'artifacts\ApexSim-Win64\Windows'
}

. (Join-Path $PSScriptRoot 'lib\ApexEngine.ps1')

function Write-Step {
    param([string]$Message)
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Detail {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor DarkGray
}

# Runs $Exe from $WorkingDir and turns a non-zero exit code into a terminating
# error, so a failed stage never silently feeds a stale one downstream.
function Invoke-Tool {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [string]$WorkingDir = $RepoRoot,
        [string]$What
    )

    Write-Detail "$Exe $($Arguments -join ' ')"
    Push-Location $WorkingDir
    try {
        & $Exe @Arguments
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($code -ne 0) {
        throw "$What failed with exit code $code"
    }
}

function Format-Size {
    param([double]$Bytes)
    if ($Bytes -ge 1GB) { return '{0:N1} GB' -f ($Bytes / 1GB) }
    return '{0:N0} MB' -f ($Bytes / 1MB)
}

function Write-TextFile {
    param([string]$Path, [string]$Content)
    [IO.File]::WriteAllText($Path, $Content, (New-Object Text.UTF8Encoding($false)))
}

function Test-Command {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

# Both return an array PowerShell will unroll to $null when empty, so callers
# wrap the call in @() before touching .Count.
function Get-CarFiles {
    if (-not (Test-Path $CarsDir)) { return @() }
    return @(Get-ChildItem $CarsDir -Filter 'car.toml' -Recurse -File)
}

function Get-TrackFiles {
    if (-not (Test-Path $TrackDir)) { return @() }
    return @(Get-ChildItem $TrackDir -Filter '*.yaml' -File)
}

# Tracks whose runtime export (manifest and mesh blob) is not on disk.
function Get-MissingExports {
    return @(Get-TrackFiles | ForEach-Object { $_.BaseName } | Where-Object {
        -not (Test-Path (Join-Path $ExportDir "$_.uescene.json")) -or
        -not (Test-Path (Join-Path $ExportDir "$_.uemesh"))
    })
}

# The runtime tracks, as the game looks for them: manifest, blob and the
# catalog preview beside each other in Game\Tracks.
function Copy-RuntimeTracks {
    param([string]$Destination)
    if (Test-Path $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $noPreview = [Collections.Generic.List[string]]::new()
    foreach ($track in Get-TrackFiles) {
        $stem = $track.BaseName
        foreach ($file in @("$stem.uescene.json", "$stem.uemesh")) {
            Copy-Item -LiteralPath (Join-Path $ExportDir $file) -Destination $Destination -Force
        }
        $preview = Join-Path $ExportDir "previews\$stem.png"
        if (Test-Path -LiteralPath $preview) {
            Copy-Item -LiteralPath $preview -Destination (Join-Path $Destination "$stem.png") -Force
        } else {
            $noPreview.Add($stem)
        }
    }
    if ($noPreview.Count -gt 0) {
        Write-Warning ("no preview for: {0} (run build_track_catalog.py; the picker shows placeholder art)" -f ($noPreview -join ', '))
    }
}

# ---------------------------------------------------------------------------
# Preflight
#
# Everything downstream is expensive, so the content the pipeline consumes is
# checked first and the run aborts with one list of what is missing rather
# than failing halfway through a long cook.
# ---------------------------------------------------------------------------
function Invoke-Preflight {
    $problems = [System.Collections.Generic.List[string]]::new()

    if (-not (Test-Path $Uproject)) {
        $problems.Add("no Unreal project at $Uproject")
    }
    if (-not (Test-Path (Join-Path $RepoRoot 'server\Cargo.toml'))) {
        $problems.Add("no Rust server at $(Join-Path $RepoRoot 'server')")
    }

    $cars = @(Get-CarFiles)
    if ($cars.Count -eq 0) {
        $problems.Add("no car data: expected at least one car.toml under $CarsDir")
    }

    $tracks = @(Get-TrackFiles)
    if ($tracks.Count -eq 0) {
        $problems.Add("no track data: expected at least one .yaml under $TrackDir")
    }
    else {
        # The catalog is keyed by track_id. Without one the server mints a
        # fresh UUID on every start, so no picker row can ever match and the
        # circuit ships nameless and previewless.
        $unidentified = @($tracks | Where-Object {
            -not (Select-String -LiteralPath $_.FullName -Pattern '^\s*track_id\s*:\s*\S' -Quiet)
        } | ForEach-Object { $_.BaseName })
        if ($unidentified.Count -gt 0) {
            $problems.Add("track(s) without a track_id, which the catalog needs: $($unidentified -join ', ')")
        }
    }

    if (-not $SkipServer -and -not (Test-Command 'cargo')) {
        $problems.Add('cargo is not on PATH; install Rust or pass -SkipServer')
    }
    if ($SkipServer -and -not (Test-Path $ServerExe)) {
        $problems.Add("-SkipServer, but there is no prebuilt server at $ServerExe")
    }
    if (-not $SkipTracks -and -not (Test-Command 'cargo')) {
        $problems.Add('cargo is not on PATH and the track bake needs it; install Rust or pass -SkipTracks')
    }
    if (-not $SkipCatalog -and -not (Test-Command 'python')) {
        $problems.Add('python is not on PATH and the track catalog needs it; install it or pass -SkipCatalog')
    }
    if ($SkipCatalog -and -not (Test-Path $CatalogAsset)) {
        $problems.Add("-SkipCatalog, but DT_TrackCatalog has never been synced ($CatalogAsset)")
    }

    # The levels reference the kit by path; a skipped import has to find it
    # already there, or every barrier and stand ships as a generated box.
    if ($SkipProps) {
        $imported = @(Get-ChildItem $PropsDir -Filter 'SM_*.uasset' -Recurse -File -ErrorAction SilentlyContinue)
        if ($imported.Count -eq 0) {
            $problems.Add("-SkipProps, but no prop meshes have been imported under $PropsDir")
        }
    }
    elseif (-not (Test-Path $PropsSrcDir)) {
        $problems.Add("no prop kit at $PropsSrcDir; pass -SkipProps to build without it")
    }

    # A packaged build with no track levels races in an empty world, so a
    # skipped bake has to prove the levels (or the exports) are already there.
    if ($SkipTracks -and $RuntimeTracks -and $tracks.Count -gt 0) {
        $missing = @(Get-MissingExports)
        if ($missing.Count -gt 0) {
            $problems.Add("-SkipTracks -RuntimeTracks, but these tracks have no export under ${ExportDir}: $($missing -join ', ')")
        }
        if (-not (Test-Path (Join-Path $MaterialsDir 'M_ApexTrackBase.uasset'))) {
            $problems.Add("-SkipTracks -RuntimeTracks, but the track materials were never baked ($MaterialsDir); run -run=ApexMaterialBake")
        }
    }
    elseif ($SkipTracks -and $tracks.Count -gt 0) {
        $missing = @($tracks | ForEach-Object { $_.BaseName } | Where-Object {
            -not (Test-Path (Join-Path $LevelDir "$_\L_$_.umap"))
        })
        if ($missing.Count -gt 0) {
            $problems.Add("-SkipTracks, but these tracks have no level under ${LevelDir}: $($missing -join ', ')")
        }
    }

    if ($problems.Count -gt 0) {
        $lines = ($problems | ForEach-Object { "  - $_" }) -join [Environment]::NewLine
        throw ("cannot build a release package:" + [Environment]::NewLine + $lines)
    }

    Write-Detail "$($cars.Count) car(s) in $CarsDir"
    Write-Detail "$($tracks.Count) track(s) in $TrackDir"
}

function Get-ProjectVersion {
    $ini = Join-Path $RepoRoot 'game-unreal\Config\DefaultGame.ini'
    if (Test-Path $ini) {
        $match = Select-String -LiteralPath $ini -Pattern '^\s*ProjectVersion\s*=\s*(.+?)\s*$' |
            Select-Object -First 1
        if ($match) { return $match.Matches[0].Groups[1].Value }
    }
    return '0.0.0'
}

function Get-GitCommit {
    if (-not (Test-Command 'git')) { return 'unknown' }
    $sha = & git -C $RepoRoot rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $sha) { return 'unknown' }
    if (& git -C $RepoRoot status --porcelain 2>$null) { return "$sha-dirty" }
    return $sha
}

# Server content: only what the server actually reads at startup. The .glb car
# models (~43 MB) are already cooked into the client, the .ats sidecars belong
# to the track editor, and content/tracks/export is intermediate bake output.
# The <Track>.ground.msgpack heightfield, <Track>.curbs.msgpack bands and
# <Track>.walls.msgpack barriers the bake writes next to each YAML are server
# input: without the first a car that leaves the asphalt stays at road height
# instead of following the ground the client draws, without the second the
# curbs count as off track, and without the third nothing stops a car at the
# barriers.
function Copy-ServerContent {
    param([string]$Destination)

    $carsOut = Join-Path $Destination 'cars'
    foreach ($car in Get-CarFiles) {
        $target = Join-Path $carsOut (Split-Path -Leaf $car.DirectoryName)
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -LiteralPath $car.FullName -Destination (Join-Path $target 'car.toml') -Force
    }

    $tracksOut = Join-Path $Destination 'tracks\real'
    New-Item -ItemType Directory -Path $tracksOut -Force | Out-Null
    $missingGround = [Collections.Generic.List[string]]::new()
    $missingCurbs = [Collections.Generic.List[string]]::new()
    $missingWalls = [Collections.Generic.List[string]]::new()
    foreach ($track in Get-TrackFiles) {
        Copy-Item -LiteralPath $track.FullName -Destination $tracksOut -Force
        $ground = Join-Path $track.DirectoryName ($track.BaseName + '.ground.msgpack')
        if (Test-Path -LiteralPath $ground) {
            Copy-Item -LiteralPath $ground -Destination $tracksOut -Force
        } else {
            $missingGround.Add($track.BaseName)
        }
        $curbs = Join-Path $track.DirectoryName ($track.BaseName + '.curbs.msgpack')
        if (Test-Path -LiteralPath $curbs) {
            Copy-Item -LiteralPath $curbs -Destination $tracksOut -Force
        } else {
            $missingCurbs.Add($track.BaseName)
        }
        $walls = Join-Path $track.DirectoryName ($track.BaseName + '.walls.msgpack')
        if (Test-Path -LiteralPath $walls) {
            Copy-Item -LiteralPath $walls -Destination $tracksOut -Force
        } else {
            $missingWalls.Add($track.BaseName)
        }
    }
    if ($missingGround.Count -gt 0) {
        Write-Warning ("no ground heightfield for: {0} (run the track bake; off-track cars will sit at road height)" -f ($missingGround -join ', '))
    }
    if ($missingCurbs.Count -gt 0) {
        Write-Warning ("no curb bands for: {0} (run the track bake; curbs will count as off track)" -f ($missingCurbs -join ', '))
    }
    if ($missingWalls.Count -gt 0) {
        Write-Warning ("no walls for: {0} (run the track bake; cars will drive through the barriers)" -f ($missingWalls -join ', '))
    }
}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()

Write-Step 'Preflight'
Invoke-Preflight

if (-not $Version) { $Version = Get-ProjectVersion }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $RepoRoot 'artifacts\release' }

$PackageName = "ApexSim-$Version-Win64"
$ReleaseDir  = [IO.Path]::GetFullPath((Join-Path $OutputDirectory $PackageName))
$GameDir     = Join-Path $ReleaseDir 'Game'
$ServerDir   = Join-Path $ReleaseDir 'Server'
$Commit      = Get-GitCommit

Write-Detail "version $Version ($Commit), client configuration $Configuration"
Write-Detail "package $ReleaseDir"

if ($SkipClient -and
    -not (Test-Path (Join-Path $GameDir 'ApexSim.exe')) -and
    -not (Test-Path (Join-Path $ClientArtifactDirectory 'ApexSim.exe'))) {
    throw ("-SkipClient, but there is no packaged client at " +
           "$(Join-Path $GameDir 'ApexSim.exe') or $(Join-Path $ClientArtifactDirectory 'ApexSim.exe')")
}

# --- server ----------------------------------------------------------------

if ($SkipServer) {
    Write-Step 'Skipping the server build; using the existing binary'
}
else {
    Write-Step 'Building the server (cargo build --release)'
    Invoke-Tool -Exe 'cargo' -What 'cargo build' `
        -Arguments @('build', '--release', '--manifest-path',
                     (Join-Path $RepoRoot 'server\Cargo.toml'))
}
if (-not (Test-Path $ServerExe)) {
    throw "the server build did not produce $ServerExe"
}

# --- props -----------------------------------------------------------------

if ($SkipProps) {
    Write-Step 'Skipping the prop import; using the kit already imported'
}
else {
    Write-Step 'Importing the prop kit into /Game/Props'
    $engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
        -Requires 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    if ($BuildEditor) {
        # The commandlet has to match the source; the track step's own
        # build then finds the target up to date.
        Invoke-Tool -Exe (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') -What 'build' `
            -Arguments @('ApexSimEditor', 'Win64', 'Development', "-Project=$Uproject", '-WaitMutex')
    }
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') `
        -What 'ApexPropImport' `
        -Arguments @($Uproject, '-run=ApexPropImport', '-all', '-unattended',
                     '-nopause', '-nosplash', '-stdout', '-utf8output')
    $imported = @(Get-ChildItem $PropsDir -Filter 'SM_*.uasset' -Recurse -File -ErrorAction SilentlyContinue)
    if ($imported.Count -eq 0) {
        throw "the prop import left nothing under $PropsDir"
    }
}

# --- tracks ----------------------------------------------------------------

if ($SkipTracks) {
    Write-Step ('Skipping the track bake; using the {0} already there' -f ($(if ($RuntimeTracks) { 'exports' } else { 'levels' })))
}
elseif ($RuntimeTracks) {
    Write-Step 'Baking tracks for the game to build at runtime'
    $trackArgs = @{ Release = $true; SkipImport = $true }
    if ($EngineRoot) { $trackArgs.EngineRoot = $EngineRoot }
    & (Join-Path $PSScriptRoot 'build_track_levels.ps1') @trackArgs

    $missing = @(Get-MissingExports)
    if ($missing.Count -gt 0) {
        throw "the bake left these circuits without an export: $($missing -join ', ')"
    }

    # Nothing but a runtime track references the parent materials, and no
    # level import ran to bake them, so bake them here for the cook.
    Write-Step 'Baking the track materials into /Game/Materials/Track'
    $engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
        -Requires 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    if ($BuildEditor) {
        Invoke-Tool -Exe (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') -What 'build' `
            -Arguments @('ApexSimEditor', 'Win64', 'Development', "-Project=$Uproject", '-WaitMutex')
    }
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') `
        -What 'ApexMaterialBake' `
        -Arguments @($Uproject, '-run=ApexMaterialBake', '-unattended',
                     '-nopause', '-nosplash', '-stdout', '-utf8output')
    if (-not (Test-Path (Join-Path $MaterialsDir 'M_ApexTrackBase.uasset'))) {
        throw "ApexMaterialBake left no M_ApexTrackBase under $MaterialsDir"
    }
    $cooked = @(Get-ChildItem $LevelDir -Filter 'L_*.umap' -Recurse -ErrorAction SilentlyContinue)
    if ($cooked.Count -gt 0) {
        Write-Warning (("{0} imported level(s) under {1} will be cooked too, and the game prefers them; " +
                        'delete that folder for a package whose circuits are all built at runtime') -f $cooked.Count, $LevelDir)
    }
}
else {
    Write-Step 'Baking tracks and importing them as Unreal levels'
    $trackArgs = @{ Release = $true }
    if ($BuildEditor) { $trackArgs.Build = $true }
    if ($EngineRoot)  { $trackArgs.EngineRoot = $EngineRoot }
    & (Join-Path $PSScriptRoot 'build_track_levels.ps1') @trackArgs

    $missing = @(Get-TrackFiles | ForEach-Object { $_.BaseName } | Where-Object {
        -not (Test-Path (Join-Path $LevelDir "$_\L_$_.umap"))
    })
    if ($missing.Count -gt 0) {
        throw ('the track import left these circuits without a level, so they ' +
               "would race in an empty world: $($missing -join ', ')")
    }
}

# --- track catalog ---------------------------------------------------------

if ($SkipCatalog) {
    Write-Step 'Skipping the catalog sync; using the rows already imported'
}
else {
    Write-Step 'Baking the track catalog and syncing it into DT_TrackCatalog'
    Invoke-Tool -Exe 'python' -What 'build_track_catalog.py' `
        -Arguments @((Join-Path $PSScriptRoot 'build_track_catalog.py'))

    $engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
        -Requires 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') `
        -What 'ApexTrackCatalogSync' `
        -Arguments @($Uproject, '-run=ApexTrackCatalogSync', '-unattended',
                     '-nopause', '-nosplash', '-stdout', '-utf8output')
}

# --- client ----------------------------------------------------------------

if ($SkipClient) {
    if (Test-Path (Join-Path $GameDir 'ApexSim.exe')) {
        Write-Step 'Skipping the client package; using the build already staged'
    }
    else {
        Write-Step 'Skipping the client package; copying the standalone build artifact'
        Write-Detail "source: $ClientArtifactDirectory"
        if (Test-Path $GameDir) { Remove-Item -LiteralPath $GameDir -Recurse -Force }
        New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null
        Copy-Item -LiteralPath $ClientArtifactDirectory -Destination $GameDir -Recurse -Force
    }
}
else {
    Write-Step "Packaging the client ($Configuration, Win64)"

    # BuildCookRun archives into <archivedirectory>\Windows. Point it straight
    # at the release folder and rename afterwards: a 1.8 GB copy is not worth
    # paying for, and a same-volume rename is free.
    $staged = Join-Path $ReleaseDir 'Windows'
    foreach ($stale in @($staged, $GameDir)) {
        if (Test-Path $stale) {
            Write-Detail "removing $stale"
            Remove-Item -LiteralPath $stale -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null

    $clientArgs = @{ Configuration = $Configuration; OutputDirectory = $ReleaseDir }
    if ($EngineRoot) { $clientArgs.EngineRoot = $EngineRoot }
    & (Join-Path $PSScriptRoot 'build_game_standalone.ps1') @clientArgs

    if (-not (Test-Path (Join-Path $staged 'ApexSim.exe'))) {
        throw "expected the packaged client at $(Join-Path $staged 'ApexSim.exe')"
    }
    Rename-Item -LiteralPath $staged -NewName 'Game'
}

# --- assemble --------------------------------------------------------------

Write-Step "Assembling $PackageName"

if (Test-Path $ServerDir) { Remove-Item -LiteralPath $ServerDir -Recurse -Force }
New-Item -ItemType Directory -Path $ServerDir -Force | Out-Null

Copy-Item -LiteralPath $ServerExe -Destination $ServerDir -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'server.toml') -Destination $ServerDir -Force
Copy-ServerContent -Destination (Join-Path $ServerDir 'content')
$runtimeTrackCount = 0
if ($RuntimeTracks) {
    Copy-RuntimeTracks -Destination (Join-Path $GameDir 'Tracks')
    $runtimeTrackCount = @(Get-ChildItem (Join-Path $GameDir 'Tracks') -Filter '*.uescene.json' -File).Count
    Write-Detail "$runtimeTrackCount runtime track(s) in $(Join-Path $GameDir 'Tracks')"
}
Copy-Item -LiteralPath (Join-Path $RepoRoot 'LICENSE') -Destination $ReleaseDir -Force

# A sample rather than a live settings.yml: the client creates the real file on
# its first run, filled in for the display it actually finds, and shipping one
# would override that with a resolution guess that is wrong on most monitors.
# Keep this in step with ApexBootSettingsIo::Serialise, which writes the same
# shape from the game side.
$sampleSettings = @'
# ApexSim settings - sample.
#
# The game writes its own settings.yml next to ApexSim.exe the first time it
# runs, describing the display it finds there. This file is that same file with
# the shipped defaults, so you can see what is in it without starting the game.
#
# To use it, copy it over Game\settings.yml. The game rewrites that file
# whenever these settings are changed in Settings, so comments added to it will
# not survive; delete it to go back to the defaults.

display:
  # WIDTHxHEIGHT, e.g. 2560x1440. Ignored in borderless, which always
  # takes the size of the monitor it opens on.
  resolution: 1920x1080
  # fullscreen | borderless | windowed
  window_mode: fullscreen
  vsync: false
  # Frames per second, or 0 for uncapped.
  frame_limit: 144

server:
  # The server the game connects to when it starts. 127.0.0.1 is a server
  # on this machine, such as the one Play.bat starts for you.
  host: 127.0.0.1
  port: 9000
'@
Write-TextFile (Join-Path $GameDir 'settings.sample.yml') $sampleSettings

$carCount   = @(Get-CarFiles).Count
$trackCount = @(Get-TrackFiles).Count
$levels     = @(Get-ChildItem $LevelDir -Filter 'L_*.umap' -Recurse -ErrorAction SilentlyContinue)

# Launchers. Both cd into their own folder first: the server resolves
# content/ and its TLS paths relative to the working directory.
$startServerBat = @'
@echo off
rem Host an ApexSim server. Extra arguments are passed through, e.g.
rem   Start-Server.bat --log-level debug
cd /d "%~dp0Server"
apexsim-server.exe %*
if errorlevel 1 pause
'@
Set-Content -LiteralPath (Join-Path $ReleaseDir 'Start-Server.bat') `
    -Value $startServerBat -Encoding ascii

$playBat = @'
@echo off
rem Start a local server unless one is already listening, then play.
cd /d "%~dp0"
netstat -an | findstr /r /c:":9000 .*LISTENING" >nul
if errorlevel 1 (
    echo Starting the ApexSim server...
    start "ApexSim Server" /min /d "%~dp0Server" "%~dp0Server\apexsim-server.exe"
    timeout /t 2 /nobreak >nul
) else (
    echo Using the server already listening on port 9000.
)
start "" "%~dp0Game\ApexSim.exe"
'@
Set-Content -LiteralPath (Join-Path $ReleaseDir 'Play.bat') `
    -Value $playBat -Encoding ascii

$readme = @"
ApexSim $Version (Windows 64-bit)
Built from commit $Commit, client configuration $Configuration.

QUICK START

    Double-click Play.bat. It starts a local server and launches the game.
    In the menu, connect to 127.0.0.1:9000.

WHAT IS IN HERE

    Game\              The ApexSim client. Run Game\ApexSim.exe to play
                       against a server someone else is hosting.
    Game\Tracks\       Circuits the game builds when they are raced (present
                       when the package ships its tracks as data). A new
                       circuit is its .uescene.json, .uemesh and .png here,
                       with the matching .yaml in Server\content\tracks\real.
    Game\settings.yml  Resolution, window mode and the server to connect to.
                       Written on the first run; edit it in any text editor.
                       Game\settings.sample.yml is the same file with the
                       shipped defaults, to read before you have run anything.
    Server\            The authoritative simulation server, its server.toml
                       and the car and track data it reads at startup.
    Start-Server.bat   Run the server on its own, to host for other people.
    Play.bat           Server plus client, for playing on your own machine.

SETTINGS

    The first run writes Game\settings.yml next to ApexSim.exe, holding the
    resolution, window mode, vsync, frame limit and the server to connect to.
    Game\settings.sample.yml is that file with the shipped defaults and a
    comment on every setting: read it now, or copy it over settings.yml to
    start from it.

    Point host at someone else's machine to join their game without going
    through the menu. The game rewrites settings.yml when you change any of
    these in Settings, so comments you add to it will not survive that; delete
    the file to go back to the defaults. Everything else - assists, quality,
    key bindings - lives in Settings inside the game.

HOSTING FOR OTHER PEOPLE

    Run Start-Server.bat and open these ports to your players:

        9000/tcp   login, lobby and session management
        9001/udp   telemetry out, player input in
        9002/tcp   health and Prometheus metrics (optional, keep it private)

    Players then connect to your machine's address on port 9000.

    Out of the box the server accepts any name (auth mode "dev") and does not
    require TLS, which suits a LAN or a private game among friends. For
    anything public, edit Server\server.toml: set require_tls = true with a
    certificate and key, and switch [auth] to mode = "token".

MODDING

    Server\content\cars\<car>\car.toml holds each car's physics, and
    Server\content\tracks\real\*.yaml the circuit centrelines. The server
    validates both at startup and is the authority on them, so edits change
    the simulation for everyone connected. Visuals stay as they were cooked
    into the client.

CONTENTS

    $trackCount track(s), $carCount car(s), $($levels.Count) baked circuit level(s), $runtimeTrackCount runtime track(s).

Licensed under the terms in LICENSE.
"@
# UTF-8 without a BOM: Notepad and JSON parsers both prefer it, and PS 5.1's
# -Encoding utf8 always writes one.
Write-TextFile (Join-Path $ReleaseDir 'README.txt') $readme

$manifest = [ordered]@{
    name          = 'ApexSim'
    version       = $Version
    commit        = $Commit
    platform      = 'Win64'
    configuration = $Configuration
    built_at      = (Get-Date).ToUniversalTime().ToString('o')
    contents      = [ordered]@{
        cars         = $carCount
        tracks       = $trackCount
        track_levels = $levels.Count
        runtime_tracks = $runtimeTrackCount
        server       = (Split-Path -Leaf $ServerExe)
    }
}
Write-TextFile (Join-Path $ReleaseDir 'release.json') ($manifest | ConvertTo-Json -Depth 4)

$size = (Get-ChildItem $ReleaseDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Detail ('{0} car(s), {1} track(s), {2} level(s), {3} on disk' -f `
    $carCount, $trackCount, $levels.Count, (Format-Size $size))

# --- zip -------------------------------------------------------------------

$zipPath = $null
if ($Zip) {
    Write-Step 'Compressing the package'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipPath = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) "$PackageName.zip"
    if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Write-Detail 'this takes a few minutes for a package this size'
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $ReleaseDir, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
}

$stopwatch.Stop()

Write-Step 'Done'
Write-Host "    Package: $ReleaseDir"
if ($zipPath) {
    Write-Host ('    Zip:     {0} ({1})' -f $zipPath, (Format-Size (Get-Item $zipPath).Length))
}
else {
    Write-Host '    Re-run with -Zip to produce the archive to upload.'
}
Write-Host ('    Took {0:hh\:mm\:ss}' -f $stopwatch.Elapsed)

<#
.SYNOPSIS
    Build whatever generated content a checkout is missing, so the game runs.

.DESCRIPTION
    game-unreal/Content/ is gitignored: only the menu, the two catalog tables,
    the splash and a few legacy cars are checked in. Everything else the
    client races on is generated from content/ by commandlets, and the server
    needs the track sidecars the bake writes. On a fresh clone this script
    produces all of it; on an existing checkout it only redoes what is
    missing, so it is safe to run at any time.

        1. ApexSimEditor target (incremental; the commandlets live in it)
        2. The release server, if server/target/release has none
        3. Cars: every content/cars/<folder> whose SM_<folder> is missing
           (all of them when a class wheel mesh is missing)
        4. Ground textures: bake the PNGs, then ApexGroundTexImport
        5. Track materials: ApexMaterialBake when /Game/Materials/Track lacks
           them or stage 4 ran (the base parent's surface graph depends on
           the ground textures)
        6. Props: ApexPropImport -all when any kit GLB has no mesh
        7. Tracks: dress and export every circuit that lacks its export
           (.uescene.json + .uemesh), its catalog preview or one of its
           server sidecars (ground/curbs/walls). There are no track levels:
           the game builds each circuit from its export when it is raced,
           with whatever props and ground textures /Game holds then, so an
           import in stage 4 or 6 needs no rebake.

    Close the editor first; the commandlets and the build need its assets and
    binaries. A full fresh run takes a while, most of it the Unreal stages.

.PARAMETER Force
    Redo every stage, whatever is already there.

.PARAMETER DryRun
    Report what is missing and which stages would run, and do none of them.

.PARAMETER SkipBuild
    Do not build the ApexSimEditor target (use the binaries already there).

.PARAMETER EngineRoot
    Unreal Engine install directory (the folder containing Engine/). Falls back
    to $env:UE and the .uproject's EngineAssociation (see lib/ApexEngine.ps1).

.EXAMPLE
    ./scripts/initialize_content.ps1
    ./scripts/initialize_content.ps1 -DryRun
    ./scripts/initialize_content.ps1 -Force
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$DryRun,
    [switch]$SkipBuild,
    [string]$EngineRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot    = Split-Path -Parent $PSScriptRoot
$Uproject    = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
$GameContent = Join-Path $RepoRoot 'game-unreal\Content'
$CarsSrc     = Join-Path $RepoRoot 'content\cars'
$WheelsSrc   = Join-Path $RepoRoot 'content\wheels'
$PropsSrc    = Join-Path $RepoRoot 'content\props'
$TrackDir    = Join-Path $RepoRoot 'content\tracks\real'
$ExportDir   = Join-Path $RepoRoot 'content\tracks\export'
$TrackMats   = Join-Path $RepoRoot 'game-unreal\Content\Materials\Track'
$GroundPngs  = Join-Path $RepoRoot 'content\textures\ground'
$ServerExe   = Join-Path $RepoRoot 'server\target\release\apexsim-server.exe'

# Must match bake_ground_textures.py and ApexGroundTexImport.
$GroundSets  = 'asphalt', 'grass', 'gravel', 'sand', 'concrete', 'astroturf', 'kerb'
$GroundMaps  = 'col', 'nrm', 'rough'
$Sidecars    = 'ground', 'curbs', 'walls'

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

function Test-Command {
    param([string]$Name)
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

# Runs a tool from the repo root and turns a non-zero exit code into a
# terminating error, so a failed stage never feeds a stale one downstream.
function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments, [string]$What)

    Write-Detail "$Exe $($Arguments -join ' ')"
    Push-Location $RepoRoot
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

# A car folder's mesh, as ApexCarImport names it: hyphens become underscores.
function Get-CarMeshPath {
    param([string]$Folder)
    $segment = $Folder -replace '-', '_'
    return Join-Path $GameContent "Cars\$segment\SM_$segment.uasset"
}

function Format-List {
    param([string[]]$Items, [int]$Max = 8)
    if ($Items.Count -le $Max) { return $Items -join ', ' }
    return (($Items | Select-Object -First $Max) -join ', ') + ", ... ($($Items.Count) in all)"
}

if (-not (Test-Path $Uproject)) {
    throw "expected the Unreal project at $Uproject"
}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()

# ---------------------------------------------------------------------------
# What is missing
# ---------------------------------------------------------------------------

Write-Step 'Looking for missing content'

$carFolders = @(Get-ChildItem $CarsSrc -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'car.toml') } |
    ForEach-Object { $_.Name })
$missingCars = @($carFolders | Where-Object { -not (Test-Path (Get-CarMeshPath $_)) })
$missingWheels = @(Get-ChildItem $WheelsSrc -Filter '*.glb' -File -ErrorAction SilentlyContinue |
    Where-Object {
        -not (Test-Path (Join-Path $GameContent "Cars\Wheels\$($_.BaseName)\SM_Wheel_$($_.BaseName).uasset"))
    } | ForEach-Object { $_.BaseName })

$missingPngs = @()
$missingGround = @()
foreach ($set in $GroundSets) {
    foreach ($map in $GroundMaps) {
        if (-not (Test-Path (Join-Path $GroundPngs "${set}_$map.png"))) { $missingPngs += "${set}_$map" }
        if (-not (Test-Path (Join-Path $GameContent "Ground\T_ground_${set}_$map.uasset"))) {
            $missingGround += "${set}_$map"
        }
    }
}

# The kit is content/props/<kind>/<asset>.glb; _tools and _batches hold the
# generators, not assets.
$missingProps = @(Get-ChildItem $PropsSrc -Directory |
    Where-Object { -not $_.Name.StartsWith('_') } |
    ForEach-Object {
        $kind = $_.Name
        Get-ChildItem $_.FullName -Filter '*.glb' -File | Where-Object {
            -not (Test-Path (Join-Path $GameContent "Props\$kind\SM_$($_.BaseName).uasset"))
        } | ForEach-Object { "$kind/$($_.BaseName)" }
    })

$trackStems = @(Get-ChildItem $TrackDir -Filter '*.yaml' -File | ForEach-Object { $_.BaseName })
$missingTracks = @($trackStems | Where-Object {
    $stem = $_
    # The export the game builds the circuit from (one from before the mesh
    # blob has no .uemesh), its preview, and the server's sidecars.
    $noExport = -not (Test-Path (Join-Path $ExportDir "$stem.uemesh"))
    $noPreview = -not (Test-Path (Join-Path $ExportDir "previews\$stem.png"))
    $noSidecar = @($Sidecars | Where-Object {
        -not (Test-Path (Join-Path $TrackDir "$stem.$_.msgpack"))
    }).Count -gt 0
    $noExport -or $noPreview -or $noSidecar
})
$missingMaterials = @('M_ApexTrackBase', 'M_ApexEmissive', 'M_ApexBrand', 'M_ApexDecal' | Where-Object {
    -not (Test-Path (Join-Path $TrackMats "$_.uasset"))
})

$splash = Join-Path $GameContent 'Splash\Splash.bmp'

Write-Detail ("cars:            " + $(if ($missingCars) { Format-List $missingCars } else { 'ok' }))
Write-Detail ("wheels:          " + $(if ($missingWheels) { Format-List $missingWheels } else { 'ok' }))
Write-Detail ("ground PNGs:     " + $(if ($missingPngs) { "$($missingPngs.Count) missing" } else { 'ok' }))
Write-Detail ("ground textures: " + $(if ($missingGround) { "$($missingGround.Count) missing" } else { 'ok' }))
Write-Detail ("track materials: " + $(if ($missingMaterials) { Format-List $missingMaterials } else { 'ok' }))
Write-Detail ("props:           " + $(if ($missingProps) { Format-List $missingProps } else { 'ok' }))
Write-Detail ("tracks:          " + $(if ($missingTracks) { Format-List $missingTracks } else { 'ok' }))
Write-Detail ("server:          " + $(if (Test-Path $ServerExe) { 'ok' } else { 'not built' }))
if (-not (Test-Path $splash)) {
    Write-Warning "no startup splash at $splash (it is checked in; is the checkout complete?)"
}

# ---------------------------------------------------------------------------
# What to run
# ---------------------------------------------------------------------------

$doServer  = $Force -or -not (Test-Path $ServerExe)
$doCars    = $Force -or $missingCars.Count -gt 0 -or $missingWheels.Count -gt 0
$doPngs    = $Force -or $missingPngs.Count -gt 0
$doGround  = $Force -or $doPngs -or $missingGround.Count -gt 0
$doMats    = $Force -or $doGround -or $missingMaterials.Count -gt 0
$doProps   = $Force -or $missingProps.Count -gt 0
# The bake picks the ground material's shape and the import resolves props by
# what /Game holds, so either import invalidates every level.
# The game dresses each circuit with whatever /Game holds when it builds it,
# so a prop or ground import leaves the exports as they are.
$allTracks = $Force
$tracks    = @(if ($allTracks) { $trackStems } else { $missingTracks })
$doTracks  = $tracks.Count -gt 0
$needUnreal = $doCars -or $doGround -or $doMats -or $doProps

# A missing wheel is imported with a car that uses it; simplest is all cars.
$carsToImport = @(if ($Force -or $missingWheels.Count -gt 0) { } else { $missingCars })

$plan = [Collections.Generic.List[string]]::new()
if ($needUnreal -and -not $SkipBuild) { $plan.Add('build the ApexSimEditor target') }
if ($doServer)  { $plan.Add('build the release server') }
if ($doCars)    { $plan.Add($(if ($carsToImport) { "import cars: $(Format-List $carsToImport)" } else { 'import every car' })) }
if ($doPngs)    { $plan.Add('bake the ground texture PNGs') }
if ($doGround)  { $plan.Add('import the ground textures') }
if ($doMats)    { $plan.Add('bake the track materials') }
if ($doProps)   { $plan.Add('import the prop kit') }
if ($doTracks)  { $plan.Add($(if ($allTracks) { 'dress and bake every track, with previews' } else { "dress and bake, with previews: $(Format-List $tracks)" })) }

if ($plan.Count -eq 0) {
    Write-Step 'Nothing is missing'
    return
}

Write-Step 'Plan'
$plan | ForEach-Object { Write-Detail "- $_" }
if ($DryRun) {
    Write-Step 'Dry run: stopping here'
    return
}

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------

$problems = [Collections.Generic.List[string]]::new()
if (($doServer -or $doTracks) -and -not (Test-Command 'cargo')) {
    $problems.Add('cargo is not on PATH (install Rust)')
}
if ($doPngs -or $doTracks) {
    if (-not (Test-Command 'python')) {
        $problems.Add('python is not on PATH (install Python 3)')
    }
    else {
        & python -c 'import numpy, PIL, yaml' 2>$null
        if ($LASTEXITCODE -ne 0) {
            $problems.Add('python is missing numpy, Pillow or PyYAML (pip install numpy pillow pyyaml)')
        }
    }
}
$editorCmd = $null
$engine = $null
if ($needUnreal) {
    $engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
        -Requires 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $editorCmd = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    Write-Detail "Unreal Engine: $engine"
    if (Get-Process -Name 'UnrealEditor' -ErrorAction SilentlyContinue) {
        $problems.Add('the Unreal editor is running; close it first (it holds the assets and binaries)')
    }
}
if ($problems.Count -gt 0) {
    throw ("cannot initialise content:`n  - " + ($problems -join "`n  - "))
}

$commandletArgs = @('-unattended', '-nopause', '-nosplash', '-stdout', '-utf8output')

# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------

if ($needUnreal -and -not $SkipBuild) {
    Write-Step 'Building the ApexSimEditor target'
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') `
        -Arguments @('ApexSimEditor', 'Win64', 'Development', "-Project=$Uproject", '-WaitMutex') `
        -What 'ApexSimEditor build'
}

if ($doServer) {
    Write-Step 'Building the release server'
    Push-Location (Join-Path $RepoRoot 'server')
    try {
        & cargo build --release
        if ($LASTEXITCODE -ne 0) { throw "cargo build --release failed with exit code $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}

if ($doCars) {
    Write-Step 'Importing cars'
    $carArgs = @{ EngineRoot = $engine }
    if ($carsToImport) { $carArgs.Car = $carsToImport }
    & (Join-Path $PSScriptRoot 'import_cars.ps1') @carArgs
}

if ($doPngs) {
    Write-Step 'Baking the ground texture PNGs'
    Invoke-Tool -Exe 'python' -Arguments @('scripts/bake_ground_textures.py') -What 'bake_ground_textures.py'
}

if ($doGround) {
    Write-Step 'Importing the ground textures into /Game/Ground'
    Invoke-Tool -Exe $editorCmd -Arguments (@($Uproject, '-run=ApexGroundTexImport') + $commandletArgs) `
        -What 'ApexGroundTexImport'
}

if ($doMats) {
    # Every track the game builds instantiates these; a -Force run redoes
    # them, since a graph may have changed.
    Write-Step 'Baking the track materials into /Game/Materials/Track'
    $bakeArgs = @($Uproject, '-run=ApexMaterialBake') + $commandletArgs
    if ($Force) { $bakeArgs += '-force' }
    Invoke-Tool -Exe $editorCmd -Arguments $bakeArgs -What 'ApexMaterialBake'
}

if ($doProps) {
    Write-Step 'Importing the prop kit into /Game/Props'
    Invoke-Tool -Exe $editorCmd -Arguments (@($Uproject, '-run=ApexPropImport', '-all') + $commandletArgs) `
        -What 'ApexPropImport'
}

if ($doTracks) {
    Write-Step 'Baking the tracks'
    # The materials were seen to above; this is Rust and Python only.
    $trackArgs = @{ Release = $true; SkipMaterials = $true }
    if (-not $allTracks) { $trackArgs.Track = $tracks }
    & (Join-Path $PSScriptRoot 'build_track_levels.ps1') @trackArgs
}

$stopwatch.Stop()
Write-Step ("Content initialised in {0:hh\:mm\:ss}" -f $stopwatch.Elapsed)
Write-Detail 'Play with ./scripts/play_editor.ps1'

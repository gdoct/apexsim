<#
.SYNOPSIS
    Bake every circuit into the export the game builds it from at runtime.

.DESCRIPTION
    Runs the whole track pipeline end to end, so every circuit under
    content/tracks/real ends up as an export in content/tracks/export - the
    files the game builds the circuit from when it is raced. There are no
    cooked track levels (docs/RUNTIME_CONTENT_LOADING.md): a changed circuit
    is playable in the editor build as soon as this finishes (restart the
    game, or `apexsim.track.Rescan`).

        1. (optional, -Build) build the ApexSimEditor target, so the
           commandlets in the ApexTrackEditor module match the C++ source
        2. (optional, -ImportProps) UnrealEditor-Cmd -run=ApexPropImport -all
                                                 -> game-unreal/Content/Props/...
        3. cargo run --bin ats-dress -- --all    -> content/tracks/real/*.ats
        4. cargo run --bin ats-export -- --all   -> content/tracks/export/<Track>.{uescene.json,uemesh}
                                                    (+ the server's sidecars beside each YAML)
        5. python scripts/build_track_catalog.py -> content/tracks/export/previews/<Track>.png
        6. UnrealEditor-Cmd -run=ApexMaterialBake -> /Game/Materials/Track, the
           parent materials every runtime track instantiates (only the
           missing ones; cheap)
        7. (optional, -ImportLevels) UnrealEditor-Cmd -run=ApexTrackImport
                                                 -> game-unreal/Content/Tracks/<Track>/L_<Track>
           A level to look at a circuit in the editor. The game never loads
           it and it is never cooked.

    Stage 3 is what keeps a circuit's scenery in step with its layout
    dossier. It used to be run by hand, which is exactly how the Red Bull
    Ring shipped for a day without its bull statue: the landmark went into
    Spielberg.layout.json, nobody re-dressed the scene, and the bake read
    the old one. Dressing is idempotent, so running it every time costs a
    few seconds and removes the failure mode.

    Both generated stages are regenerated wholesale; nothing under
    content/tracks/export or Content/Tracks should be hand-edited.

    The exporter resolves its input and output directories relative to the
    working directory, so this script always runs cargo from the repo root
    rather than from track-editor/.

.PARAMETER Track
    Track stems (the .yaml file name without extension, e.g. Monza) to process
    instead of every track. Applies to both the bake and the import.

.PARAMETER EngineRoot
    Unreal Engine install directory (the folder containing Engine/). Falls back
    to $env:UE, $env:UE_ROOT, the registry entry for the .uproject's
    EngineAssociation, and then the default launcher install locations.

.PARAMETER Build
    Compile the ApexSimEditor target before the Unreal steps. Needed after
    touching C++ under game-unreal/Source; skip it for a content-only rebake.

.PARAMETER ImportProps
    Bring the authored prop kit (content/props/<kind>/*.glb, docs/PROPS.md)
    into /Game/Props first, with the ApexPropImport commandlet. Needed once,
    and again whenever a GLB changes; a track is dressed with whatever is
    there when the game builds it, with generated stand-ins for the rest, so
    nothing needs rebaking after an import.

.PARAMETER Release
    Build the exporter in release mode. Slower to compile, much faster to bake
    a full 26-track set.

.PARAMETER DryRun
    Report what each step would do without writing assets: prints the
    commands, passes --dry-run to the dresser and -dryrun to the commandlets.
    The bake still runs.

.PARAMETER SkipDress
    Leave the .ats scenes alone. Only for working on a scene by hand; the
    next dress run overwrites what the pass owns either way.

.PARAMETER SkipExport
    Keep the exports already sitting in content/tracks/export.

.PARAMETER SkipPreviews
    Leave the catalog previews alone (they need Python with numpy, Pillow and
    PyYAML).

.PARAMETER SkipMaterials
    Do not run ApexMaterialBake. With neither -ImportProps nor -ImportLevels
    that makes this a pure Rust/Python run that needs no engine at all.

.PARAMETER ImportLevels
    Also import the exports as levels under game-unreal/Content/Tracks, to
    open a circuit in the editor. Never needed to play.

.PARAMETER ExtraEditorArgs
    Extra switches appended to the UnrealEditor-Cmd invocation.

.EXAMPLE
    ./scripts/build_track_levels.ps1
    Dress and bake every track, with previews and materials.

.EXAMPLE
    ./scripts/build_track_levels.ps1 -Track Monza,Spa -SkipMaterials
    Redress and rebake two circuits; no engine involved.

.EXAMPLE
    ./scripts/build_track_levels.ps1 -Track Spa -ImportLevels -Build
    Rebuild the editor target, rebake Spa and import it as a level to inspect.
#>
[CmdletBinding()]
param(
    [string[]]$Track,
    [string]$EngineRoot,
    [switch]$Build,
    [switch]$ImportProps,
    [switch]$Release,
    [switch]$DryRun,
    [switch]$SkipDress,
    [switch]$SkipExport,
    [switch]$SkipPreviews,
    [switch]$SkipMaterials,
    [switch]$ImportLevels,
    [string[]]$ExtraEditorArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$Uproject  = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
$TrackDir  = Join-Path $RepoRoot 'content\tracks\real'
$ExportDir = Join-Path $RepoRoot 'content\tracks\export'
$LevelDir  = Join-Path $RepoRoot 'game-unreal\Content\Tracks'

. (Join-Path $PSScriptRoot 'lib\ApexEngine.ps1')

function Write-Step {
    param([string]$Message)
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# Runs $Exe from $WorkingDir and turns a non-zero exit code into a terminating
# error, so a failed bake never silently feeds a stale import.
function Invoke-Tool {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [string]$WorkingDir = $RepoRoot,
        [string]$What
    )

    Write-Host "    $Exe $($Arguments -join ' ')" -ForegroundColor DarkGray
    if ($DryRun -and $What -eq 'build') { return }

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

# Track stem -> the YAML the exporter takes. The stem is also what the
# commandlet's -track= matches, since exports keep their source file name.
function Resolve-TrackFiles {
    param([string[]]$Names)

    $files = @()
    foreach ($name in $Names) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($name)
        $path = Join-Path $TrackDir "$stem.yaml"
        if (-not (Test-Path $path)) {
            $available = (Get-ChildItem $TrackDir -Filter '*.yaml' |
                ForEach-Object { $_.BaseName }) -join ', '
            throw "no track named `"$stem`" in $TrackDir. Available: $available"
        }
        $files += $path
    }
    return $files
}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()

if (-not (Test-Path $Uproject)) {
    throw "expected the Unreal project at $Uproject"
}

$trackFiles = @()
if ($Track) {
    # @() keeps a single track from unrolling into a bare string.
    $trackFiles = @(Resolve-TrackFiles $Track)
}

$needEngine = $Build -or $ImportProps -or $ImportLevels -or -not $SkipMaterials
$engine = $null
if ($needEngine) {
    $engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
        -Requires 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    Write-Host "Unreal Engine: $engine" -ForegroundColor DarkGray
}

if ($Build) {
    Write-Step 'Building the ApexSimEditor target'
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') `
        -Arguments @('ApexSimEditor', 'Win64', 'Development', "-Project=$Uproject", '-WaitMutex') `
        -What 'build'
}

if ($ImportProps) {
    Write-Step 'Importing the prop kit into /Game/Props'
    $propArgs = @($Uproject, '-run=ApexPropImport', '-all',
        '-unattended', '-nopause', '-nosplash', '-stdout', '-utf8output')
    if ($DryRun) { $propArgs += '-dryrun' }
    if ($ExtraEditorArgs) { $propArgs += $ExtraEditorArgs }
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') `
        -Arguments $propArgs -What 'ApexPropImport'
}

if ($SkipDress) {
    Write-Step 'Skipping the dressing pass; using the scenes as they are'
}
else {
    if ($Track) {
        Write-Step "Dressing $($trackFiles.Count) track(s) from their layout dossiers"
    }
    else {
        Write-Step "Dressing every track in $TrackDir that has a layout dossier"
    }

    $dressArgs = @('run', '--quiet', '--manifest-path',
        (Join-Path $RepoRoot 'track-editor\Cargo.toml'), '--bin', 'ats-dress')
    if ($Release) { $dressArgs += '--release' }
    $dressArgs += '--'
    if ($DryRun) { $dressArgs += '--dry-run' }
    if ($trackFiles.Count -gt 0) { $dressArgs += $trackFiles } else { $dressArgs += '--all' }

    Invoke-Tool -Exe 'cargo' -Arguments $dressArgs -What 'ats-dress'
}

if ($SkipExport) {
    Write-Step 'Skipping the bake; using the existing exports'
}
else {
    if ($Track) {
        Write-Step "Baking $($trackFiles.Count) track(s) to $ExportDir"
    }
    else {
        Write-Step "Baking every track in $TrackDir to $ExportDir"
    }

    $cargoArgs = @('run', '--quiet', '--manifest-path',
        (Join-Path $RepoRoot 'track-editor\Cargo.toml'), '--bin', 'ats-export')
    if ($Release) { $cargoArgs += '--release' }
    $cargoArgs += '--'
    if ($trackFiles.Count -gt 0) { $cargoArgs += $trackFiles } else { $cargoArgs += '--all' }

    Invoke-Tool -Exe 'cargo' -Arguments $cargoArgs -What 'ats-export'
}

if ($SkipPreviews) {
    Write-Step 'Skipping the catalog previews'
}
else {
    Write-Step 'Drawing the catalog previews'
    $previewArgs = @((Join-Path $PSScriptRoot 'build_track_catalog.py'))
    if ($Track) { $previewArgs += @($Track | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }) }
    Invoke-Tool -Exe 'python' -Arguments $previewArgs -What 'build_track_catalog.py'
}

if ($SkipMaterials) {
    Write-Step 'Skipping the track materials'
}
else {
    Write-Step 'Baking the track materials into /Game/Materials/Track (the missing ones)'
    $bakeArgs = @($Uproject, '-run=ApexMaterialBake',
        '-unattended', '-nopause', '-nosplash', '-stdout', '-utf8output')
    if ($ExtraEditorArgs) { $bakeArgs += $ExtraEditorArgs }
    if ($DryRun) {
        Write-Host "    (dry run) UnrealEditor-Cmd $($bakeArgs -join ' ')" -ForegroundColor DarkGray
    }
    else {
        Invoke-Tool -Exe (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') `
            -Arguments $bakeArgs -What 'ApexMaterialBake'
    }
}

if ($ImportLevels) {
    Write-Step "Importing exports into $LevelDir (for the editor only)"

    $editorArgs = @($Uproject, '-run=ApexTrackImport')
    if ($Track) { $editorArgs += "-track=$($Track -join ',')" } else { $editorArgs += '-all' }
    $editorArgs += @('-unattended', '-nopause', '-nosplash', '-stdout', '-utf8output')
    if ($DryRun) { $editorArgs += '-dryrun' }
    if ($ExtraEditorArgs) { $editorArgs += $ExtraEditorArgs }

    Invoke-Tool -Exe (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') `
        -Arguments $editorArgs -What 'ApexTrackImport'
}

$stopwatch.Stop()

$exports = @(Get-ChildItem $ExportDir -Filter '*.uescene.json' -ErrorAction SilentlyContinue)
$levels = @(Get-ChildItem $LevelDir -Filter 'L_*.umap' -Recurse -ErrorAction SilentlyContinue)

Write-Step 'Done'
Write-Host ("    {0} export(s) in {1}" -f $exports.Count, $ExportDir)
if ($levels.Count -gt 0) {
    Write-Host ("    {0} editor-only level(s) in {1} (never loaded by the game)" -f $levels.Count, $LevelDir)
}
Write-Host ("    took {0:mm\:ss}" -f $stopwatch.Elapsed)

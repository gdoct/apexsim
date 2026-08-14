<#
.SYNOPSIS
    Bake tracks with the track editor and import them into Unreal as levels.

.DESCRIPTION
    Runs the whole track pipeline end to end, so every circuit under
    content/tracks/real ends up as /Game/Tracks/<Track>/L_<Track>:

        1. (optional) build the ApexSimEditor target, so the commandlet in the
           ApexTrackEditor module matches the current C++ source
        2. cargo run --bin ats-export -- --all   -> content/tracks/export/*.uescene.json
        3. UnrealEditor-Cmd -run=ApexTrackImport -> game-unreal/Content/Tracks/...

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
    Compile the ApexSimEditor target before importing. Needed after touching
    C++ under game-unreal/Source; skip it for a content-only rebake.

.PARAMETER Release
    Build the exporter in release mode. Slower to compile, much faster to bake
    a full 26-track set.

.PARAMETER DryRun
    Report what each step would do without writing assets: prints the commands,
    and passes -dryrun to the commandlet so it parses and summarises the
    exports without building assets. The bake still runs, since the import has
    nothing to read otherwise.

.PARAMETER SkipExport
    Import the exports already sitting in content/tracks/export.

.PARAMETER SkipImport
    Bake the .uescene.json files and stop before Unreal.

.PARAMETER ExtraEditorArgs
    Extra switches appended to the UnrealEditor-Cmd invocation.

.EXAMPLE
    ./scripts/build_track_levels.ps1
    Bake and import every track.

.EXAMPLE
    ./scripts/build_track_levels.ps1 -Track Monza,Spa -Build
    Rebuild the editor target, then rebake and reimport two circuits.
#>
[CmdletBinding()]
param(
    [string[]]$Track,
    [string]$EngineRoot,
    [switch]$Build,
    [switch]$Release,
    [switch]$DryRun,
    [switch]$SkipExport,
    [switch]$SkipImport,
    [string[]]$ExtraEditorArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$Uproject  = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
$TrackDir  = Join-Path $RepoRoot 'content\tracks\real'
$ExportDir = Join-Path $RepoRoot 'content\tracks\export'
$LevelDir  = Join-Path $RepoRoot 'game-unreal\Content\Tracks'

function Write-Step {
    param([string]$Message)
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Get-Property {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
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

function Resolve-EngineRoot {
    param([string]$Explicit)

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($Explicit) { $candidates.Add($Explicit) }
    foreach ($name in 'UE', 'UE_ROOT', 'UE5_ROOT') {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $candidates.Add($value) }
    }

    # The .uproject's EngineAssociation ("5.8") doubles as the registry key
    # name a launcher install writes its location under.
    $association = Get-Property (Get-Content $Uproject -Raw | ConvertFrom-Json) 'EngineAssociation'
    if ($association) {
        foreach ($hive in 'HKLM:\SOFTWARE\EpicGames\Unreal Engine',
                          'HKCU:\SOFTWARE\EpicGames\Unreal Engine') {
            $key = Join-Path $hive $association
            if (Test-Path $key) {
                $installed = Get-Property (Get-ItemProperty $key) 'InstalledDirectory'
                if ($installed) { $candidates.Add($installed) }
            }
        }
        $candidates.Add("C:\Program Files\Epic Games\UE_$association")
        $candidates.Add("C:\Epic Games\UE_$association")
    }

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe')) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw ("could not find Unreal Engine $association. Pass -EngineRoot " +
           '<path>, or set $env:UE to the folder containing Engine\. Tried: ' +
           ($candidates -join '; '))
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

$engine = $null
if (-not $SkipImport) {
    $engine = Resolve-EngineRoot $EngineRoot
    Write-Host "Unreal Engine: $engine" -ForegroundColor DarkGray
}

if ($Build) {
    if ($SkipImport) {
        Write-Warning '-Build without an import step only compiles; nothing will be imported.'
    }
    Write-Step 'Building the ApexSimEditor target'
    Invoke-Tool -Exe (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') `
        -Arguments @('ApexSimEditor', 'Win64', 'Development', "-Project=$Uproject", '-WaitMutex') `
        -What 'build'
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

if ($SkipImport) {
    Write-Step 'Skipping the Unreal import'
}
else {
    Write-Step "Importing exports into $LevelDir"

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
Write-Host ("    {0} level(s) in {1}" -f $levels.Count, $LevelDir)
Write-Host ("    took {0:mm\:ss}" -f $stopwatch.Elapsed)

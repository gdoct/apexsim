<#
.SYNOPSIS
    Bake the content: the ground textures and every circuit's scene, export,
    sidecars and catalog preview. Needs cargo and Python, nothing else.

.DESCRIPTION
    The content half of the build, run from this folder (the content root;
    every path below is relative to it). It reads and writes only the content:
    the code (../apexsim) calls it, never the other way round.

        ground    python tools/bake_ground_textures.py -> textures/ground/*.png
                  (only when one is missing, or with -Force)
        dress     ats-dress  -> tracks/*/<Track>.ats, from each track's layout
                  dossier (<Track>.layout.json); a track without one is kept
                  as it is
        export    ats-export -> build/tracks/<Track>.{uescene.json,uemesh}
                  and the server's sidecars beside each YAML
                  (<Track>.{ground,curbs,walls,road,pit}.msgpack)
        previews  python tools/build_track_catalog.py
                  -> build/tracks/previews/<Track>.png, build/tracks/track_catalog.json

    Dressing is idempotent and runs every time, so a dossier edit always
    reaches the scene (docs/track-pipeline.md has the refresh order). A track
    marked imported (tools/ac_import.py) belongs to its importer and is left
    alone. Everything this writes is regenerated wholesale: nothing under
    build/tracks, and no sidecar, is edited by hand.

.PARAMETER Stage
    The stages to run, in the order above (default: dress, export, previews;
    ground runs by itself only when a PNG is missing).

.PARAMETER Track
    Track stems (Monza, Spa, ...) to dress, export and draw instead of all.

.PARAMETER Release
    Build the ats-* tools in release mode: slower to compile, much faster
    over the whole calendar.

.PARAMETER Force
    Bake the ground PNGs even when they are all there.

.PARAMETER DryRun
    Print the commands; pass --dry-run to the dresser. The export still runs.

.EXAMPLE
    ./bake.ps1
    Dress, export and draw every track (and the ground PNGs if missing).

.EXAMPLE
    ./bake.ps1 -Track Monzarella,SpaFrankenchamps -Release
    Redress and rebake two circuits.

.EXAMPLE
    ./bake.ps1 -Stage ground -Force
    Rebake the ground texture PNGs.
#>
[CmdletBinding()]
param(
    [ValidateSet('ground', 'dress', 'export', 'previews')]
    [string[]]$Stage = @('ground', 'dress', 'export', 'previews'),
    [string[]]$Track,
    [switch]$Release,
    [switch]$Force,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ContentRoot = $PSScriptRoot
$ExportDir = Join-Path $ContentRoot 'build\tracks'
$GroundPngs = Join-Path $ContentRoot 'textures\ground'
# Must match tools/bake_ground_textures.py (and ApexGroundTexImport in the code).
$GroundSets = @('asphalt', 'grass', 'gravel', 'sand', 'concrete', 'astroturf', 'kerb')
$GroundMaps = @('col', 'nrm', 'rough')

. (Join-Path $ContentRoot 'tools\lib\ApexTracks.ps1')

function Write-Step {
    param([string]$Message)
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# Runs $Exe from the content root and turns a non-zero exit code into a
# terminating error, so a failed stage never feeds a stale one downstream.
function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments, [string]$What)

    Write-Host "    $Exe $($Arguments -join ' ')" -ForegroundColor DarkGray
    Push-Location $ContentRoot
    try {
        & $Exe @Arguments
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($code -ne 0) { throw "$What failed with exit code $code" }
}

# Track stem -> its YAML, relative to the content root (how the ats-* tools
# take it). An imported track is skipped with a warning.
function Resolve-TrackFiles {
    param([string[]]$Names)

    $prefix = $ContentRoot.TrimEnd('\') + '\'
    $files = @()
    foreach ($name in $Names) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($name)
        $path = Find-ApexTrackFile -ContentRoot $ContentRoot -Stem $stem
        if (-not $path) {
            $available = (Get-ApexTrackFiles -ContentRoot $ContentRoot |
                ForEach-Object { $_.BaseName }) -join ', '
            throw "no track named `"$stem`" in tracks\default or tracks\custom. Available: $available"
        }
        if (Test-ApexImportedTrack -TrackFile $path) {
            $rebuild = Get-ApexImportedTrackRebuild -TrackFile $path
            Write-Warning ("skipping ${stem}: it was imported, so its export and sidecars come from the importer" +
                $(if ($rebuild) { "; rebuild it with: $rebuild" } else { '' }))
            continue
        }
        $full = [IO.Path]::GetFullPath($path)
        $files += $(if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { $full.Substring($prefix.Length) } else { $full })
    }
    return $files
}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()

$trackFiles = @()
if ($Track) {
    # @() keeps a single track from unrolling into a bare string.
    $trackFiles = @(Resolve-TrackFiles $Track)
    if ($trackFiles.Count -eq 0) {
        # Every named track was imported: the batch forms would rebake the calendar.
        Write-Host 'Nothing to bake: every named track is an import.' -ForegroundColor Yellow
        $Stage = @($Stage | Where-Object { $_ -eq 'ground' })
    }
}

if ($Stage -contains 'ground') {
    $missing = @(foreach ($set in $GroundSets) {
            foreach ($map in $GroundMaps) {
                if (-not (Test-Path (Join-Path $GroundPngs "${set}_$map.png"))) { "${set}_$map" }
            }
        })
    if ($Force -or $missing.Count -gt 0) {
        Write-Step 'Baking the ground texture PNGs'
        if ($DryRun) { Write-Host '    (dry run) python tools/bake_ground_textures.py' -ForegroundColor DarkGray }
        else { Invoke-Tool -Exe 'python' -Arguments @('tools/bake_ground_textures.py') -What 'bake_ground_textures.py' }
    }
}

if ($Stage -contains 'dress') {
    # Only a track with a layout dossier has anything to dress from; naming
    # one without to ats-dress is a failure.
    $dressFiles = @($trackFiles | Where-Object {
            Test-Path (Join-Path $ContentRoot ([IO.Path]::ChangeExtension($_, '.layout.json')))
        })
    $undressed = @($trackFiles | Where-Object { $dressFiles -notcontains $_ })
    if ($undressed.Count -gt 0) {
        Write-Host ("No layout dossier, scene kept as it is: " +
            (($undressed | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }) -join ', ')) `
            -ForegroundColor DarkGray
    }
    if ($Track -and $dressFiles.Count -eq 0) {
        Write-Step 'Skipping the dressing pass; no named track has a layout dossier'
    }
    else {
        Write-Step $(if ($Track) { "Dressing $($dressFiles.Count) track(s) from their layout dossiers" }
            else { 'Dressing every track (default and custom) that has a layout dossier' })
        $dressArgs = @('run', '--quiet', '--manifest-path', 'track-editor\Cargo.toml', '--bin', 'ats-dress')
        if ($Release) { $dressArgs += '--release' }
        $dressArgs += '--'
        if ($DryRun) { $dressArgs += '--dry-run' }
        if ($Track) { $dressArgs += $dressFiles } else { $dressArgs += '--all' }
        Invoke-Tool -Exe 'cargo' -Arguments $dressArgs -What 'ats-dress'
    }
}

if ($Stage -contains 'export' -and -not ($Track -and $trackFiles.Count -eq 0)) {
    Write-Step $(if ($Track) { "Baking $($trackFiles.Count) track(s) to $ExportDir" }
        else { "Baking every track (default and custom) to $ExportDir" })
    $exportArgs = @('run', '--quiet', '--manifest-path', 'track-editor\Cargo.toml', '--bin', 'ats-export')
    if ($Release) { $exportArgs += '--release' }
    $exportArgs += '--'
    if ($trackFiles.Count -gt 0) { $exportArgs += $trackFiles } else { $exportArgs += '--all' }
    Invoke-Tool -Exe 'cargo' -Arguments $exportArgs -What 'ats-export'
}

if ($Stage -contains 'previews' -and -not ($Track -and $trackFiles.Count -eq 0)) {
    Write-Step 'Drawing the catalog previews'
    $previewArgs = @('tools\build_track_catalog.py')
    if ($Track) { $previewArgs += @($Track | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }) }
    Invoke-Tool -Exe 'python' -Arguments $previewArgs -What 'build_track_catalog.py'
}

$stopwatch.Stop()
$exports = @(Get-ChildItem $ExportDir -Filter '*.uescene.json' -ErrorAction SilentlyContinue)
Write-Step 'Baked'
Write-Host ("    {0} export(s) in {1}" -f $exports.Count, $ExportDir)
Write-Host ("    took {0:mm\:ss}" -f $stopwatch.Elapsed)

<#
.SYNOPSIS
    Where a track's files live: the shipped circuits, then the player's own.

.DESCRIPTION
    Dot-source this from a script in scripts/:

        . (Join-Path $PSScriptRoot 'lib\ApexTracks.ps1')

    content\tracks\default holds the circuits that ship with the game;
    content\tracks\custom the player's own (imported or hand-made, and
    gitignored). Every file of a track (<Stem>.yaml, .ats, .layout.json, the
    sidecars) sits in the folder its YAML is in, and a stem is unique across
    both, since the exports and previews are keyed by the stem alone. The
    same two folders as scripts/track_dirs.py and
    track_core::ue_export_io::TRACK_DIRS.
#>

function Get-ApexTrackDirs {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        # Only the shipped circuits (what a release packages by default).
        [switch]$DefaultOnly
    )
    $dirs = @(Join-Path $RepoRoot 'content\tracks\default')
    if (-not $DefaultOnly) { $dirs += Join-Path $RepoRoot 'content\tracks\custom' }
    return $dirs
}

# Every track YAML in the folders, default first, sorted within each.
function Get-ApexTrackFiles {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$DefaultOnly
    )
    $files = @()
    foreach ($dir in Get-ApexTrackDirs -RepoRoot $RepoRoot -DefaultOnly:$DefaultOnly) {
        if (Test-Path $dir) {
            $files += @(Get-ChildItem $dir -Filter '*.yaml' -File | Sort-Object Name)
        }
    }
    return $files
}

# The YAML of the track with this stem, or $null.
function Find-ApexTrackFile {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$Stem
    )
    foreach ($dir in Get-ApexTrackDirs -RepoRoot $RepoRoot) {
        $path = Join-Path $dir "$Stem.yaml"
        if (Test-Path $path) { return $path }
    }
    return $null
}

<#
.SYNOPSIS
    The track guides: what to build and where they go.

.DESCRIPTION
    Dot-source this from a script in scripts/:

        . (Join-Path $PSScriptRoot 'lib\ApexGuide.ps1')

    A track guide (docs/content/track-guide.md) is two files per shipped circuit
    and car class, `<Stem>.<Class>.guide.json` and its recording
    `<Stem>.<Class>.guide.apxs`, built by `apexsim-replay guide` from the
    track YAML, its hand-written notes (`<Stem>.guide.yml`), its dossier and
    the class's car.toml files into build\guide (gitignored, like the
    showcases). The game reads them from Guide\ beside ApexSim.exe, or the
    repo's build\guide in the editor.

    The tool resolves content paths relative to the working directory, so
    it runs from the repo root. Its `--missing-only` skips a guide newer
    than every file it was made from, which is the freshness rule here too.
#>

. (Join-Path $PSScriptRoot 'ApexTracks.ps1')

function Get-ApexGuideDir {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return Join-Path $RepoRoot 'build\guide'
}

function Get-ApexGuideToolPath {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return Join-Path $RepoRoot 'server\target\release\apexsim-replay.exe'
}

# The shipped circuits (content\tracks\default, imported ones excepted) with
# no guide at all, or with one older than its YAML, notes or dossier. The car
# side of freshness is the tool's own --missing-only.
function Get-ApexGuideWork {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $dir = Get-ApexGuideDir -RepoRoot $RepoRoot
    $work = [Collections.Generic.List[string]]::new()
    foreach ($yaml in @(Get-ApexTrackFiles -RepoRoot $RepoRoot -DefaultOnly)) {
        if (Test-ApexImportedTrack -TrackFile $yaml.FullName) { continue }
        $stem = $yaml.BaseName
        $tracks = $yaml.DirectoryName
        $guides = @(if (Test-Path -LiteralPath $dir) {
                Get-ChildItem -LiteralPath $dir -Filter "$stem.*.guide.json" -File
            })
        if ($guides.Count -eq 0) { $work.Add($stem); continue }
        $inputs = @($yaml.FullName,
            (Join-Path $tracks "$stem.guide.yml"),
            (Join-Path $tracks "$stem.layout.json") | Where-Object { Test-Path -LiteralPath $_ })
        $newest = ($inputs | ForEach-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } | Measure-Object -Maximum).Maximum
        $oldest = ($guides | ForEach-Object { $_.LastWriteTimeUtc } | Measure-Object -Minimum).Minimum
        if ($oldest -lt $newest) { $work.Add($stem) }
    }
    return @($work)
}

# Builds every shipped circuit's guides (all classes). Without -Force only
# what is missing or stale is rebuilt. Returns the tool's exit code.
function Start-ApexGuideBuild {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$Force
    )
    $tool = Get-ApexGuideToolPath -RepoRoot $RepoRoot
    if (-not (Test-Path -LiteralPath $tool)) {
        Push-Location (Join-Path $RepoRoot 'server')
        try {
            & cargo build --release --bin apexsim-replay
            if ($LASTEXITCODE -ne 0) { throw 'cargo build --release --bin apexsim-replay failed' }
        }
        finally { Pop-Location }
    }
    $arguments = @('guide', '--all', '--out', (Get-ApexGuideDir -RepoRoot $RepoRoot))
    if (-not $Force) { $arguments += '--missing-only' }
    Push-Location $RepoRoot
    try {
        Write-Host "      $tool $($arguments -join ' ')" -ForegroundColor DarkGray
        # stdout is the tool's JSON summary; progress goes to stderr.
        & $tool @arguments | Out-Null
        return $LASTEXITCODE
    }
    finally { Pop-Location }
}

# Every built guide's two files, sorted by name.
function Get-ApexGuideFiles {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $dir = Get-ApexGuideDir -RepoRoot $RepoRoot
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    $json = @(Get-ChildItem -LiteralPath $dir -Filter '*.guide.json' -File)
    $files = foreach ($file in $json) {
        $file
        $recording = Join-Path $dir ($file.Name -replace '\.guide\.json$', '.guide.apxs')
        if (Test-Path -LiteralPath $recording) { Get-Item -LiteralPath $recording }
    }
    return @($files | Sort-Object Name)
}

# Copies every guide into <Destination> (replaced): Guide\ beside ApexSim.exe
# in a package. Returns the number of guides.
function Copy-ApexGuides {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$Destination
    )
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $files = @(Get-ApexGuideFiles -RepoRoot $RepoRoot)
    foreach ($file in $files) {
        Copy-Item -LiteralPath $file.FullName -Destination $Destination -Force
    }
    return @($files | Where-Object { $_.Name -like '*.guide.json' }).Count
}

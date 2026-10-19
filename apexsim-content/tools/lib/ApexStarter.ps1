<#
.SYNOPSIS
    The starter set (starter.yml at the content root): the cars and circuits
    a release ships.

.DESCRIPTION
    Dot-source it, then Get-ApexStarterSet -ContentRoot <content root>. The
    file is a flat list format (a `key:` line, then `- item` lines, `#`
    comments), read here without a YAML parser; anything else in it is an
    error, so a mis-shaped file fails loudly rather than shipping the wrong
    set. Every car must be a folder under cars/default and every track a
    stem under tracks/default.
#>

function Get-ApexStarterSet {
    param([Parameter(Mandatory)][string]$ContentRoot)

    $path = Join-Path $ContentRoot 'starter.yml'
    if (-not (Test-Path -LiteralPath $path)) { throw "no starter set at $path" }

    $lists = @{ cars = [Collections.Generic.List[string]]::new(); tracks = [Collections.Generic.List[string]]::new() }
    $key = $null
    $lineNo = 0
    foreach ($raw in Get-Content -LiteralPath $path) {
        $lineNo++
        $line = ($raw -replace '#.*$', '').TrimEnd()
        if (-not $line.Trim()) { continue }
        if ($line -match '^([a-z_]+):\s*$') {
            $key = $Matches[1]
            if (-not $lists.ContainsKey($key)) { throw "starter.yml line ${lineNo}: unknown key '$key' (cars, tracks)" }
            continue
        }
        if ($line -match '^\s+-\s+(\S+)\s*$') {
            if (-not $key) { throw "starter.yml line ${lineNo}: an item before any key" }
            $lists[$key].Add($Matches[1])
            continue
        }
        throw "starter.yml line ${lineNo}: expected 'key:' or '- item', got '$line'"
    }

    $problems = @()
    foreach ($car in $lists.cars) {
        if (-not (Test-Path -LiteralPath (Join-Path $ContentRoot "cars\default\$car\car.toml"))) {
            $problems += "car '$car' has no cars\default\$car\car.toml"
        }
    }
    foreach ($track in $lists.tracks) {
        if (-not (Test-Path -LiteralPath (Join-Path $ContentRoot "tracks\default\$track\$track.yaml"))) {
            $problems += "track '$track' has no tracks\default\$track\$track.yaml"
        }
    }
    if ($lists.cars.Count -eq 0) { $problems += 'no cars' }
    if ($lists.tracks.Count -eq 0) { $problems += 'no tracks' }
    if ($problems) { throw ("starter.yml: " + ($problems -join '; ')) }

    return [pscustomobject]@{ Cars = @($lists.cars); Tracks = @($lists.tracks); Path = $path }
}

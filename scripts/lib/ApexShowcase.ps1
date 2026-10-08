<#
.SYNOPSIS
    The showcases: rendered AI races, what to render and whether it is fresh.

.DESCRIPTION
    Dot-source this from a script in scripts/:

        . (Join-Path $PSScriptRoot 'lib\ApexShowcase.ps1')

    A showcase is a spectator stream (<Stem>.<class>.<variant>.apxs,
    docs/SPECTATOR.md) rendered by `apexsim-replay render` from a headless AI
    race: the menu plays it behind its screens and the server plays it to
    clients. content\showcase.yml lists what to render (track, class, sky
    variants, grid, laps, seed); build\showcase holds the files, gitignored
    like the track exports. The same arguments and seed write a
    byte-identical file, and `apexsim-replay info --check` exits non-zero
    when the track YAML or a car.toml changed since the render, which is
    what makes a showcase stale.

    Every render runs from the repo root: the tool resolves content paths
    relative to the working directory, and so does `--check`.

    content\showcase.yml is read by a parser of its own, since PowerShell
    has no YAML reader: top-level keys whose values are nested `key: value`
    maps (defaults), nested keys holding a flow map (variants), and a list
    of one-line flow maps (showcases). Anything else is an error, so a
    mis-shaped file fails loudly rather than rendering the wrong thing.

    The functions that run the tool are Start-*, not Invoke-*: Windows
    Defender's script scanner blocks any function whose name is "Invoke-"
    followed by this project's name (a post-exploitation toolkit is called
    that) before the file is even parsed, and the text of a comment counts.
#>

# Where a track's files are and whether it was imported (Find-ApexTrackFile,
# Test-ApexImportedTrack).
. (Join-Path $PSScriptRoot 'ApexTracks.ps1')

# The tool, built with the server: server\target\release\apexsim-replay.exe.
function Get-ApexShowcaseToolPath {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return Join-Path $RepoRoot 'server\target\release\apexsim-replay.exe'
}

# The list file and the output folder.
function Get-ApexShowcaseListPath {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return Join-Path $RepoRoot 'content\showcase.yml'
}

function Get-ApexShowcaseDir {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return Join-Path $RepoRoot 'build\showcase'
}

# One YAML scalar: quotes stripped, an integer kept as a string (the render
# arguments are strings anyway).
function ConvertFrom-ApexYamlScalar {
    param([string]$Text)
    $t = $Text.Trim()
    if ($t.Length -ge 2 -and (($t[0] -eq '"' -and $t[-1] -eq '"') -or ($t[0] -eq "'" -and $t[-1] -eq "'"))) {
        return $t.Substring(1, $t.Length - 2)
    }
    return $t
}

# Splits the inside of `{ ... }` or `[ ... ]` at the commas outside any
# nested brackets or quotes.
function Split-ApexYamlFlow {
    param([string]$Text)
    $parts = [Collections.Generic.List[string]]::new()
    $depth = 0
    $quote = $null
    $current = [Text.StringBuilder]::new()
    foreach ($ch in $Text.ToCharArray()) {
        if ($quote) {
            [void]$current.Append($ch)
            if ($ch -eq $quote) { $quote = $null }
            continue
        }
        switch ($ch) {
            '"' { $quote = '"'; [void]$current.Append($ch) }
            "'" { $quote = "'"; [void]$current.Append($ch) }
            '{' { $depth++; [void]$current.Append($ch) }
            '[' { $depth++; [void]$current.Append($ch) }
            '}' { $depth--; [void]$current.Append($ch) }
            ']' { $depth--; [void]$current.Append($ch) }
            ',' {
                if ($depth -eq 0) { $parts.Add($current.ToString()); [void]$current.Clear() }
                else { [void]$current.Append($ch) }
            }
            default { [void]$current.Append($ch) }
        }
    }
    if ($current.ToString().Trim()) { $parts.Add($current.ToString()) }
    return @($parts | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

# A flow value: `{ k: v, k2: [a, b] }` -> ordered hashtable, `[a, b]` ->
# array, else a scalar.
function ConvertFrom-ApexYamlFlow {
    param([string]$Text, [string]$Where)
    $t = $Text.Trim()
    if ($t.StartsWith('{')) {
        if (-not $t.EndsWith('}')) { throw "showcase.yml ${Where}: unclosed { in '$t'" }
        $map = [ordered]@{}
        foreach ($pair in Split-ApexYamlFlow $t.Substring(1, $t.Length - 2)) {
            $at = $pair.IndexOf(':')
            if ($at -le 0) { throw "showcase.yml ${Where}: expected key: value, got '$pair'" }
            $map[$pair.Substring(0, $at).Trim()] = ConvertFrom-ApexYamlFlow -Text $pair.Substring($at + 1) -Where $Where
        }
        return $map
    }
    if ($t.StartsWith('[')) {
        if (-not $t.EndsWith(']')) { throw "showcase.yml ${Where}: unclosed [ in '$t'" }
        return @(Split-ApexYamlFlow $t.Substring(1, $t.Length - 2) | ForEach-Object { ConvertFrom-ApexYamlFlow -Text $_ -Where $Where })
    }
    return ConvertFrom-ApexYamlScalar $t
}

# content\showcase.yml as an object: Defaults (hashtable), Variants (ordered
# hashtable of hashtables) and Showcases (array of hashtables), exactly as
# written. The comments in the file say what each key means.
function Get-ApexShowcaseList {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [string]$Path
    )
    if (-not $Path) { $Path = Get-ApexShowcaseListPath -RepoRoot $RepoRoot }
    if (-not (Test-Path -LiteralPath $Path)) { throw "no showcase list at $Path" }

    $sections = [ordered]@{}
    $section = $null
    $lineNo = 0
    foreach ($raw in Get-Content -LiteralPath $Path) {
        $lineNo++
        # A comment or blank line (a # inside quotes never starts a line).
        $line = ($raw -replace '\s+#.*$', '').TrimEnd()
        if (-not $line.Trim() -or $line.TrimStart().StartsWith('#')) { continue }
        $where = "line $lineNo"
        if (-not $line.StartsWith(' ')) {
            # A top-level key: `name:` opening a map or list.
            if ($line -notmatch '^([A-Za-z_][A-Za-z0-9_]*):\s*$') { throw "showcase.yml ${where}: expected a top-level 'key:' line, got '$line'" }
            $section = $Matches[1]
            $sections[$section] = $null
            continue
        }
        if (-not $section) { throw "showcase.yml ${where}: indented line before any section" }
        $body = $line.Trim()
        if ($body.StartsWith('- ')) {
            # A list item: a flow map.
            if ($null -eq $sections[$section]) { $sections[$section] = [Collections.Generic.List[object]]::new() }
            elseif ($sections[$section] -isnot [Collections.Generic.List[object]]) { throw "showcase.yml ${where}: '$section' mixes list items and keys" }
            $item = ConvertFrom-ApexYamlFlow -Text $body.Substring(2) -Where $where
            if ($item -isnot [Collections.IDictionary]) { throw "showcase.yml ${where}: a '$section' item must be a { ... } map" }
            $sections[$section].Add($item)
            continue
        }
        # A nested `key: value` or `key: { ... }`.
        $at = $body.IndexOf(':')
        if ($at -le 0) { throw "showcase.yml ${where}: expected 'key: value', got '$body'" }
        if ($null -eq $sections[$section]) { $sections[$section] = [ordered]@{} }
        elseif ($sections[$section] -isnot [Collections.IDictionary]) { throw "showcase.yml ${where}: '$section' mixes keys and list items" }
        $key = $body.Substring(0, $at).Trim()
        $value = $body.Substring($at + 1).Trim()
        if (-not $value) { throw "showcase.yml ${where}: '$key' has no value (nested blocks are not supported; use { ... })" }
        $sections[$section][$key] = ConvertFrom-ApexYamlFlow -Text $value -Where $where
    }

    foreach ($required in 'variants', 'showcases') {
        if (-not $sections.Contains($required) -or $null -eq $sections[$required]) { throw "showcase.yml has no '$required' section" }
    }
    $defaults = if ($sections.Contains('defaults') -and $sections['defaults']) { $sections['defaults'] } else { [ordered]@{} }
    $variants = $sections['variants']
    foreach ($name in @($variants.Keys)) {
        $v = $variants[$name]
        if ($v -isnot [Collections.IDictionary] -or -not $v.Contains('weather') -or -not $v.Contains('time')) {
            throw "showcase.yml: variant '$name' needs { weather: ..., time: ... }"
        }
    }
    $showcases = @($sections['showcases'])
    foreach ($entry in $showcases) {
        foreach ($need in 'track', 'class', 'variants') {
            if (-not $entry.Contains($need)) { throw "showcase.yml: a showcase entry lacks '$need': $(($entry.Keys | ForEach-Object { "$_=$($entry[$_])" }) -join ', ')" }
        }
        foreach ($variant in @($entry['variants'])) {
            if (-not $variants.Contains($variant)) { throw "showcase.yml: $($entry['track']) names a variant '$variant' that 'variants' does not define" }
        }
    }
    return [pscustomobject]@{ Path = $Path; Defaults = $defaults; Variants = $variants; Showcases = $showcases }
}

# The file name of one showcase: <Stem>.<class>.<variant>, class in lower
# case (GT3 -> gt3), as docs/SPECTATOR.md names them.
function Get-ApexShowcaseName {
    param([string]$Track, [string]$Class, [string]$Variant)
    return "$Track.$($Class.ToLowerInvariant()).$Variant"
}

# Runs `apexsim-replay info --check` on a file from the repo root. Returns
# $null when the file is fresh, else a one-line reason (the "stale" list the
# tool prints, or its error).
function Test-ApexShowcaseStale {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$Tool,
        [Parameter(Mandatory)][string]$File
    )
    # Under a caller's $ErrorActionPreference = 'Stop', PS 5.1 turns a native
    # command's redirected stderr into a terminating error; this scope's own
    # preference keeps the tool's verdict readable.
    $ErrorActionPreference = 'Continue'
    Push-Location $RepoRoot
    try {
        $output = & $Tool info $File --check 2>&1
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -eq 0) { return $null }
    # Redirected stderr arrives as ErrorRecords, stdout as strings: the JSON
    # report is the strings, the tool's one-line verdict the records.
    $stdout = @($output | Where-Object { $_ -is [string] }) -join "`n"
    $stderr = @($output | Where-Object { $_ -isnot [string] } | ForEach-Object { "$_" }) -join "`n"
    try {
        $json = $stdout | ConvertFrom-Json -ErrorAction Stop
        if ($json.PSObject.Properties['stale'] -and $json.stale) {
            return 'stale: ' + (@($json.stale | ForEach-Object { "$_" }) -join '; ')
        }
    }
    catch { }
    $first = (($stderr + "`n" + $stdout) -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    return "info --check exited $code" + $(if ($first) { ": $first" } else { '' })
}

# What there is to render: one row per (entry, variant) with the output
# path, the render arguments (relative to the repo root) and a Status:
#   missing    no file
#   stale      the file's track or a car changed since it was rendered
#   fresh      the file matches the content on disk
#   unchecked  the file exists and there is no tool to check it with
#   imported   the track was imported (never rendered by the pipeline)
#   no-track   the list names a track that is not on disk
# -Track narrows it to those stems; -NoCheck skips `info --check` (every
# existing file is then 'unchecked').
function Get-ApexShowcasePlan {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [string[]]$Track,
        [string]$Tool,
        [switch]$NoCheck
    )
    $list = Get-ApexShowcaseList -RepoRoot $RepoRoot
    if (-not $Tool) { $Tool = Get-ApexShowcaseToolPath -RepoRoot $RepoRoot }
    $canCheck = (-not $NoCheck) -and (Test-Path -LiteralPath $Tool)
    $outDir = Get-ApexShowcaseDir -RepoRoot $RepoRoot
    $wanted = $null
    if ($Track) { $wanted = @($Track | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }) }

    $rows = [Collections.Generic.List[object]]::new()
    foreach ($entry in $list.Showcases) {
        $stem = [string]$entry['track']
        if ($wanted -and ($wanted -notcontains $stem)) { continue }
        $class = [string]$entry['class']
        $trackFile = Find-ApexTrackFile -RepoRoot $RepoRoot -Stem $stem
        $imported = $false
        if ($trackFile) {
            $imported = (Test-ApexImportedTrack -TrackFile $trackFile) -or
                (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($trackFile, '.import.json')))
        }
        foreach ($variant in @($entry['variants'])) {
            $sky = $list.Variants[$variant]
            $name = Get-ApexShowcaseName -Track $stem -Class $class -Variant $variant
            $outFile = Join-Path $outDir "$name.apxs"
            $settings = [ordered]@{ cars = '20'; laps = '2'; seed = '0'; seeds = $null }
            foreach ($key in @($list.Defaults.Keys)) { $settings[$key] = [string]$list.Defaults[$key] }
            foreach ($key in 'cars', 'laps', 'seed', 'seeds') { if ($entry.Contains($key)) { $settings[$key] = [string]$entry[$key] } }

            $relTrack = if ($trackFile) { $trackFile.Substring($RepoRoot.TrimEnd('\').Length + 1).Replace('\', '/') } else { "content/tracks/default/$stem/$stem.yaml" }
            $renderArgs = @('render', '--track', $relTrack, '--class', $class,
                '--cars-dir', 'content/cars/default',
                '--cars', $settings['cars'], '--laps', $settings['laps'],
                '--weather', [string]$sky['weather'], '--time', [string]$sky['time'],
                '--seed', $settings['seed'])
            if ($settings['seeds'] -and [int]$settings['seeds'] -gt 1) { $renderArgs += @('--seeds', $settings['seeds']) }
            if ($sky.Contains('air')) { $renderArgs += @('--air', [string]$sky['air']) }
            if ($sky.Contains('wind')) { $renderArgs += @('--wind', [string]$sky['wind']) }
            if ($sky.Contains('wind_from')) { $renderArgs += @('--wind-from', [string]$sky['wind_from']) }
            $renderArgs += @('--out', "build/showcase/$name.apxs")

            $status = 'missing'
            $reason = $null
            if (-not $trackFile) { $status = 'no-track' }
            elseif ($imported) { $status = 'imported' }
            elseif (Test-Path -LiteralPath $outFile) {
                if ($canCheck) {
                    $reason = Test-ApexShowcaseStale -RepoRoot $RepoRoot -Tool $Tool -File "build/showcase/$name.apxs"
                    $status = if ($reason) { 'stale' } else { 'fresh' }
                }
                else { $status = 'unchecked' }
            }
            $rows.Add([pscustomobject]@{
                Name      = $name
                Track     = $stem
                Class     = $class
                Variant   = $variant
                TrackFile = $trackFile
                OutFile   = $outFile
                Arguments = $renderArgs
                Status    = $status
                Reason    = $reason
            })
        }
    }
    return @($rows)
}

# The rows of a plan that need rendering: missing or stale, or every
# renderable one with -Force. Imported and absent tracks are never rendered.
function Select-ApexShowcaseWork {
    param([object[]]$Plan, [switch]$Force)
    return @($Plan | Where-Object {
        if ($_.Status -in 'imported', 'no-track') { $false }
        elseif ($Force) { $true }
        else { $_.Status -in 'missing', 'stale' }
    })
}

# Makes sure apexsim-replay.exe exists, building it with cargo when it does
# not (a release build, like the server's). Returns its path.
function Resolve-ApexShowcaseTool {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$DryRun
    )
    $tool = Get-ApexShowcaseToolPath -RepoRoot $RepoRoot
    if (Test-Path -LiteralPath $tool) { return $tool }
    $manifest = Join-Path $RepoRoot 'server\Cargo.toml'
    Write-Host "    cargo build --release --bin apexsim-replay --manifest-path $manifest" -ForegroundColor DarkGray
    if ($DryRun) { return $tool }
    if (-not (Get-Command 'cargo' -ErrorAction SilentlyContinue)) {
        throw "no $tool and cargo is not on PATH (install Rust)"
    }
    Push-Location $RepoRoot
    try {
        & cargo build --release --bin apexsim-replay --manifest-path $manifest
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $tool)) {
        throw "cargo build --release --bin apexsim-replay failed (exit code $code)"
    }
    return $tool
}

# Renders the rows of a plan (see Select-ApexShowcaseWork), one after the
# other, from the repo root; a failed render stops the run. With -DryRun
# the commands are printed and nothing is written. Returns the number
# rendered.
# Runs the tool from the repo root and turns a non-zero exit code into a
# terminating error, like the scripts' own Invoke-Tool.
function Start-ApexShowcaseTool {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$Tool,
        [string[]]$Arguments,
        [string]$What = 'apexsim-replay'
    )
    # The tool prints its report (the header and the score) as JSON on
    # stdout and a one-line summary on stderr; the summary is enough here,
    # and the JSON would otherwise become this function's return value.
    Push-Location $RepoRoot
    try {
        & $Tool @Arguments | Out-Null
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($code -ne 0) {
        throw "$What failed with exit code $code"
    }
}

function Start-ApexShowcaseRender {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [object[]]$Rows,
        [switch]$DryRun
    )
    $rows = @($Rows)
    if ($rows.Count -eq 0) { return 0 }
    $tool = Resolve-ApexShowcaseTool -RepoRoot $RepoRoot -DryRun:$DryRun
    $outDir = Get-ApexShowcaseDir -RepoRoot $RepoRoot
    if (-not $DryRun) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
    $done = 0
    foreach ($row in $rows) {
        $why = ''
        if ($row.Status -eq 'stale') { $why = " ($($row.Reason))" }
        elseif ($row.Status -eq 'missing') { $why = ' (missing)' }
        Write-Host "    $($row.Name)$why" -ForegroundColor DarkGray
        Write-Host "      $tool $($row.Arguments -join ' ')" -ForegroundColor DarkGray
        if ($DryRun) { continue }
        Start-ApexShowcaseTool -RepoRoot $RepoRoot -Tool $tool -Arguments $row.Arguments -What "apexsim-replay render ($($row.Name))"
        if (-not (Test-Path -LiteralPath $row.OutFile)) { throw "apexsim-replay render wrote nothing for $($row.Name)" }
        $done++
    }
    return $done
}

# Every rendered showcase in build\showcase (the pipeline's and any rendered
# by hand), sorted by name.
function Get-ApexShowcaseFiles {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $dir = Get-ApexShowcaseDir -RepoRoot $RepoRoot
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    return @(Get-ChildItem -LiteralPath $dir -Filter '*.apxs' -File | Sort-Object Name)
}

# The showcases content\showcase.yml asks for that build\showcase does not
# have (imported and absent tracks excepted): what a package would ship
# without.
function Get-ApexMissingShowcases {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return @(Get-ApexShowcasePlan -RepoRoot $RepoRoot -NoCheck |
        Where-Object { $_.Status -eq 'missing' } | ForEach-Object { $_.Name })
}

# Copies every .apxs in build\showcase to <Destination> (replaced), where a
# packaged game (Showcase\ beside ApexSim.exe) or server ([showcase] dir)
# looks. Returns the number of files.
function Copy-ApexShowcases {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$Destination
    )
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $files = @(Get-ApexShowcaseFiles -RepoRoot $RepoRoot)
    foreach ($file in $files) {
        Copy-Item -LiteralPath $file.FullName -Destination $Destination -Force
    }
    return $files.Count
}

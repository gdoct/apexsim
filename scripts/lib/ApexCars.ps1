<#
.SYNOPSIS
    The files a car is made of, and copying them where a packaged game looks.

.DESCRIPTION
    Dot-source this from a script in scripts/:

        . (Join-Path $PSScriptRoot 'lib\ApexCars.ps1')

    Cars are not cooked: the game reads each car.toml and builds the GLBs it
    names at runtime (UApexCarContentSubsystem, docs/RUNTIME_CONTENT_LOADING.md).
    A packaged game looks in Cars\ beside ApexSim.exe, with the class wheels in
    Wheels\ beside that. Only what the game reads is shipped: car.toml, the
    body GLB (`model`), the DRS flap GLB (`[drs_flap] model`), the livery
    logos, skins, slot textures and previews (`[[livery]] logo`, `skin`,
    `textures`, `preview`), a wheel GLB of the car's own (`[wheels] model` or
    `rear_model` ending in .glb or holding a /), its steering wheel
    (`[cockpit] steering_wheel_model`) and its driver (`[driver] model`), not
    the .blend files or the texture
    sources.

    content\cars\default holds the cars that ship with the game;
    content\cars\custom the player's own (imported or hand-made, and
    gitignored). A package keeps the split (Cars\default, Cars\custom), and
    the game and the server read default\ first, so a custom car reusing a
    shipped id is the one left out (car_loader::car_toml_paths,
    UApexCarContentSubsystem::CarFolders). A folder name is unique across both.
#>

# Every car.toml under a cars folder: default\ then custom\ (only default\
# with -DefaultOnly, what a release ships), sorted within each. A folder with
# neither subfolder is searched as it is.
function Get-ApexCarTomls {
    param(
        [Parameter(Mandatory)][string]$CarsDir,
        [switch]$DefaultOnly
    )
    $subs = @('default')
    if (-not $DefaultOnly) { $subs += 'custom' }
    $split = @(Join-Path $CarsDir 'default'; Join-Path $CarsDir 'custom') | Where-Object { Test-Path -LiteralPath $_ }
    $dirs = if ($split) { @($subs | ForEach-Object { Join-Path $CarsDir $_ } | Where-Object { Test-Path -LiteralPath $_ }) } else { @($CarsDir) }
    $tomls = @()
    foreach ($dir in $dirs) {
        $tomls += @(Get-ChildItem -LiteralPath $dir -Filter 'car.toml' -Recurse -File -ErrorAction SilentlyContinue |
            Sort-Object FullName)
    }
    return $tomls
}

# A car's folder relative to the cars folder (default\posh-gt3rs), which is
# where a package puts it.
function Get-ApexCarRelativeDir {
    param(
        [Parameter(Mandatory)][string]$CarsDir,
        [Parameter(Mandatory)][string]$CarDir
    )
    $root = (Resolve-Path -LiteralPath $CarsDir).Path.TrimEnd('\') + '\'
    $full = (Resolve-Path -LiteralPath $CarDir).Path
    if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { return $full.Substring($root.Length) }
    return Split-Path -Leaf $full
}

# Whether a [wheels] model / rear_model names a GLB in the car's own folder
# rather than a class wheel in the wheels folder: it ends in .glb or holds a
# path separator (ApexCarToml::IsCarLocalWheel, the game's rule).
function Test-ApexCarLocalWheel {
    param([string]$Model)
    return [bool]($Model -and ($Model.EndsWith('.glb', [StringComparison]::OrdinalIgnoreCase) -or $Model.Contains('/') -or $Model.Contains('\')))
}

# What a car.toml names, read with the same few rules as the game's parser:
# `key = "value"` lines under `[table]` headers, comments dropped, and a
# livery's `textures = ["SLOT=file", ...]` on one line. Wheels holds the
# class wheels it names (shared, by name); CarFiles every other file it
# names relative to the car folder: skins, slot textures, previews, its own
# wheels and its steering wheel.
function Get-ApexCarFiles {
    param([Parameter(Mandatory)][string]$TomlPath)

    $result = [ordered]@{ Model = $null; DrsFlap = $null; Wheel = $null; Wheels = @(); Logos = @(); CarFiles = @() }
    $logos = [Collections.Generic.List[string]]::new()
    $wheels = [Collections.Generic.List[string]]::new()
    $carFiles = [Collections.Generic.List[string]]::new()
    $table = ''
    foreach ($raw in Get-Content -LiteralPath $TomlPath) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith('#')) { continue }
        if ($line.StartsWith('[')) {
            $table = $line.Trim('[', ']', ' ')
            continue
        }
        if ($table -eq 'livery' -and $line -match '^textures\s*=\s*\[(.*)\]') {
            foreach ($entry in [regex]::Matches($Matches[1], '"([^"]*)"')) {
                $pair = $entry.Groups[1].Value
                $at = $pair.IndexOf('=')
                if ($at -gt 0) { $carFiles.Add($pair.Substring($at + 1).Trim()) }
            }
            continue
        }
        if ($line -notmatch '^([A-Za-z0-9_]+)\s*=\s*"([^"]*)"') { continue }
        $key = $Matches[1]
        $value = $Matches[2]
        switch ($table) {
            ''         { if ($key -eq 'model') { $result.Model = $value } }
            'drs_flap' { if ($key -eq 'model') { $result.DrsFlap = $value } }
            'wheels'   {
                if ($key -eq 'model' -or $key -eq 'rear_model') {
                    if ($key -eq 'model') { $result.Wheel = $value }
                    if (Test-ApexCarLocalWheel $value) { $carFiles.Add($value) } else { $wheels.Add($value) }
                }
            }
            'livery'   {
                if ($key -eq 'logo') { $logos.Add($value) }
                elseif ($key -eq 'skin' -or $key -eq 'preview') { $carFiles.Add($value) }
            }
            'cockpit'  { if ($key -eq 'steering_wheel_model') { $carFiles.Add($value) } }
            'driver'   { if ($key -eq 'model') { $carFiles.Add($value) } }
        }
    }
    $result.Logos = @($logos)
    $result.Wheels = @($wheels)
    $result.CarFiles = @($carFiles)
    return [pscustomobject]$result
}

# Problems that would leave a car undrawn or wheel-less in the game: a model,
# flap, logo, skin, wheel or steering wheel the car.toml names but the disk
# does not have.
function Test-ApexCars {
    param(
        [Parameter(Mandatory)][string]$CarsDir,
        [Parameter(Mandatory)][string]$WheelsDir,
        [switch]$DefaultOnly
    )
    $problems = [Collections.Generic.List[string]]::new()
    $seen = @{}
    foreach ($toml in @(Get-ApexCarTomls -CarsDir $CarsDir -DefaultOnly:$DefaultOnly)) {
        $dir = $toml.DirectoryName
        $folder = Get-ApexCarRelativeDir -CarsDir $CarsDir -CarDir $dir
        $leaf = Split-Path -Leaf $dir
        if ($seen.ContainsKey($leaf)) {
            $problems.Add("$folder has the folder name of $($seen[$leaf]); a car folder name must be unique across default and custom")
        }
        else { $seen[$leaf] = $folder }
        $files = Get-ApexCarFiles -TomlPath $toml.FullName
        if (-not $files.Model) {
            $problems.Add("$folder names no model")
        }
        foreach ($relative in @($files.Model, $files.DrsFlap) + $files.Logos + $files.CarFiles) {
            if ($relative -and -not (Test-Path -LiteralPath (Join-Path $dir $relative))) {
                $problems.Add("$folder is missing $relative")
            }
        }
        foreach ($wheel in $files.Wheels) {
            if (-not (Test-Path -LiteralPath (Join-Path $WheelsDir "$wheel.glb"))) {
                $problems.Add("$folder wants the $wheel wheel, and $WheelsDir has no $wheel.glb")
            }
        }
    }
    return @($problems)
}

# Copy every car to <Destination>\Cars\<default|custom>\<folder> and the
# wheels its cars use to <Destination>\Wheels. Both folders are replaced.
# Only the shipped cars with -DefaultOnly. Returns the number of cars.
function Copy-ApexRuntimeCars {
    param(
        [Parameter(Mandatory)][string]$CarsDir,
        [Parameter(Mandatory)][string]$WheelsDir,
        [Parameter(Mandatory)][string]$Destination,
        [switch]$DefaultOnly
    )
    $carsOut = Join-Path $Destination 'Cars'
    $wheelsOut = Join-Path $Destination 'Wheels'
    foreach ($stale in @($carsOut, $wheelsOut)) {
        if (Test-Path $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }
        New-Item -ItemType Directory -Path $stale -Force | Out-Null
    }
    $wheels = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $count = 0
    foreach ($toml in @(Get-ApexCarTomls -CarsDir $CarsDir -DefaultOnly:$DefaultOnly)) {
        $dir = $toml.DirectoryName
        $target = Join-Path $carsOut (Get-ApexCarRelativeDir -CarsDir $CarsDir -CarDir $dir)
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -LiteralPath $toml.FullName -Destination (Join-Path $target 'car.toml') -Force
        $files = Get-ApexCarFiles -TomlPath $toml.FullName
        foreach ($relative in @($files.Model, $files.DrsFlap) + $files.Logos + $files.CarFiles) {
            if (-not $relative) { continue }
            $source = Join-Path $dir $relative
            if (-not (Test-Path -LiteralPath $source)) {
                Write-Warning "$(Split-Path -Leaf $dir): $relative is missing; not shipped"
                continue
            }
            # Relative paths are kept (textures\logo.png), since car.toml names them so.
            $out = Join-Path $target $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $out -Force
        }
        foreach ($wheel in $files.Wheels) { [void]$wheels.Add($wheel) }
        $count++
    }
    foreach ($wheel in $wheels) {
        $source = Join-Path $WheelsDir "$wheel.glb"
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination $wheelsOut -Force
        }
        else {
            Write-Warning "no $wheel.glb in $WheelsDir; the cars that use it will have no wheels"
        }
    }
    return $count
}

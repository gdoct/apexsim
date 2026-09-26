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
    body GLB (`model`), the DRS flap GLB (`[drs_flap] model`) and the livery
    logos (`[[livery]] logo`), not the .blend files or the texture sources.
#>

# What a car.toml names, read with the same few rules as the game's parser:
# `key = "value"` lines under `[table]` headers, comments dropped.
function Get-ApexCarFiles {
    param([Parameter(Mandatory)][string]$TomlPath)

    $result = [ordered]@{ Model = $null; DrsFlap = $null; Wheel = $null; Logos = @() }
    $logos = [Collections.Generic.List[string]]::new()
    $table = ''
    foreach ($raw in Get-Content -LiteralPath $TomlPath) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith('#')) { continue }
        if ($line.StartsWith('[')) {
            $table = $line.Trim('[', ']', ' ')
            continue
        }
        if ($line -notmatch '^([A-Za-z0-9_]+)\s*=\s*"([^"]*)"') { continue }
        $key = $Matches[1]
        $value = $Matches[2]
        switch ($table) {
            ''         { if ($key -eq 'model') { $result.Model = $value } }
            'drs_flap' { if ($key -eq 'model') { $result.DrsFlap = $value } }
            'wheels'   { if ($key -eq 'model') { $result.Wheel = $value } }
            'livery'   { if ($key -eq 'logo')  { $logos.Add($value) } }
        }
    }
    $result.Logos = @($logos)
    return [pscustomobject]$result
}

# Problems that would leave a car undrawn or wheel-less in the game: a model,
# flap, logo or class wheel the car.toml names but the disk does not have.
function Test-ApexCars {
    param(
        [Parameter(Mandatory)][string]$CarsDir,
        [Parameter(Mandatory)][string]$WheelsDir
    )
    $problems = [Collections.Generic.List[string]]::new()
    foreach ($toml in @(Get-ChildItem $CarsDir -Filter 'car.toml' -Recurse -File -ErrorAction SilentlyContinue)) {
        $dir = $toml.DirectoryName
        $folder = Split-Path -Leaf $dir
        $files = Get-ApexCarFiles -TomlPath $toml.FullName
        if (-not $files.Model) {
            $problems.Add("$folder names no model")
        }
        foreach ($relative in @($files.Model, $files.DrsFlap) + $files.Logos) {
            if ($relative -and -not (Test-Path -LiteralPath (Join-Path $dir $relative))) {
                $problems.Add("$folder is missing $relative")
            }
        }
        if ($files.Wheel -and -not (Test-Path -LiteralPath (Join-Path $WheelsDir "$($files.Wheel).glb"))) {
            $problems.Add("$folder wants the $($files.Wheel) wheel, and $WheelsDir has no $($files.Wheel).glb")
        }
    }
    return @($problems)
}

# Copy every car to <Destination>\Cars\<folder> and the wheels its cars use to
# <Destination>\Wheels. Both folders are replaced. Returns the number of cars.
function Copy-ApexRuntimeCars {
    param(
        [Parameter(Mandatory)][string]$CarsDir,
        [Parameter(Mandatory)][string]$WheelsDir,
        [Parameter(Mandatory)][string]$Destination
    )
    $carsOut = Join-Path $Destination 'Cars'
    $wheelsOut = Join-Path $Destination 'Wheels'
    foreach ($stale in @($carsOut, $wheelsOut)) {
        if (Test-Path $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }
        New-Item -ItemType Directory -Path $stale -Force | Out-Null
    }
    $wheels = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $count = 0
    foreach ($toml in @(Get-ChildItem $CarsDir -Filter 'car.toml' -Recurse -File -ErrorAction SilentlyContinue)) {
        $dir = $toml.DirectoryName
        $target = Join-Path $carsOut (Split-Path -Leaf $dir)
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -LiteralPath $toml.FullName -Destination (Join-Path $target 'car.toml') -Force
        $files = Get-ApexCarFiles -TomlPath $toml.FullName
        foreach ($relative in @($files.Model, $files.DrsFlap) + $files.Logos) {
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
        if ($files.Wheel) { [void]$wheels.Add($files.Wheel) }
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

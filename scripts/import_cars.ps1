<#
.SYNOPSIS
    Import the authored car models (content/cars/<folder>/car.toml + .glb) into Unreal.

.DESCRIPTION
    Runs UnrealEditor-Cmd -run=ApexCarImport, which builds
    /Game/Cars/<folder>/SM_<stem> from each folder's GLB and adds or refreshes
    its DT_CarCatalog row from car.toml. By default every folder is imported
    and existing cars are overwritten (-force), which is what you want after
    re-exporting from Blender. Close the editor first; it holds the assets.

.PARAMETER Car
    Car folder names (e.g. limbotiti-caravan-gt3) to import instead of all.

.PARAMETER NoForce
    Skip cars that already have a catalog row instead of re-importing them.

.PARAMETER List
    Print the cars the commandlet would see and stop.

.PARAMETER DryRun
    Parse everything and report, without writing assets.

.PARAMETER EngineRoot
    Unreal Engine install directory (the folder containing Engine/). Falls back
    to $env:UE, $env:UE_ROOT, the registry entry for the .uproject's
    EngineAssociation, and then the default launcher install locations.

.PARAMETER ExtraEditorArgs
    Extra switches appended to the UnrealEditor-Cmd invocation.

.EXAMPLE
    ./scripts/import_cars.ps1
    Re-import every car.

.EXAMPLE
    ./scripts/import_cars.ps1 -Car limbotiti-caravan-gt3,posh-gt3rs
#>
[CmdletBinding()]
param(
    [string[]]$Car,
    [switch]$NoForce,
    [switch]$List,
    [switch]$DryRun,
    [string]$EngineRoot,
    [string[]]$ExtraEditorArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Uproject = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
$CarDir   = Join-Path $RepoRoot 'content\cars'

. (Join-Path $PSScriptRoot 'lib\ApexEngine.ps1')

if (-not (Test-Path $Uproject)) {
    throw "expected the Unreal project at $Uproject"
}

if ($Car) {
    foreach ($name in $Car) {
        if (-not (Test-Path (Join-Path $CarDir "$name\car.toml"))) {
            $available = (Get-ChildItem $CarDir -Directory |
                Where-Object { Test-Path (Join-Path $_.FullName 'car.toml') } |
                ForEach-Object { $_.Name }) -join ', '
            throw "no car folder `"$name`" with a car.toml in $CarDir. Available: $available"
        }
    }
}

$engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
    -Requires 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$editorExe = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
Write-Host "Unreal Engine: $engine" -ForegroundColor DarkGray

$editorArgs = @($Uproject, '-run=ApexCarImport')
if ($List) {
    $editorArgs += '-list'
}
else {
    if ($Car) { $editorArgs += "-car=$($Car -join ',')" } else { $editorArgs += '-all' }
    if (-not $NoForce) { $editorArgs += '-force' }
    if ($DryRun) { $editorArgs += '-dryrun' }
}
$editorArgs += @('-unattended', '-nopause', '-nosplash', '-stdout', '-utf8output')
if ($ExtraEditorArgs) { $editorArgs += $ExtraEditorArgs }

Write-Host ''
Write-Host '==> Importing cars into /Game/Cars' -ForegroundColor Cyan
Write-Host "    $editorExe $($editorArgs -join ' ')" -ForegroundColor DarkGray

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
Push-Location $RepoRoot
try {
    & $editorExe @editorArgs
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}
$stopwatch.Stop()

if ($code -ne 0) {
    throw "ApexCarImport failed with exit code $code"
}
Write-Host ("==> Done in {0:mm\:ss}" -f $stopwatch.Elapsed) -ForegroundColor Cyan

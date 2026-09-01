<#
.SYNOPSIS
   Build, cook, package, and archive the ApexSim Windows client.

.DESCRIPTION
   Runs Unreal Automation Tool's BuildCookRun command to produce a portable
   Win64 build. By default the archive is written to artifacts\ApexSim-Win64
   and contains ApexSim.exe plus its required runtime files.

.PARAMETER EngineRoot
   Unreal Engine install directory (the folder containing Engine/). Defaults
   to $env:UE, $env:UE_ROOT, the project's launcher registry entry, and then
   the usual Epic launcher install locations.

.PARAMETER Configuration
   Client build configuration. Development is the default for local testing.

.PARAMETER OutputDirectory
   Directory that receives the packaged standalone build.

.PARAMETER Clean
   Remove the previous archive before packaging.

.PARAMETER ExtraUatArgs
   Extra arguments appended to the BuildCookRun invocation.

.EXAMPLE
   ./scripts/build_game_standalone.ps1

.EXAMPLE
   ./scripts/build_game_standalone.ps1 -Configuration Shipping -Clean
#>
[CmdletBinding()]
param(
   [string]$EngineRoot,
   [ValidateSet('DebugGame', 'Development', 'Shipping', 'Test')]
   [string]$Configuration = 'Development',
   [string]$OutputDirectory,
   [switch]$Clean,
   [string[]]$ExtraUatArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Uproject = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
if (-not $OutputDirectory) {
   $OutputDirectory = Join-Path $RepoRoot 'artifacts\ApexSim-Win64'
}

function Get-Property {
   param($Object, [string]$Name)

   if ($null -eq $Object) { return $null }
   $property = $Object.PSObject.Properties[$Name]
   if ($null -eq $property) { return $null }
   return $property.Value
}

function Resolve-EngineRoot {
   param([string]$Explicit)

   $candidates = [System.Collections.Generic.List[string]]::new()
   if ($Explicit) { $candidates.Add($Explicit) }
   foreach ($name in 'UE', 'UE_ROOT', 'UE5_ROOT') {
      $value = [Environment]::GetEnvironmentVariable($name)
      if ($value) { $candidates.Add($value) }
   }

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
      if (Test-Path (Join-Path $candidate 'Engine\Build\BatchFiles\RunUAT.bat')) {
         return (Resolve-Path $candidate).Path
      }
   }

   throw ("could not find Unreal Engine $association. Pass -EngineRoot <path>, " +
         'or set $env:UE to the folder containing Engine\. Tried: ' +
         ($candidates -join '; '))
}

if (-not (Test-Path $Uproject)) {
   throw "expected the Unreal project at $Uproject"
}

$engine = Resolve-EngineRoot $EngineRoot
$uat = Join-Path $engine 'Engine\Build\BatchFiles\RunUAT.bat'
$output = [IO.Path]::GetFullPath($OutputDirectory)

if ($Clean -and (Test-Path $output)) {
   Write-Host "Removing previous archive: $output" -ForegroundColor DarkGray
   Remove-Item -LiteralPath $output -Recurse -Force
}

$uatArgs = @(
   'BuildCookRun',
   "-project=$Uproject",
   '-noP4',
   '-platform=Win64',
   "-clientconfig=$Configuration",
   '-build',
   '-cook',
   '-stage',
   '-pak',
   '-archive',
   "-archivedirectory=$output",
   '-unattended'
)
if ($ExtraUatArgs) { $uatArgs += $ExtraUatArgs }

Write-Host "Unreal Engine: $engine" -ForegroundColor DarkGray
Write-Host "Archive: $output" -ForegroundColor DarkGray
Write-Host ''
Write-Host "==> Packaging ApexSim ($Configuration, Win64)" -ForegroundColor Cyan
Write-Host "    $uat $($uatArgs -join ' ')" -ForegroundColor DarkGray

& $uat @uatArgs
if ($LASTEXITCODE -ne 0) {
   throw "BuildCookRun failed with exit code $LASTEXITCODE"
}

$executable = Get-ChildItem -Path $output -Filter 'ApexSim.exe' -Recurse -File |
   Select-Object -First 1
if ($null -eq $executable) {
   throw "BuildCookRun completed but did not produce ApexSim.exe under $output"
}

Write-Host ''
Write-Host '==> Done' -ForegroundColor Cyan
Write-Host "    Standalone game: $($executable.FullName)"

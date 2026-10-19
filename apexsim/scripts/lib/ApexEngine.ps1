<#
.SYNOPSIS
    Shared helpers for the ApexSim build scripts.

.DESCRIPTION
    Dot-source this from a script in scripts/:

        . (Join-Path $PSScriptRoot 'lib\ApexEngine.ps1')

    It provides the Unreal Engine lookup every packaging script needs, so the
    fallback order (explicit path, environment, the .uproject's
    EngineAssociation, the launcher's default install locations) is written
    down once instead of drifting between copies.
#>

# Property access that tolerates a missing member under Set-StrictMode, which
# ConvertFrom-Json objects hit whenever an optional key is absent.
function Get-ApexProperty {
    param($Object, [string]$Name)

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

<#
.SYNOPSIS
    Locate an Unreal Engine install that can build the given project.

.PARAMETER Uproject
    Path to the .uproject, read for its EngineAssociation.

.PARAMETER Requires
    Engine-relative path to the tool the caller is about to run, e.g.
    'Engine\Build\BatchFiles\RunUAT.bat' or
    'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'. A candidate only wins if it
    actually contains that file.

.PARAMETER Explicit
    A -EngineRoot the user passed; tried first.
#>
function Resolve-ApexEngineRoot {
    param(
        [Parameter(Mandatory)][string]$Uproject,
        [Parameter(Mandatory)][string]$Requires,
        [string]$Explicit
    )

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($Explicit) { $candidates.Add($Explicit) }
    foreach ($name in 'UE', 'UE_ROOT', 'UE5_ROOT') {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $candidates.Add($value) }
    }

    # The .uproject's EngineAssociation ("5.8") doubles as the registry key
    # name a launcher install writes its location under.
    $association = Get-ApexProperty (Get-Content $Uproject -Raw | ConvertFrom-Json) 'EngineAssociation'
    if ($association) {
        foreach ($hive in 'HKLM:\SOFTWARE\EpicGames\Unreal Engine',
                          'HKCU:\SOFTWARE\EpicGames\Unreal Engine') {
            $key = Join-Path $hive $association
            if (Test-Path $key) {
                $installed = Get-ApexProperty (Get-ItemProperty $key) 'InstalledDirectory'
                if ($installed) { $candidates.Add($installed) }
            }
        }
        $candidates.Add("C:\Program Files\Epic Games\UE_$association")
        $candidates.Add("C:\Epic Games\UE_$association")
    }

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate $Requires)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw ("could not find Unreal Engine $association. Pass -EngineRoot <path>, " +
           'or set $env:UE to the folder containing Engine\. Tried: ' +
           ($candidates -join '; '))
}

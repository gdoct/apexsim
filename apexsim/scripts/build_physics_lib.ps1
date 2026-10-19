<#
.SYNOPSIS
    Build the client's physics library (server/ffi) and put it where the game links it.

.DESCRIPTION
    Runs `cargo build --release` in server/ffi and copies apexsim_physics.lib to
    game-unreal/Source/ThirdParty/ApexPhysics/lib/Win64/, where the ApexPhysics
    module links it into the client for the prediction of the local car
    (docs/proposals/client-prediction.md). Build the client afterwards. A
    client built without it runs with prediction unavailable.

    play_editor.ps1 -Build, initialize_content.ps1 and build_game_standalone.ps1
    run this step themselves. Honours CARGO_TARGET_DIR.

.EXAMPLE
    ./scripts/build_physics_lib.ps1
#>
[CmdletBinding()]
param(
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\ApexPhysicsLib.ps1')

Write-Host '==> Building the physics library (server/ffi)' -ForegroundColor Cyan
Build-ApexPhysicsLib -RepoRoot $RepoRoot -DryRun:$DryRun

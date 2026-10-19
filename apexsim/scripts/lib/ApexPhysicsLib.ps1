# The client's copy of the simulation's car step: server/ffi (crate
# apexsim-physics, on apexsim-sim) built as a static library and dropped where the
# ApexPhysics third-party module links it
# (game-unreal/Source/ThirdParty/ApexPhysics, docs/building.md).
#
# The .lib is built from the simulation's source on demand and never checked
# in (*.lib is gitignored): it is 36 MB, it is only right for the source
# beside it, and cargo rebuilds it in seconds when nothing changed.

function Get-ApexPhysicsLibPath {
    param([Parameter(Mandatory)][string]$RepoRoot)
    return Join-Path $RepoRoot 'game-unreal\Source\ThirdParty\ApexPhysics\lib\Win64\apexsim_physics.lib'
}

# Where cargo leaves the release .lib: CARGO_TARGET_DIR when set, else the
# crate's own target folder.
function Get-ApexPhysicsCargoOutput {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $target = if ($env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR } else { Join-Path $RepoRoot 'server\ffi\target' }
    return Join-Path $target 'release\apexsim_physics.lib'
}

# Builds the library (incremental) and copies it into the module. Touches
# ApexPhysics.Build.cs when the library appears, so UBT reads the rules again
# and links it (it caches them until a rules file changes). Throws when cargo
# fails.
function Build-ApexPhysicsLib {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$DryRun
    )
    $dest = Get-ApexPhysicsLibPath -RepoRoot $RepoRoot
    if ($DryRun) {
        Write-Host "    cargo build --release in server\ffi -> $dest" -ForegroundColor DarkGray
        return
    }
    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
        throw 'cargo is not on PATH (install Rust): the physics library is built from server/ffi'
    }
    Push-Location (Join-Path $RepoRoot 'server\ffi')
    # cargo reports progress on stderr: under a redirected caller, Windows
    # PowerShell turns each line into an error record, and 'Stop' would end
    # the build at the first one. The exit code says whether it failed.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & cargo build --release
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
        Pop-Location
    }
    if ($code -ne 0) { throw "cargo build --release in server/ffi failed with exit code $code" }
    $built = Get-ApexPhysicsCargoOutput -RepoRoot $RepoRoot
    if (-not (Test-Path $built)) {
        throw "cargo built server/ffi but there is no $built"
    }
    $hadLibrary = Test-Path $dest
    $same = $hadLibrary -and (Get-Item $dest).Length -eq (Get-Item $built).Length -and
        (Get-FileHash $dest).Hash -eq (Get-FileHash $built).Hash
    if (-not $same) {
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        Copy-Item -LiteralPath $built -Destination $dest -Force
        Write-Host "    physics library -> $dest" -ForegroundColor DarkGray
    }
    if (-not $hadLibrary) {
        $rules = Join-Path $RepoRoot 'game-unreal\Source\ThirdParty\ApexPhysics\ApexPhysics.Build.cs'
        (Get-Item $rules).LastWriteTime = Get-Date
    }
}

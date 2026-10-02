<#
.SYNOPSIS
   Build, cook, package, and archive the ApexSim Windows client.

.DESCRIPTION
   Runs Unreal Automation Tool's BuildCookRun command to produce a portable
   Win64 build. By default the archive is written to artifacts\ApexSim-Win64
   and contains ApexSim.exe plus its required runtime files.

   The circuits are not cooked: the game builds each one from its export at
   runtime (docs/RUNTIME_CONTENT_LOADING.md). After packaging, the exports in
   build/tracks (run scripts/build_track_levels.ps1 first) and their
   previews are copied into Tracks\ beside ApexSim.exe, where the game looks.

   Neither are the cars: the game builds each from its car.toml and GLBs at
   runtime. Every content/cars folder's car.toml, model, DRS flap and livery
   logos are copied into Cars\, and the class wheels into Wheels\.

   The HUD is files too (docs/HUD_MODDING.md): content\hud\default is
   copied into Hud\default, with an empty Hud\custom (and its README) for
   the player's own components.

   The showcases (docs/SPECTATOR.md) are the rendered AI races the menu
   plays behind its screens when no server streams one: every .apxs in
   build\showcase (run scripts/build_track_levels.ps1 first) is copied into
   Showcase\ beside ApexSim.exe, where the game looks.

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

.PARAMETER SkipTracks
   Do not copy the track exports next to the executable.

.PARAMETER IncludeCustomTracks
   Also copy the exports of the tracks in content\tracks\custom. Off by
   default: that folder is the player's own, and may hold circuits converted
   from content that must not be redistributed.

.PARAMETER IncludeCustomCars
   Also copy the cars in content\cars\custom. Off by default, for the same
   reason as -IncludeCustomTracks.

.PARAMETER SkipCars
   Do not copy the cars and wheels next to the executable.

.PARAMETER SkipHud
   Do not copy the HUD components (content\hud\default) into Hud\ next to
   the executable. The game then races with no HUD.

.PARAMETER SkipShowcase
   Do not copy the showcases (build\showcase\*.apxs) into Showcase\ next to
   the executable. The run still checks that every showcase
   content\showcase.yml lists has been rendered, like -SkipTracks does for
   the exports, so a package is never quietly short of them; the menu then
   plays only what a server streams.

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
   [switch]$SkipTracks,
   [switch]$IncludeCustomTracks,
   [switch]$IncludeCustomCars,
   [switch]$SkipCars,
   [switch]$SkipHud,
   [switch]$SkipShowcase,
   [string[]]$ExtraUatArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\ApexEngine.ps1')
. (Join-Path $PSScriptRoot 'lib\ApexCars.ps1')
. (Join-Path $PSScriptRoot 'lib\ApexTracks.ps1')
. (Join-Path $PSScriptRoot 'lib\ApexShowcase.ps1')

$Uproject = Join-Path $RepoRoot 'game-unreal\ApexSim.uproject'
if (-not $OutputDirectory) {
   $OutputDirectory = Join-Path $RepoRoot 'artifacts\ApexSim-Win64'
}

if (-not (Test-Path $Uproject)) {
   throw "expected the Unreal project at $Uproject"
}

$engine = Resolve-ApexEngineRoot -Uproject $Uproject -Explicit $EngineRoot `
   -Requires 'Engine\Build\BatchFiles\RunUAT.bat'
$uat = Join-Path $engine 'Engine\Build\BatchFiles\RunUAT.bat'
$output = [IO.Path]::GetFullPath($OutputDirectory)

# A showcase that was never rendered is checked for before the long cook,
# whether or not it is copied: a package short of one has a menu with
# nothing behind it on that circuit.
$missingShowcases = @(Get-ApexMissingShowcases -RepoRoot $RepoRoot)
if ($missingShowcases.Count -gt 0) {
   throw ("these showcases have not been rendered under $(Get-ApexShowcaseDir -RepoRoot $RepoRoot) " +
      "(run scripts/build_track_levels.ps1): $($missingShowcases -join ', ')")
}

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

if (-not $SkipTracks) {
   # Where UApexTrackContentSubsystem looks in a packaged build: Tracks\ next
   # to ApexSim.exe (and settings.yml).
   $exportDir = Join-Path $RepoRoot 'build\tracks'
   $tracksOut = Join-Path $executable.DirectoryName 'Tracks'
   Write-Host ''
   Write-Host "==> Copying the track exports to $tracksOut" -ForegroundColor Cyan
   if (Test-Path $tracksOut) { Remove-Item -LiteralPath $tracksOut -Recurse -Force }
   New-Item -ItemType Directory -Path $tracksOut -Force | Out-Null
   # Only the exports of the circuits being shipped: build\tracks
   # also holds the player's own (content\tracks\custom) once baked.
   $shipped = @(Get-ApexTrackFiles -RepoRoot $RepoRoot -DefaultOnly:(-not $IncludeCustomTracks) |
      ForEach-Object { $_.BaseName })
   $manifests = @(Get-ChildItem $exportDir -Filter '*.uescene.json' -File -ErrorAction SilentlyContinue |
      Where-Object { $shipped -contains ($_.Name -replace '\.uescene\.json$', '') })
   foreach ($manifest in $manifests) {
      $stem = $manifest.Name -replace '\.uescene\.json$', ''
      $blob = Join-Path $exportDir "$stem.uemesh"
      if (-not (Test-Path -LiteralPath $blob)) {
         Write-Warning "$stem has no $stem.uemesh (an export from before the mesh blob); re-run build_track_levels.ps1"
         continue
      }
      Copy-Item -LiteralPath $manifest.FullName -Destination $tracksOut -Force
      Copy-Item -LiteralPath $blob -Destination $tracksOut -Force
      $preview = Join-Path $exportDir "previews\$stem.png"
      if (Test-Path -LiteralPath $preview) {
         Copy-Item -LiteralPath $preview -Destination (Join-Path $tracksOut "$stem.png") -Force
      }
      # An imported circuit's textures (scripts/ac_import.py) sit in a
      # folder beside its manifest, which the manifest names by relative path.
      $textures = Join-Path $exportDir "$stem.textures"
      if (Test-Path -LiteralPath $textures) {
         Copy-Item -LiteralPath $textures -Destination (Join-Path $tracksOut "$stem.textures") -Recurse -Force
      }
   }
   if ($manifests.Count -eq 0) {
      Write-Warning "no track exports in $exportDir; the game will have nothing to race on (run scripts/build_track_levels.ps1)"
   }
   else {
      Write-Host "    $($manifests.Count) track(s)" -ForegroundColor DarkGray
   }
}

if (-not $SkipCars) {
   # Where UApexCarContentSubsystem looks in a packaged build: Cars\ and
   # Wheels\ next to ApexSim.exe (Cars\default and Cars\custom, as in the repo).
   Write-Host ''
   Write-Host "==> Copying the cars to $(Join-Path $executable.DirectoryName 'Cars')" -ForegroundColor Cyan
   $carCount = Copy-ApexRuntimeCars -CarsDir (Join-Path $RepoRoot 'content\cars') `
      -WheelsDir (Join-Path $RepoRoot 'content\wheels') -Destination $executable.DirectoryName `
      -DefaultOnly:(-not $IncludeCustomCars)
   if ($carCount -eq 0) {
      Write-Warning 'no cars in content\cars; every car will be drawn as nothing'
   }
   else {
      Write-Host "    $carCount car(s)" -ForegroundColor DarkGray
   }
}

if (-not $SkipHud) {
   # Where the HUD host looks in a packaged build (ApexHud::HudDirectories):
   # Hud\ next to ApexSim.exe. Only the shipped components: content\hud\custom
   # is this machine's own, and Hud\custom is left for the player's.
   $hudSource = Join-Path $RepoRoot 'content\hud'
   $hudOut = Join-Path $executable.DirectoryName 'Hud'
   Write-Host ''
   Write-Host "==> Copying the HUD to $hudOut" -ForegroundColor Cyan
   if (Test-Path -LiteralPath $hudOut) { Remove-Item -LiteralPath $hudOut -Recurse -Force }
   New-Item -ItemType Directory -Path (Join-Path $hudOut 'custom') -Force | Out-Null
   Copy-Item -LiteralPath (Join-Path $hudSource 'default') -Destination (Join-Path $hudOut 'default') -Recurse -Force
   $readme = Join-Path $hudSource 'custom\README.md'
   if (Test-Path -LiteralPath $readme) { Copy-Item -LiteralPath $readme -Destination (Join-Path $hudOut 'custom') -Force }
   $hudCount = @(Get-ChildItem -LiteralPath (Join-Path $hudOut 'default') -Filter 'component.json' -Recurse -File).Count
   if ($hudCount -eq 0) {
      Write-Warning 'no HUD components in content\hud\default; races will have no HUD'
   }
   else {
      Write-Host "    $hudCount component(s)" -ForegroundColor DarkGray
   }
}

if (-not $SkipShowcase) {
   # Where the menu's local backdrop looks in a packaged build: Showcase\
   # next to ApexSim.exe (docs/SPECTATOR.md). Every rendered showcase, the
   # pipeline's and any rendered by hand.
   $showcaseOut = Join-Path $executable.DirectoryName 'Showcase'
   Write-Host ''
   Write-Host "==> Copying the showcases to $showcaseOut" -ForegroundColor Cyan
   $showcaseCount = Copy-ApexShowcases -RepoRoot $RepoRoot -Destination $showcaseOut
   if ($showcaseCount -eq 0) {
      Write-Warning 'no showcases in build\showcase; the menu plays only what a server streams (run scripts/build_track_levels.ps1)'
   }
   else {
      Write-Host "    $showcaseCount showcase(s)" -ForegroundColor DarkGray
   }
}

Write-Host ''
Write-Host '==> Done' -ForegroundColor Cyan
Write-Host "    Standalone game: $($executable.FullName)"

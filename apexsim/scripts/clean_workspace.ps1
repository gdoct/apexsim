<#
.SYNOPSIS
    Reclaims disk space from ApexSim build output, scratch targets, caches and logs.

.DESCRIPTION
    Dry run by default: prints what would go and how large it is. -Apply deletes.

    Default set (cheap to get back, safe to run weekly):
      - scratch cargo targets in a drive root (apexsim-cargo-target*) and the
        repo's target-scratch*/.scratch-target, once older than -KeepDays
      - Claude session scratchpads in %TEMP%\claude\*apexsim*, once older than -KeepDays
      - cargo incremental caches under server/ and ../apexsim-content/track-editor/ target dirs
      - stale %TEMP% leftovers (proc-macro-srv*, apex-target, apex_target, bash-edit-diff)
      - Unreal Saved: crashes, old logs, screenshots, shader debug info, temp
      - __pycache__ folders in the code root and ../apexsim-content/tools
      - unregistered, empty .claude/worktrees folders; worktrees whose commits are
        all in main are reported (and removed only with -RemoveMergedWorktrees)

    -Deep adds what costs a rebuild: server/target and
    ../apexsim-content/track-editor/target, Unreal
    Intermediate/Build, and out/promo, out/site, out/verify.
    -IncludePip adds the pip download cache (not ApexSim's, but large).
    -Install registers a weekly Task Scheduler job for the default set.

    Never touched: ../apexsim-content/build (build/tracks), ../apexsim-content/.cache,
    the content itself (cars, tracks, props, ...), artifacts, .git, server/ffi/target
    (the physics lib the client links), server/replays, records, ~/.claude/projects.
    -RepoRoot is the code root (apexsim/); the content root is its sibling
    ../apexsim-content, and .git and .claude/worktrees are looked for in the code
    root first, then in the folder above it (one repository holding both roots).
    Directory junctions and symlinks are unlinked, never followed.

.EXAMPLE
    pwsh scripts/clean_workspace.ps1                 # dry run
    pwsh scripts/clean_workspace.ps1 -Apply          # default set
    pwsh scripts/clean_workspace.ps1 -Apply -Deep -IncludePip
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$Deep,
    [switch]$IncludePip,
    [switch]$RemoveMergedWorktrees,
    [int]$KeepDays = 2,
    [int]$ScreenshotDays = 7,
    [string]$RepoRoot = '',
    [switch]$Install,
    [string]$InstallTime = '04:00'
)

$ErrorActionPreference = 'Continue'
if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path.TrimEnd('\')
$ContentRoot = [IO.Path]::GetFullPath((Join-Path $RepoRoot '..\apexsim-content')).TrimEnd('\')
# The git checkout: the code root once it is a repository of its own, else the
# folder above it that holds both roots.
$GitRoot = $RepoRoot
if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot '.git'))) {
    $above = Split-Path -Parent $RepoRoot
    if ($above -and (Test-Path -LiteralPath (Join-Path $above '.git'))) { $GitRoot = $above.TrimEnd('\') }
}

if ($Install) {
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Apply -RepoRoot `"$RepoRoot`""
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
    $trigger = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Sunday -At $InstallTime
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Hours 2)
    Register-ScheduledTask -TaskName 'ApexSim workspace clean' -Action $action -Trigger $trigger `
        -Settings $settings -Description 'scripts/clean_workspace.ps1 -Apply (default set)' -Force | Out-Null
    Write-Host "Registered 'ApexSim workspace clean': Sundays $InstallTime. Remove with: Unregister-ScheduledTask 'ApexSim workspace clean'"
    return
}

$now = Get-Date
$logPath = Join-Path $RepoRoot 'out\clean_workspace.log'
function Log([string]$m) {
    Write-Host $m
    if ($Apply) {
        try {
            New-Item -ItemType Directory -Force -Path (Split-Path $logPath) | Out-Null
            Add-Content -LiteralPath $logPath -Value ("{0:s} {1}" -f (Get-Date), $m)
        } catch { }
    }
}
function Fmt([long]$b) { if ($b -ge 1GB) { '{0:N1} GB' -f ($b / 1GB) } elseif ($b -ge 1MB) { '{0:N0} MB' -f ($b / 1MB) } else { '{0:N0} KB' -f ($b / 1KB) } }

# One walk: total bytes, newest file time, and every link met (never followed).
function Get-TreeInfo([string]$Path) {
    $root = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $root) { return $null }
    if (-not $root.PSIsContainer) {
        return [pscustomobject]@{ Bytes = $root.Length; Newest = $root.LastWriteTime; Links = @() }
    }
    $bytes = 0L; $newest = $root.LastWriteTime; $links = New-Object System.Collections.Generic.List[string]
    $stack = New-Object System.Collections.Generic.Stack[System.IO.DirectoryInfo]
    $stack.Push([System.IO.DirectoryInfo]$root)
    while ($stack.Count) {
        $d = $stack.Pop()
        try {
            foreach ($e in $d.EnumerateFileSystemInfos()) {
                if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) { $links.Add($e.FullName); continue }
                if ($e -is [System.IO.DirectoryInfo]) { $stack.Push($e) }
                else {
                    $bytes += $e.Length
                    if ($e.LastWriteTime -gt $newest) { $newest = $e.LastWriteTime }
                }
            }
        } catch { }
    }
    [pscustomobject]@{ Bytes = $bytes; Newest = $newest; Links = $links.ToArray() }
}

function Remove-Tree([string]$Path) {
    $info = Get-TreeInfo $Path
    if (-not $info) { return }
    foreach ($l in $info.Links) {
        # unlink only; a junction's target is somebody else's data
        try { [System.IO.Directory]::Delete($l) } catch { try { [System.IO.File]::Delete($l) } catch { } }
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($item -and -not $item.PSIsContainer) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue; return }
    & cmd.exe /c "rd /s /q `"$Path`"" 2>$null | Out-Null
}

$busy = @{
    cargo  = [bool](Get-Process cargo, rustc -ErrorAction SilentlyContinue)
    unreal = [bool](Get-Process UnrealEditor, UnrealEditor-Cmd, UnrealBuildTool -ErrorAction SilentlyContinue)
}

$items = New-Object System.Collections.Generic.List[object]
# Mode: Tree (the path as one unit), Children (each child separately), Files (old files inside)
function Add-Item([string]$Group, [string]$Path, [string]$Mode = 'Tree', [double]$MinAgeDays = 0, [string]$Needs = '') {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if ($Path.TrimEnd('\').Length -lt 8) { return }   # never a drive root or a top-level folder
    $items.Add([pscustomobject]@{ Group = $Group; Path = $Path; Mode = $Mode; MinAgeDays = $MinAgeDays; Needs = $Needs })
}

$tempClaude = Join-Path $env:TEMP 'claude'
$repoDrive = (Split-Path -Qualifier $RepoRoot) + '\'
$sysDrive = $env:SystemDrive + '\'

# --- default set -------------------------------------------------------------
foreach ($root in @($sysDrive, $repoDrive) | Select-Object -Unique) {
    Get-ChildItem -LiteralPath $root -Directory -Filter 'apexsim-cargo-target*' -Force -ErrorAction SilentlyContinue |
        ForEach-Object { Add-Item 'Scratch cargo targets' $_.FullName 'Tree' $KeepDays 'cargo' }
}
foreach ($p in 'target-scratch', 'target-scratch2', '.scratch-target') {
    foreach ($base in $RepoRoot, (Join-Path $RepoRoot 'server'), $ContentRoot, (Join-Path $ContentRoot 'track-editor')) {
        Add-Item 'Scratch cargo targets' (Join-Path $base $p) 'Tree' $KeepDays 'cargo'
    }
}
if (Test-Path $tempClaude) {
    Get-ChildItem -LiteralPath $tempClaude -Directory -Filter '*apexsim*' -Force -ErrorAction SilentlyContinue |
        ForEach-Object { Add-Item 'Claude session scratchpads' $_.FullName 'Children' $KeepDays }
    Add-Item 'Claude session scratchpads' (Join-Path $tempClaude 'bash-edit-diff') 'Children' $KeepDays
}
foreach ($target in (Join-Path $RepoRoot 'server\target'), (Join-Path $ContentRoot 'track-editor\target')) {
    if (-not $Deep -and (Test-Path $target)) {   # -Deep removes the whole target instead
        Get-ChildItem -LiteralPath $target -Directory -Force | ForEach-Object {
            Add-Item 'Cargo incremental caches' (Join-Path $_.FullName 'incremental') 'Tree' 0 'cargo'
        }
    }
}
Get-ChildItem -LiteralPath $env:TEMP -Directory -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like 'proc-macro-srv*' -or $_.Name -in 'apex-target', 'apex_target' } |
    ForEach-Object { Add-Item 'Stale %TEMP%' $_.FullName 'Tree' $KeepDays }
$saved = Join-Path $RepoRoot 'game-unreal\Saved'
Add-Item 'Unreal Saved' (Join-Path $saved 'Crashes') 'Children' $KeepDays
Add-Item 'Unreal Saved' (Join-Path $saved 'Logs') 'Files' $KeepDays
Add-Item 'Unreal Saved' (Join-Path $saved 'Screenshots') 'Files' $ScreenshotDays
Add-Item 'Unreal Saved' (Join-Path $saved 'ShaderDebugInfo') 'Children' $KeepDays
Add-Item 'Unreal Saved' (Join-Path $saved 'Temp') 'Children' $KeepDays
foreach ($pyRoot in $RepoRoot, (Join-Path $ContentRoot 'tools')) {
    if (-not (Test-Path -LiteralPath $pyRoot)) { continue }
    Get-ChildItem -LiteralPath $pyRoot -Directory -Recurse -Force -Filter '__pycache__' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(target|\.git|\.cache|content|node_modules)\\' } |
        ForEach-Object { Add-Item '__pycache__' $_.FullName 'Tree' 0 }
}

# --- deep set ----------------------------------------------------------------
if ($Deep) {
    # server/ffi/target is deliberately absent: the client links the lib built there
    Add-Item 'Deep: cargo targets' (Join-Path $RepoRoot 'server\target') 'Tree' 0 'cargo'
    Add-Item 'Deep: cargo targets' (Join-Path $ContentRoot 'track-editor\target') 'Tree' 0 'cargo'
    Add-Item 'Deep: Unreal intermediates' (Join-Path $RepoRoot 'game-unreal\Intermediate\Build') 'Tree' 0 'unreal'
    foreach ($o in 'promo', 'site', 'verify') { Add-Item 'Deep: regenerable out/' (Join-Path $RepoRoot "out\$o") 'Tree' 0 }
}
if ($IncludePip) {
    foreach ($o in 'http', 'http-v2', 'wheels') { Add-Item 'pip cache' (Join-Path $env:LOCALAPPDATA "pip\cache\$o") 'Tree' 0 }
}

# --- run ---------------------------------------------------------------------
$beforeFree = @{}
foreach ($d in $sysDrive, $repoDrive | Select-Object -Unique) { $beforeFree[$d] = (Get-PSDrive ($d.Substring(0, 1))).Free }

Log ("clean_workspace {0} (KeepDays={1}, Deep={2}, IncludePip={3}) repo={4}" -f $(if ($Apply) { 'APPLY' } else { 'DRY RUN' }), $KeepDays, $Deep, $IncludePip, $RepoRoot)
$totals = [ordered]@{}
$smallCount = 0
foreach ($it in $items) {
    $targets = @()
    if ($it.Mode -eq 'Children') {
        $targets = @(Get-ChildItem -LiteralPath $it.Path -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    } else { $targets = @($it.Path) }

    foreach ($t in $targets) {
        if ($it.Mode -eq 'Files') {
            $cut = $now.AddDays(-$it.MinAgeDays)
            $old = @(Get-ChildItem -LiteralPath $t -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cut -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) })
            $bytes = [long]($old | Measure-Object Length -Sum).Sum
            if ($old.Count -eq 0) { continue }
            $label = "{0} old files in {1}" -f $old.Count, $t
            if (-not $Apply) { Log ("  would remove {0,9}  {1}" -f (Fmt $bytes), $label); $freed = $bytes }
            else {
                $freed = 0L
                foreach ($f in $old) { try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop; $freed += $f.Length } catch { } }
                Get-ChildItem -LiteralPath $t -Recurse -Directory -Force -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending |
                    Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue) } |
                    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
                Log ("  removed      {0,9}  {1}" -f (Fmt $freed), $label)
            }
        } else {
            $info = Get-TreeInfo $t
            if (-not $info) { continue }
            $age = ($now - $info.Newest).TotalDays
            if ($age -lt $it.MinAgeDays) { continue }
            if ($it.Needs -and $busy[$it.Needs]) { Log ("  SKIP (a {0} process is running) {1}" -f $it.Needs, $t); continue }
            $small = $info.Bytes -lt 1MB   # tiny leftovers are counted, not listed
            if (-not $Apply) {
                if ($small) { $smallCount++ } else { Log ("  would remove {0,9}  {1}  (newest file {2:N0} d old)" -f (Fmt $info.Bytes), $t, $age) }
                $freed = $info.Bytes
            } else {
                Remove-Tree $t
                $left = Get-TreeInfo $t
                $freed = $info.Bytes - $(if ($left) { $left.Bytes } else { 0 })
                if ($left) { Log ("  PARTIAL      {0,9}  {1} (locked files remain)" -f (Fmt $freed), $t) }
                elseif ($small) { $smallCount++ }
                else { Log ("  removed      {0,9}  {1}" -f (Fmt $freed), $t) }
            }
        }
        $totals[$it.Group] = [long]$totals[$it.Group] + $freed
    }
}

if ($smallCount) { Log ("  {0} {1} small items (< 1 MB each)" -f $(if ($Apply) { 'removed' } else { 'would remove' }), $smallCount) }

# --- worktrees ---------------------------------------------------------------
$wtRoot = Join-Path $GitRoot '.claude\worktrees'
if (Test-Path -LiteralPath (Join-Path $GitRoot '.git')) {
    if ($Apply) { & git -C $GitRoot worktree prune 2>$null }
    $registered = @(& git -C $GitRoot worktree list --porcelain 2>$null | Where-Object { $_ -like 'worktree *' } |
            ForEach-Object { ($_ -replace '^worktree ', '').Replace('/', '\').TrimEnd('\').ToLowerInvariant() })
    if (Test-Path $wtRoot) {
        foreach ($d in Get-ChildItem -LiteralPath $wtRoot -Directory -Force) {
            $info = Get-TreeInfo $d.FullName
            if ($registered -notcontains $d.FullName.TrimEnd('\').ToLowerInvariant()) {
                if ($info.Bytes -lt 1MB) {
                    Log ("  {0} unregistered, empty worktree folder {1}" -f $(if ($Apply) { 'removed' } else { 'would remove' }), $d.FullName)
                    if ($Apply) { Remove-Tree $d.FullName }
                } else { Log ("  NOTE unregistered worktree folder with content, left alone: {0} ({1})" -f $d.FullName, (Fmt $info.Bytes)) }
                continue
            }
            $branch = (& git -C $d.FullName rev-parse --abbrev-ref HEAD 2>$null)
            $dirty = (& git -C $d.FullName status --porcelain 2>$null | Measure-Object).Count
            $unmerged = @(& git -C $d.FullName cherry main 2>$null | Where-Object { $_ -like '+*' }).Count
            Log ("  worktree {0} [{1}] {2}, {3} uncommitted, {4} commits not in main" -f $d.Name, $branch, (Fmt $info.Bytes), $dirty, $unmerged)
            if ($RemoveMergedWorktrees -and $Apply -and $dirty -eq 0 -and $unmerged -eq 0) {
                & git -C $GitRoot worktree remove $d.FullName   # no --force, on purpose
                if (-not (Test-Path $d.FullName)) { Log "  removed worktree $($d.Name)"; $totals['Worktrees'] = [long]$totals['Worktrees'] + $info.Bytes }
            }
        }
    }
}

# --- summary -----------------------------------------------------------------
Log ''
$sum = 0L
foreach ($k in $totals.Keys) { Log ("  {0,-30} {1,9}" -f $k, (Fmt $totals[$k])); $sum += $totals[$k] }
Log ("{0} {1}" -f $(if ($Apply) { 'Reclaimed:' } else { 'Would reclaim:' }), (Fmt $sum))
foreach ($d in $beforeFree.Keys) {
    $now2 = (Get-PSDrive ($d.Substring(0, 1))).Free
    Log ("  {0} free: {1} -> {2}" -f $d, (Fmt $beforeFree[$d]), (Fmt $now2))
}
if (-not $Apply) { Log 'Dry run: nothing was deleted. Re-run with -Apply.' }

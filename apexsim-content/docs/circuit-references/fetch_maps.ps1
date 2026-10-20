# Downloads every map listed in the <Stem>.sources.json files beside this script
# into maps/<Stem>/. Files already present are skipped; pass -Force to refetch.
#
#   powershell -ExecutionPolicy Bypass -File apexsim-content/docs/circuit-references/fetch_maps.ps1
#   ... -Stem SpaFrankenchamps     # one circuit
param(
    [string]$Stem = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13

$root = $PSScriptRoot
$pattern = if ($Stem) { "$Stem.sources.json" } else { "*.sources.json" }
$ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"

$ok = 0; $skipped = 0; $failed = @()
foreach ($json in Get-ChildItem -Path $root -Filter $pattern) {
    foreach ($src in (Get-Content $json.FullName -Raw | ConvertFrom-Json)) {
        if (-not $src.url -or $src.url -like "local:*") { continue }
        $dest = Join-Path $root $src.file
        if ((Test-Path $dest) -and -not $Force) { $skipped++; continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
        try {
            Invoke-WebRequest -Uri $src.url -OutFile $dest -UserAgent $ua -MaximumRedirection 5 -TimeoutSec 120
            Write-Host "ok    $($src.file)"
            $ok++
        } catch {
            if (Test-Path $dest) { Remove-Item $dest }
            Write-Warning "fail  $($src.file)  <- $($src.url)  ($($_.Exception.Message))"
            $failed += $src.url
        }
    }
}
Write-Host ""
Write-Host "$ok downloaded, $skipped already present, $($failed.Count) failed"

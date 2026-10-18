# build the server executable
echo "Building the server executable..."
# ensure we are in the project root
$currentPath = Get-Location
$RepoRoot     = Split-Path -Parent $PSScriptRoot
cd $RepoRoot/server
cargo build --release
cd $currentPath
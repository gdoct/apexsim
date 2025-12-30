#!/bin/bash
# Generate VSCode project files for ApexSim
# Run this after creating the UE project or when switching branches

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_FILE="$PROJECT_DIR/ApexSim.uproject"
UE_ROOT="${UE_ROOT:-$HOME/UnrealEngine}"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== Generating VSCode Project Files ===${NC}"

if [ ! -f "$PROJECT_FILE" ]; then
    echo -e "${YELLOW}Warning: ApexSim.uproject not found${NC}"
    echo -e "Create the Unreal project first, then run this script"
    exit 1
fi

"$UE_ROOT/Engine/Build/BatchFiles/Linux/GenerateProjectFiles.sh" \
    -project="$PROJECT_FILE" \
    -game \
    -engine \
    -vscode

echo -e "${GREEN}VSCode project files generated!${NC}"
echo -e "Reload VSCode to apply changes: ${YELLOW}Ctrl+Shift+P → Reload Window${NC}"

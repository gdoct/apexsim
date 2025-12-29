#!/bin/bash
# Build script for ApexSim Unreal Engine project
# Usage: ./build_project.sh [Development|Shipping|DebugGame]

set -e

# Configuration
PROJECT_NAME="ApexSim"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_FILE="$PROJECT_DIR/$PROJECT_NAME.uproject"
UE_ROOT="${UE_ROOT:-$HOME/UnrealEngine}"
BUILD_CONFIG="${1:-Development}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== Building ApexSim ===${NC}"
echo -e "Project: ${YELLOW}$PROJECT_FILE${NC}"
echo -e "Configuration: ${YELLOW}$BUILD_CONFIG${NC}"
echo -e "UE Root: ${YELLOW}$UE_ROOT${NC}\n"

# Verify UE installation
if [ ! -d "$UE_ROOT" ]; then
    echo -e "${RED}Error: Unreal Engine not found at $UE_ROOT${NC}"
    echo -e "Set UE_ROOT environment variable to your UE installation path"
    exit 1
fi

# Verify project file exists
if [ ! -f "$PROJECT_FILE" ]; then
    echo -e "${RED}Error: Project file not found: $PROJECT_FILE${NC}"
    echo -e "Create the Unreal project first (see SETUP.md)"
    exit 1
fi

# Build Editor target
echo -e "${YELLOW}Building Editor...${NC}"
"$UE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh" \
    "${PROJECT_NAME}Editor" \
    Linux \
    "$BUILD_CONFIG" \
    -project="$PROJECT_FILE" \
    -waitmutex \
    -verbose

echo -e "\n${GREEN}Build complete!${NC}"
echo -e "Launch editor with: ${YELLOW}$UE_ROOT/Engine/Binaries/Linux/UnrealEditor $PROJECT_FILE${NC}"

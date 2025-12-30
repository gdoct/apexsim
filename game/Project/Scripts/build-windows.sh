#!/bin/bash
set -e

# ApexSim Windows Build Script
# Builds Windows executable from Linux/WSL

PROJECT_PATH="/home/guido/apexsim/frontend/Project/ApexSim.uproject"
UE_ROOT="$HOME/unreal-engine/UnrealEngine"

# Default to Development build
CONFIG="${1:-Development}"
BUILD_DIR="/home/guido/apexsim/builds/windows-${CONFIG,,}"

echo "========================================="
echo "Building ApexSim for Windows"
echo "Configuration: $CONFIG"
echo "Output: $BUILD_DIR"
echo "========================================="
echo ""

cd "$UE_ROOT"

# Run the build
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="$PROJECT_PATH" \
  -platform=Win64 \
  -clientconfig="$CONFIG" \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="$BUILD_DIR" \
  -utf8output

echo ""
echo "========================================="
echo "Build Complete!"
echo "========================================="
echo "Output location: $BUILD_DIR"
echo ""
echo "Windows path: \\\\wsl\$\\Ubuntu$BUILD_DIR"
echo ""
echo "Executable: $BUILD_DIR/WindowsNoEditor/ApexSim.exe"
echo "========================================="

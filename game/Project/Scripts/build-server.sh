#!/bin/bash
set -e

# ApexSim Dedicated Server Build Script
# Builds a headless dedicated server for Linux

PROJECT_PATH="/home/guido/apexsim/frontend/Project/ApexSim.uproject"
UE_ROOT="$HOME/unreal-engine/UnrealEngine"

# Server is always Shipping config
CONFIG="Shipping"
BUILD_DIR="/home/guido/apexsim/builds/server"

echo "========================================="
echo "Building ApexSim Dedicated Server"
echo "Configuration: $CONFIG"
echo "Output: $BUILD_DIR"
echo "========================================="
echo ""

cd "$UE_ROOT"

# Run the build
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="$PROJECT_PATH" \
  -platform=Linux \
  -serverconfig="$CONFIG" \
  -server \
  -noclient \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="$BUILD_DIR" \
  -utf8output

echo ""
echo "========================================="
echo "Server Build Complete!"
echo "========================================="
echo "Output location: $BUILD_DIR"
echo ""
echo "Server executable: $BUILD_DIR/LinuxServer/ApexSimServer"
echo "========================================="

# Building ApexSim Frontend

## Cross-Platform Build Guide

This guide covers building the ApexSim Unreal Engine client for different platforms using WSL/Linux.

---

## Building Windows Executables from Linux

Yes! You can cross-compile Windows builds from Linux using Unreal Engine's cross-compilation toolchain.

### Prerequisites

1. **Install Windows Cross-Compilation Tools**

```bash
# Install the cross-compilation toolchain
cd ~/unreal-engine/UnrealEngine
./Engine/Extras/ThirdPartyNotUE/DotNetCore/Linux/install.sh

# Download Windows SDK and tools
./Setup.sh
```

2. **Install Wine (Optional, for testing)**

```bash
sudo apt install wine64
```

### Building for Windows

#### Development Build

```bash
cd ~/unreal-engine/UnrealEngine

# Build the editor target for Windows
./Engine/Build/BatchFiles/Linux/Build.sh ApexSimEditor Win64 Development \
  -project="/home/guido/apexsim/frontend/ApexSim.uproject"
```

#### Shipping Build (Final Release)

```bash
# Package the game for Windows
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="/home/guido/apexsim/frontend/ApexSim.uproject" \
  -platform=Win64 \
  -clientconfig=Shipping \
  -serverconfig=Shipping \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="/home/guido/apexsim/builds/windows"
```

#### Server Build (Dedicated Server)

```bash
# Build dedicated server for Windows
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="/home/guido/apexsim/frontend/ApexSim.uproject" \
  -platform=Win64 \
  -serverconfig=Shipping \
  -server \
  -noclient \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="/home/guido/apexsim/builds/windows-server"
```

---

## Building Linux Executables

### Development Build

```bash
cd ~/unreal-engine/UnrealEngine

./Engine/Build/BatchFiles/Linux/Build.sh ApexSimEditor Linux Development \
  -project="/home/guido/apexsim/frontend/ApexSim.uproject"
```

### Shipping Build

```bash
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="/home/guido/apexsim/frontend/ApexSim.uproject" \
  -platform=Linux \
  -clientconfig=Shipping \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="/home/guido/apexsim/builds/linux"
```

### Linux Dedicated Server

```bash
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="/home/guido/apexsim/frontend/ApexSim.uproject" \
  -platform=Linux \
  -serverconfig=Shipping \
  -server \
  -noclient \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="/home/guido/apexsim/builds/linux-server"
```

---

## Quick Build Scripts

### Create Build Scripts

Save these to `~/apexsim/frontend/Scripts/`:

**`build-windows.sh`**
```bash
#!/bin/bash
set -e

PROJECT_PATH="/home/guido/apexsim/frontend/ApexSim.uproject"
BUILD_DIR="/home/guido/apexsim/builds/windows"
UE_ROOT="~/unreal-engine/UnrealEngine"

cd $UE_ROOT

echo "Building ApexSim for Windows..."

./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="$PROJECT_PATH" \
  -platform=Win64 \
  -clientconfig=Shipping \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="$BUILD_DIR"

echo "Build complete! Output: $BUILD_DIR"
```

**`build-linux.sh`**
```bash
#!/bin/bash
set -e

PROJECT_PATH="/home/guido/apexsim/frontend/ApexSim.uproject"
BUILD_DIR="/home/guido/apexsim/builds/linux"
UE_ROOT="~/unreal-engine/UnrealEngine"

cd $UE_ROOT

echo "Building ApexSim for Linux..."

./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="$PROJECT_PATH" \
  -platform=Linux \
  -clientconfig=Shipping \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="$BUILD_DIR"

echo "Build complete! Output: $BUILD_DIR"
```

**`build-server.sh`**
```bash
#!/bin/bash
set -e

PROJECT_PATH="/home/guido/apexsim/frontend/ApexSim.uproject"
BUILD_DIR="/home/guido/apexsim/builds/server"
UE_ROOT="~/unreal-engine/UnrealEngine"

cd $UE_ROOT

echo "Building ApexSim Dedicated Server..."

./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -project="$PROJECT_PATH" \
  -platform=Linux \
  -serverconfig=Shipping \
  -server \
  -noclient \
  -cook \
  -build \
  -stage \
  -pak \
  -archive \
  -archivedirectory="$BUILD_DIR"

echo "Server build complete! Output: $BUILD_DIR"
```

Make them executable:
```bash
chmod +x ~/apexsim/frontend/Scripts/*.sh
```

---

## Development Workflow

### Recommended Setup

1. **Development**: Use Windows Unreal Editor for visual work
   - Access project at `\\wsl$\Ubuntu\home\guido\apexsim\frontend\`
   - Full GUI, Blueprint editing, visual debugging

2. **Building**: Use WSL for compilation
   - Faster builds on Linux
   - Cross-compile for Windows
   - CI/CD integration

3. **Testing**:
   - Test Linux builds directly in WSL
   - Copy Windows builds to Windows via `\\wsl$\` path
   - Run automated tests in headless mode

### Typical Development Cycle

```bash
# 1. Edit code/blueprints in Windows UE Editor
# 2. Build from WSL
cd ~/apexsim/frontend/Scripts
./build-windows.sh

# 3. Test the build on Windows
# Windows build output is accessible at:
# \\wsl$\Ubuntu\home\guido\apexsim\builds\windows\

# 4. Build dedicated server
./build-server.sh
```

---

## Build Output Structure

After building, you'll find:

```
/home/guido/apexsim/builds/
├── windows/
│   └── WindowsNoEditor/
│       ├── ApexSim.exe           # Main executable
│       ├── ApexSim/              # Game content
│       └── Engine/               # Required engine files
├── linux/
│   └── LinuxNoEditor/
│       ├── ApexSim.sh
│       ├── ApexSim/
│       └── Engine/
└── server/
    └── LinuxServer/
        ├── ApexSimServer         # Dedicated server
        └── ApexSim/
```

---

## Important Notes

### Windows Cross-Compilation

- ✅ **Fully supported** by Epic Games
- ✅ **No Windows machine required** for building
- ✅ **Can build shipping executables** ready for distribution
- ⚠️ **Testing requires Windows** (or Wine for basic checks)
- ⚠️ **First build downloads SDK** (several GB)

### Build Performance

- **Linux builds**: Very fast (native compilation)
- **Windows cross-compilation**: Slightly slower (cross-toolchain overhead)
- **Tip**: Use `-iterative` flag for faster rebuilds during development

### Debugging Windows Builds

While you can build Windows executables on Linux, you'll need:
- Windows for actual testing and debugging
- Visual Studio on Windows for debugging C++ code
- Unreal Editor on Windows for Blueprint debugging

---

## Troubleshooting

### "Cross-compilation toolchain not found"

```bash
cd ~/unreal-engine/UnrealEngine
./Setup.sh
```

### "Failed to build UnrealHeaderTool"

Make sure you built UE5 itself first:
```bash
cd ~/unreal-engine/UnrealEngine
make
```

### Build is very slow

Use incremental builds:
```bash
./Engine/Build/BatchFiles/RunUAT.sh BuildCookRun \
  -iterativecooking \
  -iterativedeploy \
  # ... other flags
```

---

## Next Steps

1. Create your Unreal project (if not done yet)
2. Set up the build scripts
3. Try a test build for your target platform
4. Set up automated builds/CI if needed

# ApexSim Frontend Setup Guide

This guide walks you through setting up the complete development environment for the ApexSim Unreal Engine 5.3+ frontend on Linux.

## Table of Contents

1. [System Requirements](#system-requirements)
2. [Install Dependencies](#install-dependencies)
3. [Install Unreal Engine 5.3+](#install-unreal-engine-53)
4. [Create the ApexSim Project](#create-the-apexsim-project)
5. [Configure VSCode](#configure-vscode)
6. [Project Structure](#project-structure)
7. [Building the Project](#building-the-project)
8. [Troubleshooting](#troubleshooting)

---

## System Requirements

**Minimum Requirements:**
- **OS:** Ubuntu 22.04+ or Fedora 38+ (64-bit)
- **CPU:** 4-core @ 2.5GHz (Intel i5-8400 / AMD Ryzen 5 2600 equivalent)
- **GPU:** 4GB VRAM with Vulkan support (GTX 1060 / RX 580 equivalent)
- **RAM:** 8GB (16GB+ recommended for UE5 development)
- **Storage:** 100GB+ SSD space (UE5 source + project)
- **Network:** Broadband connection for downloading UE5

**Recommended for Development:**
- **RAM:** 32GB+
- **CPU:** 8+ cores
- **GPU:** 8GB+ VRAM with Vulkan 1.3
- **Storage:** 200GB+ NVMe SSD

---

## Install Dependencies

### Quick Setup (Automated)

Run the provided setup script to install all required dependencies:

```bash
cd /home/guido/apexsim/frontend
./setup_dev_environment.sh
```

This script will:
- Detect your Linux distribution
- Install build tools (clang, cmake, ninja)
- Install Mono runtime (required for UE5)
- Install development libraries (Vulkan, X11, OpenGL)
- Check your system resources
- Create initial project directories

### Manual Setup (If script fails)

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install -y build-essential clang cmake mono-complete \
    git ninja-build python3 python3-pip dos2unix \
    libx11-dev libxcursor-dev libxrandr-dev libxi-dev \
    libxinerama-dev mesa-common-dev libgl1-mesa-dev \
    libglu1-mesa-dev libvulkan-dev vulkan-tools libssl-dev
```

**Fedora/RHEL:**
```bash
sudo dnf groupinstall -y "Development Tools" "Development Libraries"
sudo dnf install -y clang cmake mono-complete git ninja-build \
    python3 dos2unix libX11-devel libXcursor-devel libXrandr-devel \
    libXi-devel mesa-libGL-devel vulkan-devel vulkan-tools openssl-devel
```

---

## Install Unreal Engine 5.3+

Unreal Engine 5 must be built from source on Linux. Follow these steps:

### 1. Link GitHub to Epic Games Account

1. Go to https://www.epicgames.com/account/connections
2. Link your GitHub account to your Epic Games account
3. Accept the invitation to the EpicGames organization on GitHub

### 2. Clone Unreal Engine Repository

```bash
cd ~
git clone https://github.com/EpicGames/UnrealEngine.git
cd UnrealEngine

# Check out the latest stable UE 5.3+ release
git checkout 5.3  # or 5.4, 5.5, etc.
```

**Note:** The repository is ~20GB, so this will take some time.

### 3. Download Dependencies

```bash
./Setup.sh
```

This downloads binary dependencies required for building UE5 (~10GB).

### 4. Generate Project Files

```bash
./GenerateProjectFiles.sh
```

This creates the build system files for Linux.

### 5. Build Unreal Engine

```bash
make
```

**Important:** This will take 1-4 hours depending on your system and will use significant disk space (~100GB after build).

For faster builds, use parallel compilation:
```bash
make -j$(nproc)  # Use all CPU cores
# Or specify a specific number:
make -j8         # Use 8 cores
```

### 6. Verify Installation

```bash
./Engine/Binaries/Linux/UnrealEditor --version
```

You should see the UE5 version information. If you see errors, check the [Troubleshooting](#troubleshooting) section.

---

## Create the ApexSim Project

### Method 1: Using Unreal Editor (GUI)

1. **Launch Unreal Editor:**
   ```bash
   ~/UnrealEngine/Engine/Binaries/Linux/UnrealEditor
   ```

2. **Create New Project:**
   - Select "Games" category
   - Choose "Blank" template
   - Project Settings:
     - Blueprint or C++: **C++**
     - Target Platform: **Desktop**
     - Quality Preset: **Maximum Quality**
     - Starter Content: **No Starter Content** (we'll add custom content)
   - Project Location: `/home/guido/apexsim/frontend`
   - Project Name: **ApexSim**
   - Click **Create**

### Method 2: Command Line Project Creation

```bash
cd /home/guido/apexsim/frontend

# Generate a C++ project from template
~/UnrealEngine/Engine/Binaries/Linux/UnrealEditor \
    -NewProject \
    -Template=/home/guido/UnrealEngine/Templates/TP_Blank/TP_Blank.uproject \
    -ProjectName=ApexSim \
    -TargetPlatform=Linux \
    -Language=C++
```

### Method 3: Create Project File Manually

Create `ApexSim.uproject` with this content:

```json
{
    "FileVersion": 3,
    "EngineAssociation": "5.3",
    "Category": "",
    "Description": "ApexSim SimRacing Frontend",
    "Modules": [
        {
            "Name": "ApexSim",
            "Type": "Runtime",
            "LoadingPhase": "Default",
            "AdditionalDependencies": [
                "Engine",
                "UMG",
                "Sockets",
                "Networking"
            ]
        }
    ],
    "Plugins": [
        {
            "Name": "EnhancedInput",
            "Enabled": true
        }
    ],
    "TargetPlatforms": [
        "Linux",
        "Windows"
    ]
}
```

Then create the basic module structure:

```bash
mkdir -p Source/ApexSim
mkdir -p Content
mkdir -p Config
```

---

## Configure VSCode

### 1. Generate VSCode Project Files

After creating your UE project, generate VSCode-compatible build files:

```bash
cd /home/guido/apexsim/frontend
~/UnrealEngine/Engine/Build/BatchFiles/Linux/GenerateProjectFiles.sh \
    -project=/home/guido/apexsim/frontend/ApexSim.uproject \
    -game -engine -vscode
```

This creates `compile_commands.json` for IntelliSense.

### 2. Install Recommended Extensions

VSCode will prompt you to install recommended extensions from `.vscode/extensions.json`:
- C/C++ Extension Pack
- CMake Tools
- Better C++ Syntax
- GitLens
- Todo Tree

Or install manually:
```bash
code --install-extension ms-vscode.cpptools
code --install-extension ms-vscode.cmake-tools
code --install-extension jeff-hykin.better-cpp-syntax
```

### 3. Update Include Paths (if needed)

If your UE installation is not in `~/UnrealEngine`, edit `.vscode/c_cpp_properties.json`:

```json
"includePath": [
    "${workspaceFolder}/**",
    "/YOUR/PATH/TO/UnrealEngine/Engine/Source/**",
    "/YOUR/PATH/TO/UnrealEngine/Engine/Plugins/**"
]
```

### 4. Open Project in VSCode

```bash
cd /home/guido/apexsim/frontend
code .
```

---

## Project Structure

After setup, your project structure should look like this:

```
frontend/
├── .vscode/                    # VSCode configuration
│   ├── c_cpp_properties.json   # C++ IntelliSense settings
│   ├── settings.json           # Editor settings
│   ├── extensions.json         # Recommended extensions
│   └── launch.json             # Debug configurations
├── Config/                     # Unreal config files (auto-generated)
│   ├── DefaultEngine.ini
│   ├── DefaultInput.ini
│   └── DefaultGame.ini
├── Content/                    # Unreal assets (Blueprints, Materials, etc.)
│   ├── Input/                  # Enhanced Input assets
│   ├── UI/                     # UMG widgets
│   ├── Cars/                   # Car meshes and materials
│   ├── Tracks/                 # Track meshes
│   └── Audio/                  # Sound assets
├── Docs/                       # Documentation
│   └── API/                    # Code documentation
├── Scripts/                    # Build and utility scripts
│   ├── build_project.sh
│   └── package_game.sh
├── Source/                     # C++ source code
│   └── ApexSim/                # Main game module
│       ├── ApexSim.Build.cs    # Build configuration
│       ├── ApexSim.h           # Module header
│       ├── ApexSim.cpp         # Module implementation
│       ├── SimNetClient.h      # Network client header
│       ├── SimNetClient.cpp    # Network client implementation
│       ├── PlayerCar.h         # Player car actor
│       ├── PlayerCar.cpp
│       ├── OtherCar.h          # Other players' cars
│       ├── OtherCar.cpp
│       └── ...                 # Other game classes
├── .gitignore                  # Git ignore rules
├── ApexSim.uproject            # Unreal project file
├── README.md                   # Project specification
└── SETUP.md                    # This file
```

---

## Building the Project

### Build from Command Line

```bash
cd /home/guido/apexsim/frontend

# Development build (for testing)
~/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
    ApexSimEditor Linux Development \
    -project=/home/guido/apexsim/frontend/ApexSim.uproject

# Shipping build (for distribution)
~/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
    ApexSim Linux Shipping \
    -project=/home/guido/apexsim/frontend/ApexSim.uproject
```

### Build from VSCode

Use the pre-configured build task:
1. Press `Ctrl+Shift+B` to open build tasks
2. Select "Build ApexSim"

### Build from Unreal Editor

1. Open project in editor
2. File → Package Project → Linux
3. Choose output directory

---

## Troubleshooting

### Issue: "clang: command not found"

**Solution:**
```bash
sudo apt install clang  # Ubuntu/Debian
sudo dnf install clang  # Fedora
```

### Issue: "Could not find Mono"

**Solution:**
```bash
sudo apt install mono-complete  # Ubuntu/Debian
sudo dnf install mono-complete  # Fedora
```

### Issue: Vulkan errors

**Solution:**
```bash
# Check Vulkan installation
vulkaninfo | head -20

# Install Vulkan (Ubuntu/Debian)
sudo apt install libvulkan-dev vulkan-tools

# Install Vulkan (Fedora)
sudo dnf install vulkan-devel vulkan-tools

# For NVIDIA GPUs, ensure drivers are installed
nvidia-smi
```

### Issue: Out of memory during UE5 build

**Solution:**
- Reduce parallel compilation: `make -j4` (use 4 cores instead of all)
- Add swap space:
  ```bash
  sudo fallocate -l 16G /swapfile
  sudo chmod 600 /swapfile
  sudo mkswap /swapfile
  sudo swapon /swapfile
  ```

### Issue: "Engine modules are out of date"

**Solution:**
```bash
cd /home/guido/apexsim/frontend
~/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
    ApexSimEditor Linux Development \
    -project=/home/guido/apexsim/frontend/ApexSim.uproject \
    -waitmutex
```

### Issue: VSCode IntelliSense not working

**Solution:**
1. Regenerate project files:
   ```bash
   ~/UnrealEngine/Engine/Build/BatchFiles/Linux/GenerateProjectFiles.sh \
       -project=/home/guido/apexsim/frontend/ApexSim.uproject \
       -game -engine -vscode
   ```
2. Reload VSCode window: `Ctrl+Shift+P` → "Reload Window"

### Issue: "Git credentials required" when cloning UE5

**Solution:**
- Ensure your GitHub account is linked to Epic Games
- Use SSH instead of HTTPS:
  ```bash
  git clone git@github.com:EpicGames/UnrealEngine.git
  ```
- Generate a GitHub personal access token and use it as password

---

## Next Steps

Once your development environment is set up:

1. **Review the Architecture:** Read the main [README.md](README.md) to understand the application architecture
2. **Create Core Classes:** Start implementing the core C++ classes:
   - `USimNetClient` - Network communication module
   - `APlayerCar` - Player car actor
   - `AOtherCar` - Other players' cars
   - `ASimGameMode` - Game mode
3. **Set Up Enhanced Input:** Create Input Actions and Mapping Contexts in `Content/Input/`
4. **Create UI Widgets:** Build UMG widgets for MainMenu, CreateJoinSession, etc.
5. **Test Network Connection:** Connect to the Rust backend server

---

## Additional Resources

- **Unreal Engine Documentation:** https://docs.unrealengine.com/5.3/
- **Unreal Engine C++ API:** https://docs.unrealengine.com/5.3/API/
- **Enhanced Input System:** https://docs.unrealengine.com/5.3/enhanced-input-in-unreal-engine/
- **UMG UI Designer:** https://docs.unrealengine.com/5.3/umg-ui-designer-for-unreal-engine/
- **Network Programming:** https://docs.unrealengine.com/5.3/networking-and-multiplayer-in-unreal-engine/

---

**Questions or Issues?**

If you encounter problems not covered in this guide, consult the Unreal Engine Linux documentation or community forums.

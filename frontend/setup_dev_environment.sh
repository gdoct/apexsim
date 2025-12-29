#!/bin/bash
# ApexSim Frontend Development Environment Setup Script
# For Ubuntu 22.04+ / Fedora 38+ Linux distributions
# Unreal Engine 5.3+ development environment

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== ApexSim Frontend Development Environment Setup ===${NC}\n"

# Detect distribution
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
    VERSION=$VERSION_ID
else
    echo -e "${RED}Cannot detect Linux distribution${NC}"
    exit 1
fi

echo -e "Detected distribution: ${GREEN}$DISTRO $VERSION${NC}\n"

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check for sudo privileges
if ! command_exists sudo; then
    echo -e "${RED}sudo is not installed. Please install sudo first.${NC}"
    exit 1
fi

echo -e "${YELLOW}Installing required dependencies for Unreal Engine 5 development...${NC}\n"

if [[ "$DISTRO" == "ubuntu" ]] || [[ "$DISTRO" == "debian" ]]; then
    # Ubuntu/Debian dependencies
    echo -e "${GREEN}Installing Ubuntu/Debian packages...${NC}"

    sudo apt update

    # Build essentials
    sudo apt install -y build-essential \
        clang \
        cmake \
        mono-complete \
        mono-devel \
        mono-xbuild \
        mono-dmcs \
        libmono-system-data-datasetextensions4.0-cil \
        libmono-system-web-extensions4.0-cil \
        libmono-system-management4.0-cil \
        libmono-system-xml-linq4.0-cil \
        cmake \
        dos2unix \
        git \
        ninja-build \
        python3 \
        python3-pip

    # Development libraries
    sudo apt install -y \
        libx11-dev \
        libxcursor-dev \
        libxrandr-dev \
        libxi-dev \
        libxinerama-dev \
        libxxf86vm-dev \
        mesa-common-dev \
        libgl1-mesa-dev \
        libglu1-mesa-dev \
        libvulkan-dev \
        vulkan-tools \
        libssl-dev \
        libogg-dev \
        libvorbis-dev

    echo -e "${GREEN}Ubuntu/Debian packages installed successfully!${NC}\n"

elif [[ "$DISTRO" == "fedora" ]] || [[ "$DISTRO" == "rhel" ]]; then
    # Fedora/RHEL dependencies
    echo -e "${GREEN}Installing Fedora/RHEL packages...${NC}"

    sudo dnf groupinstall -y "Development Tools" "Development Libraries"

    sudo dnf install -y \
        clang \
        cmake \
        mono-complete \
        mono-devel \
        git \
        ninja-build \
        python3 \
        python3-pip \
        dos2unix \
        libX11-devel \
        libXcursor-devel \
        libXrandr-devel \
        libXi-devel \
        libXinerama-devel \
        mesa-libGL-devel \
        mesa-libGLU-devel \
        vulkan-devel \
        vulkan-tools \
        openssl-devel \
        libogg-devel \
        libvorbis-devel

    echo -e "${GREEN}Fedora/RHEL packages installed successfully!${NC}\n"

else
    echo -e "${YELLOW}Unsupported distribution. Please install dependencies manually.${NC}"
    echo -e "Required: clang, cmake, mono, git, vulkan, mesa, X11 development libraries"
fi

# Check Git configuration
echo -e "${YELLOW}Checking Git configuration...${NC}"
if ! git config --global user.name > /dev/null 2>&1; then
    echo -e "${YELLOW}Git user.name not set. Please configure:${NC}"
    echo "  git config --global user.name \"Your Name\""
fi
if ! git config --global user.email > /dev/null 2>&1; then
    echo -e "${YELLOW}Git user.email not set. Please configure:${NC}"
    echo "  git config --global user.email \"your.email@example.com\""
fi

# Check for Unreal Engine
echo -e "\n${YELLOW}Checking for Unreal Engine installation...${NC}"
UE_POTENTIAL_PATHS=(
    "$HOME/UnrealEngine"
    "/opt/UnrealEngine"
    "$HOME/Epic/UnrealEngine"
)

UE_FOUND=false
for path in "${UE_POTENTIAL_PATHS[@]}"; do
    if [ -d "$path" ]; then
        echo -e "${GREEN}Found Unreal Engine at: $path${NC}"
        UE_FOUND=true
        UE_PATH=$path
        break
    fi
done

if [ "$UE_FOUND" = false ]; then
    echo -e "${YELLOW}Unreal Engine not found in common locations.${NC}"
    echo -e "Please follow the Unreal Engine installation instructions in SETUP.md"
    echo -e "You will need to:"
    echo -e "  1. Link your Epic Games account to GitHub"
    echo -e "  2. Clone the UE5 repository from GitHub"
    echo -e "  3. Build Unreal Engine from source"
else
    # Check for UE5 binary
    if [ -f "$UE_PATH/Engine/Binaries/Linux/UnrealEditor" ]; then
        echo -e "${GREEN}Unreal Editor found!${NC}"
        UE_VERSION=$("$UE_PATH/Engine/Binaries/Linux/UnrealEditor" -version 2>/dev/null || echo "Unknown")
        echo -e "Version info: $UE_VERSION"
    else
        echo -e "${YELLOW}Unreal Editor binary not found. You may need to build UE from source.${NC}"
    fi
fi

# Check system resources
echo -e "\n${YELLOW}Checking system resources...${NC}"
TOTAL_RAM=$(free -g | awk '/^Mem:/{print $2}')
CPU_CORES=$(nproc)
echo -e "CPU Cores: ${GREEN}$CPU_CORES${NC}"
echo -e "Total RAM: ${GREEN}${TOTAL_RAM}GB${NC}"

if [ "$TOTAL_RAM" -lt 8 ]; then
    echo -e "${RED}Warning: Less than 8GB RAM detected. Unreal Engine may run slowly.${NC}"
fi

if [ "$CPU_CORES" -lt 4 ]; then
    echo -e "${RED}Warning: Less than 4 CPU cores detected. Compilation will be slow.${NC}"
fi

# Check GPU
echo -e "\n${YELLOW}Checking GPU...${NC}"
if command_exists nvidia-smi; then
    GPU_INFO=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null || echo "NVIDIA GPU detected but info unavailable")
    echo -e "GPU: ${GREEN}$GPU_INFO${NC}"
    echo -e "NVIDIA drivers installed: ${GREEN}Yes${NC}"
elif command_exists vulkaninfo; then
    GPU_INFO=$(vulkaninfo --summary 2>/dev/null | grep -i "deviceName" | head -1 || echo "Unknown")
    echo -e "GPU (Vulkan): ${GREEN}$GPU_INFO${NC}"
else
    echo -e "${YELLOW}GPU detection failed. Ensure Vulkan drivers are installed.${NC}"
fi

# Create initial directories
echo -e "\n${YELLOW}Creating project directory structure...${NC}"
mkdir -p Config
mkdir -p Docs
mkdir -p Scripts
echo -e "${GREEN}Directory structure created!${NC}"

echo -e "\n${GREEN}=== Development Environment Setup Complete! ===${NC}"
echo -e "\n${YELLOW}Next Steps:${NC}"
echo -e "1. Read ${GREEN}SETUP.md${NC} for Unreal Engine installation instructions"
echo -e "2. Install UE5.3+ if not already installed"
echo -e "3. Create the ApexSim Unreal project using the instructions in SETUP.md"
echo -e "4. Open the project in Unreal Editor or generate VSCode project files"
echo -e "\n${YELLOW}Useful commands:${NC}"
echo -e "  Check Vulkan: ${GREEN}vulkaninfo${NC}"
echo -e "  Check GPU: ${GREEN}nvidia-smi${NC} or ${GREEN}glxinfo | grep OpenGL${NC}"
echo -e "  Build UE project: ${GREEN}./Scripts/build_project.sh${NC} (after project creation)"

#!/bin/bash
# Launch Unreal Engine with NVIDIA RTX 3080 GPU via D3D12

# Set the D3D12 driver to use your NVIDIA GPU
export GALLIUM_DRIVER=d3d12

# Add WSL GPU libraries to library path
export LD_LIBRARY_PATH=/usr/lib/wsl/lib:$LD_LIBRARY_PATH

# Navigate to Unreal Engine directory
cd ~/unreal-engine/UnrealEngine

# Launch the editor with OpenGL 4
# The -opengl4 flag should force OpenGL over Vulkan
./Engine/Binaries/Linux/UnrealEditor -opengl4 "$@"

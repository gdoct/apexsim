#!/bin/bash
cd "$(dirname "$0")"

# Force Vulkan backend for WSL2 compatibility
export WGPU_BACKEND=vulkan

cargo run --release

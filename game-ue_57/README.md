# ApexSim Frontend - Unreal Engine Client Application

## Overview

ApexSim is a multiplayer sim-racing application featuring a **responsive Unreal Engine client** that connects to a Rust backend server. This repository contains the frontend client implementation, designed for cross-platform compatibility and future extensibility.

**Engine:** Unreal Engine 5.3+ (latest stable version recommended)

**Target Platforms:**
*   **Primary:** Windows 10/11 (64-bit) - DirectX 12, Vulkan
*   **Secondary:** Linux (Ubuntu 22.04+, Fedora 38+) - Vulkan
*   **Future:** macOS (Metal), Steam Deck (Proton/Vulkan)

**Minimum Requirements:**
*   CPU: 4-core @ 2.5GHz (Intel i5-8400 / AMD Ryzen 5 2600 equivalent)
*   GPU: 4GB VRAM, DX12/Vulkan support (GTX 1060 / RX 580 equivalent)
*   RAM: 8GB
*   Storage: 10GB SSD space
*   Network: Broadband connection (5 Mbps down, 1 Mbps up, <100ms ping)

---

## Documentation Structure

This project's specifications are organized into focused documents for easier navigation and maintenance.

### Core Architecture

*   **[Architecture Overview](Docs/ARCHITECTURE.md)** - High-level system architecture, data structures, and network communication

### Scene Specifications

Each game scene has its own detailed specification:

*   **[Main Menu](Docs/Scenes/MainMenu.md)** - Entry point and primary navigation hub
*   **[Create/Join Session](Docs/Scenes/CreateJoinSession.md)** - Lobby interface and session management
*   **[Settings](Docs/Scenes/Settings.md)** - Comprehensive settings menu (graphics, audio, controls, network, gameplay)
*   **[Content Management](Docs/Scenes/ContentManagement.md)** - Car and track selection browser
*   **[Driving View](Docs/Scenes/DrivingView.md)** - Main 3D racing scene and gameplay

### System Specifications

Cross-cutting systems used throughout the application:

*   **[Input System](Docs/Systems/InputSystem.md)** - Enhanced Input System configuration, device support, and force feedback
*   **[Camera System](Docs/Systems/CameraSystem.md)** - Multiple camera views and controls
*   **[HUD System](Docs/Systems/HUD.md)** - Heads-up display elements and customization
*   **[Audio System](Docs/Systems/AudioSystem.md)** - Sound effects, spatial audio, and music

---

## Quick Start

### For Developers

1. **Review Architecture**: Start with [Architecture Overview](Docs/ARCHITECTURE.md) to understand the core systems
2. **Explore Scenes**: Review individual scene specifications based on what you're implementing
3. **Reference Systems**: Consult system specifications for detailed implementation guidance

### For Designers

1. **Scene Flow**: Start with [Main Menu](Docs/Scenes/MainMenu.md) to understand navigation flow
2. **User Settings**: Review [Settings](Docs/Scenes/Settings.md) for all configurable options
3. **UI Elements**: Check [HUD System](Docs/Systems/HUD.md) for display requirements

### For Artists

1. **Visual Requirements**: See individual scene specifications for asset needs
2. **Audio Needs**: Review [Audio System](Docs/Systems/AudioSystem.md) for sound requirements
3. **Camera Perspectives**: Check [Camera System](Docs/Systems/CameraSystem.md) for view angles

---

## Key Features

### Networking
*   UDP/TCP communication with Rust backend server
*   Client-side prediction and interpolation for smooth gameplay
*   240Hz input transmission for responsive controls
*   Real-time telemetry updates

### Input Support
*   Keyboard and mouse
*   Xbox/PlayStation controllers
*   DirectInput/XInput steering wheels and pedals
*   Force feedback support
*   Fully rebindable controls

### Graphics
*   Multiple quality presets (Low to Ultra)
*   Resolution and display mode options
*   Advanced graphics settings (shadows, textures, AA, post-processing)
*   60+ FPS target performance

### Audio
*   3D positional audio
*   Multi-layered engine sounds with real-time pitch shifting
*   Independent volume controls for engine, tires, UI, and ambient
*   Doppler effect for passing cars

### Customization
*   Comprehensive settings menu
*   HUD opacity and scale adjustment
*   Multiple camera views
*   Speedometer unit selection (km/h, mph, m/s)

---

## Development Status

**Current Phase:** Initial implementation

**Implemented:**
*   Core architecture design
*   Detailed specifications for all scenes and systems

**In Progress:**
*   UE5 project setup
*   Network module implementation
*   Scene prototyping

**Planned:**
*   Complete scene implementation
*   Asset creation and integration
*   Testing and optimization
*   Modding support

---

## Contributing

When contributing to this project, please:

1. **Review Specifications**: Consult the relevant documentation before implementing features
2. **Follow Conventions**: Adhere to the architectural patterns defined in [ARCHITECTURE.md](Docs/ARCHITECTURE.md)
3. **Update Documentation**: Keep specifications in sync with implementation changes
4. **Test Thoroughly**: Ensure features work across all supported platforms

---

## Project Structure

```
/home/guido/apexsim/frontend/
├── README.md                    # This file
├── Docs/
│   ├── ARCHITECTURE.md          # Core architecture specification
│   ├── Scenes/                  # Scene-specific documentation
│   │   ├── MainMenu.md
│   │   ├── CreateJoinSession.md
│   │   ├── Settings.md
│   │   ├── ContentManagement.md
│   │   └── DrivingView.md
│   └── Systems/                 # System-specific documentation
│       ├── InputSystem.md
│       ├── CameraSystem.md
│       ├── HUD.md
│       └── AudioSystem.md
├── Config/                      # Unreal Engine config files
├── Content/                     # Game assets and blueprints
├── Scripts/                     # Build and utility scripts
└── (UE5 project files)

```

---

## License

*(License information to be added)*

---

## Contact

*(Contact information to be added)*

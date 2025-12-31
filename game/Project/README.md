# ApexSim Unreal Engine Project

## Project Structure

This is the Unreal Engine 5.7 project for ApexSim, a multiplayer sim-racing application.

### Directory Structure

- **Source/** - C++ source code
  - **ApexSim/** - Main game module
- **Content/** - Game assets and blueprints
  - **Maps/** - Level maps (MainMenu, CreateJoinSession, DrivingView)
  - **UI/** - User interface widgets
  - **Blueprints/** - Blueprint classes
  - **Data/** - Data assets and tables
  - **Audio/** - Sound files
  - **Materials/** - Material assets
- **Config/** - Configuration files

## Getting Started

### Prerequisites

- Unreal Engine 5.7
- Visual Studio 2022 with C++ development tools
- Windows 10/11 (64-bit)

### Building the Project

1. Right-click on `ApexSim.uproject` and select **Generate Visual Studio project files**
2. Open `ApexSim.sln` in Visual Studio
3. Build the solution (F7)
4. Launch the editor by opening `ApexSim.uproject`

### Quick Start

After opening the project in Unreal Editor:

1. The default starting map is **MainMenu**
2. Create your first level by going to File → New Level
3. Refer to the documentation in `/game/Docs` for scene specifications

## Network Backend

ApexSim connects to a Rust backend server located in `/server`. Make sure the backend is running before testing multiplayer features.

## Documentation

For detailed specifications, see:
- [Architecture Overview](../Docs/ARCHITECTURE.md)
- [Scene Specifications](../Docs/Scenes/)
- [System Specifications](../Docs/Systems/)

## Copyright

Copyright ApexSim Team. All Rights Reserved.

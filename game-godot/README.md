# ApexSim - Godot implementation

This is a Godot 4.3 project that implements a client for the ApexSim multiplayer simulation framework. It allows users to connect to an ApexSim server and participate in real-time simulations.

## Current Status

The basic game skeleton is implemented with:
- Loading screen with progress bar
- Main menu with logo and background
- 4 menu options (Refresh Lobby State, Select Car, Create New Session, Join Session)
- Quit functionality

## Project Structure

```
game-godot/
├── assets/               # Game assets
│   ├── logo.png         # ApexSim logo
│   ├── menu_background.png
│   └── loadingscreen.png
├── scenes/              # Godot scenes
│   ├── loading_screen.tscn
│   └── main_menu.tscn
├── scripts/             # GDScript files
│   ├── scene_manager.gd   # Global autoload for scene transitions
│   ├── loading_screen.gd  # Loading screen logic
│   └── main_menu.gd       # Main menu button handlers
└── project.godot        # Godot project configuration
```

## Running the Game

1. Open the project in Godot 4.3 or later
2. Press F5 or click the Play button
3. You will see:
   - Loading screen with progress bar (2 seconds minimum)
   - Main menu with 4 options matching the CLI client functionality

## Next Steps

The menu buttons currently print debug messages. Future implementation will include:
- Network client integration (TCP connection to ApexSim server)
- Authentication flow
- Lobby state display (players, sessions, cars, tracks)
- Car selection dialog
- Session creation dialog (track selection, player count, AI drivers, lap count)
- Session joining functionality
- Real-time telemetry visualization during races

## Requirements

- Godot 4.3 or later
- OpenGL 3.3 compatible graphics card
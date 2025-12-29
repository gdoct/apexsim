# ApexSim Content Directory

This directory contains all Unreal Engine assets for the ApexSim project.

## Structure

```
Content/
├── Input/          # Enhanced Input System assets
│   ├── IA_*.uasset       # Input Actions
│   └── IMC_*.uasset      # Input Mapping Contexts
├── UI/             # UMG User Interface widgets
│   ├── MainMenu/         # Main menu widgets
│   ├── Session/          # Session creation/join widgets
│   ├── Settings/         # Settings menu widgets
│   ├── HUD/              # In-game HUD widgets
│   └── Common/           # Reusable UI components
├── Cars/           # Car 3D models and materials
│   ├── Meshes/           # Static meshes for cars
│   ├── Materials/        # Car materials
│   ├── Textures/         # Car textures
│   └── Blueprints/       # Car Blueprint actors (if needed)
├── Tracks/         # Track 3D models and materials
│   ├── Meshes/           # Static meshes for tracks
│   ├── Materials/        # Track materials
│   ├── Textures/         # Track textures
│   └── Blueprints/       # Track Blueprint actors
└── Audio/          # Sound effects and audio
    ├── Engine/           # Engine sounds
    ├── Tires/            # Tire sounds
    ├── UI/               # UI sound effects
    └── Ambient/          # Environmental sounds
```

## Asset Naming Conventions

Follow Unreal Engine best practices:

- **Static Meshes:** `SM_CarBody`, `SM_TrackSurface`
- **Materials:** `M_CarPaint`, `M_TrackAsphalt`
- **Material Instances:** `MI_CarPaint_Red`, `MI_TrackAsphalt_Wet`
- **Textures:** `T_CarPaint_BaseColor`, `T_CarPaint_Normal`
- **Blueprints:** `BP_PlayerCar`, `BP_TrackActor`
- **Widgets:** `WBP_MainMenu`, `WBP_HUD`
- **Input Actions:** `IA_Throttle`, `IA_Steering`
- **Input Mapping Contexts:** `IMC_Keyboard`, `IMC_Gamepad`
- **Sounds:** `SFX_EngineLow`, `SFX_TireScreech`

## Creating Assets

All assets should be created through the Unreal Editor:

1. Launch Editor: `~/UnrealEngine/Engine/Binaries/Linux/UnrealEditor ApexSim.uproject`
2. Right-click in Content Browser → Create new asset
3. Follow naming conventions
4. Save and organize in appropriate folders

## Importing External Assets

For 3D models created in external tools (Blender, Maya, etc.):

1. Export as FBX format
2. In Unreal Editor: Import → Select FBX file
3. Configure import settings (collision, materials, etc.)
4. Place in appropriate Content subfolder

## Version Control

- Binary assets (`.uasset`, `.umap`) are tracked in Git
- Large assets may need Git LFS (Large File Storage)
- Always save assets before committing
- Use descriptive commit messages for asset changes

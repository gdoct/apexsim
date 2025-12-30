# Unreal Engine Headless Workflow on WSL

## What You Can Do Without the GUI

### 1. Create a New Project
```bash
cd ~/unreal-engine/UnrealEngine
./Engine/Binaries/Linux/UnrealEditor -nullrhi -run=Project -create \
  -project="/path/to/MyProject.uproject" \
  -templatepath="/path/to/template"
```

### 2. Build Your C++ Code
```bash
cd ~/unreal-engine/UnrealEngine
./Engine/Build/BatchFiles/Linux/Build.sh MyProjectEditor Linux Development \
  -project="/home/guido/apexsim/frontend/MyProject.uproject"
```

### 3. Cook Content for Packaging
```bash
./Engine/Binaries/Linux/UnrealEditor-Cmd \
  "/home/guido/apexsim/frontend/MyProject.uproject" \
  -run=cook -targetplatform=Linux -iterate -unattended
```

### 4. Run Automation Tests
```bash
./Engine/Binaries/Linux/UnrealEditor \
  "/home/guido/apexsim/frontend/MyProject.uproject" \
  -nullrhi -unattended -nopause -nosplash \
  -ExecCmds="Automation RunTests System;Quit"
```

### 5. Generate Project Files
```bash
./GenerateProjectFiles.sh
```

## Hybrid Workflow: WSL + Windows

### Setup
1. Install Unreal Engine on Windows (from Epic Games Launcher)
2. Access your WSL project from Windows at: `\\wsl$\Ubuntu\home\guido\apexsim\`
3. You can open the `.uproject` file directly from Windows

### Benefits
- **Windows**: Visual editing, Blueprint work, Material editor, Level design
- **WSL**: Fast compilation, automation, CI/CD, version control
- **Shared**: Same project files, just accessed from different OS

### File Syncing
Files are automatically synced because Windows accesses WSL's filesystem directly.
Just make sure to close the editor on one platform before opening on the other.

## What Works Best Where

| Task | WSL (Headless) | Windows (GUI) |
|------|----------------|---------------|
| Blueprint editing | ❌ | ✅ |
| C++ compilation | ✅ | ✅ |
| Level design | ❌ | ✅ |
| Material editing | ❌ | ✅ |
| Cooking/Packaging | ✅ | ✅ |
| Running tests | ✅ | ✅ |
| Version control | ✅ | ✅ |
| Debugging | Limited | ✅ |

## Recommended Setup for Your Case

Since you have networking code and likely need to test multiplayer:

1. **Use Windows Unreal Editor** for all visual work
2. **Use WSL for**:
   - Building the dedicated server
   - Running automated tests
   - CI/CD pipelines
   - Git operations

Your project at `/home/guido/apexsim/frontend/` can be accessed from Windows at:
`\\wsl$\Ubuntu\home\guido\apexsim\frontend\`

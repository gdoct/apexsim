# ApexSim - Godot C# Client

A high-performance Godot 4.5 C# client for the ApexSim multiplayer racing simulation with **full bincode protocol support**.

## ✅ Current Status - FULLY FUNCTIONAL

### Implemented Features
- ✅ **Custom Bincode Serializer** - Matches Rust bincode format exactly
- ✅ **Network Client** - TCP with length-prefixed messages
- ✅ **Connection Dialog** - Server address, port, player name, auth token
- ✅ **Session Browser** - Browse and join available sessions
- ✅ **Session Creation** - Create sessions with track, players, AI, laps
- ✅ **Main Menu** - Dynamic UI that adapts to connection/session state
- ✅ **Thread-Safe Networking** - Background receive, main thread processing
- ✅ **Auto-Authentication** - Connects and authenticates automatically

### Complete Lobby UX (Like CLI)
1. **Connect** - Configure and connect to server
2. **Browse Sessions** - See all available sessions with real-time info
3. **Join Session** - Join existing sessions
4. **Create Session** - Host new sessions with custom settings
5. **Leave/Start** - Leave sessions or start them (if host)

## 🎮 Quick Start

### 1. Open in Godot C# Editor
```bash
/home/guido/godot/Godot_v4.5.1-stable_mono_linux_x86_64/Godot_v4.5.1-stable_mono_linux.x86_64 project.godot
```

### 2. Build C# Project
- Click "Build" button in Godot (top right)
- Wait for compilation

### 3. Run Server
```bash
cd ../server
cargo run
```

### 4. Run Game
- Press F5 in Godot
- Click "Connect to Server"
- Use defaults (127.0.0.1:9000, Player, dev-token)
- Browse/Create/Join sessions!

## 📁 Project Structure

```
game-godot/
├── ApexSim.csproj, ApexSim.sln    # C# project files
├── assets/
│   ├── logo.png, menu_background.jpg, loadingscreen.png
├── scenes/
│   ├── loading_screen.tscn         # Loading screen
│   ├── main_menu.tscn              # Main menu (C#)
│   ├── connection_dialog.tscn      # Server connection
│   ├── session_browser.tscn        # Browse sessions
│   └── session_creation.tscn       # Create session
└── scripts/
    ├── csharp/
    │   ├── BincodeSerializer.cs    # Bincode read/write
    │   ├── Protocol.cs             # Message types
    │   ├── NetworkClient.cs        # TCP client (singleton)
    │   ├── MainMenu.cs             # Main menu
    │   ├── ConnectionDialog.cs
    │   ├── SessionBrowserDialog.cs
    │   └── SessionCreationDialog.cs
    └── (GDScript loading/scene management)
```

## 🔧 Technical Details

### Bincode Format
- Little-endian integers
- UTF-8 strings with u64 length prefix
- Enum variants as u32 indices
- Option<T> as byte (0=None, 1=Some) + value
- Vec<T> as u64 length + elements

### Network Protocol
Messages: `[4-byte big-endian length][bincode data]`

**Compatible with Rust server!** No server modifications needed.

## 🎨 UI Flow

1. **Loading Screen** → Shows for 2s
2. **Main Menu** → "Connect to Server" button
3. **Connection Dialog** → Enter server details
4. **Authenticated** → Shows "Create/Join Session" buttons
5. **Session Browser** → List of sessions, click to join
6. **Session Creation** → Configure track, players, AI, laps
7. **In Session** → Shows "Leave/Start Session" buttons

## Requirements

- **Godot 4.5+ Mono** (C# support)
- **.NET 8.0 SDK**
- **ApexSim Server** (localhost:9000 or custom)

## 📝 Notes

- **WSL**: Use Windows Godot editor for proper input
- **Bincode**: Fully compatible with Rust server
- **Thread-Safe**: Network on background thread, UI on main thread
- **Auto-Refresh**: Lobby updates automatically

**Ready to race!** 🏎️
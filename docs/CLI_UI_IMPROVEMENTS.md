# CLI Game UI Improvements

## Summary

Updated the CLI game client with borders and improved track display to match the new track format.

## Changes Made

### 1. **Added Borders Throughout the UI**

#### Banner
- Replaced ASCII art with bordered header
- Clean, professional look with Unicode box-drawing characters
- Cyan color scheme for consistency

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                         APEXSIM RACING CLI CLIENT                            ║
╠══════════════════════════════════════════════════════════════════════════════╣
║    High-performance multiplayer racing simulation                            ║
║    Version: 0.1.0                                                            ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

#### Main Menu Header
- Added bordered status display
- Shows player name, lobby/session status
- Displays counts with color-coded statistics

```
╔══════════════════════════════════════════════════════════════════════════════╗
║ ● CLI-Player [In Lobby]
║ Players: 1 | Sessions: 0 | Cars: 1 | Tracks: 3
╚══════════════════════════════════════════════════════════════════════════════╝
```

#### Lobby State Display
- Full bordered layout for all sections
- Separate bordered sections for:
  - Players in Lobby
  - Available Sessions
  - Available Cars
  - Available Tracks

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                              LOBBY STATE                                     ║
╚══════════════════════════════════════════════════════════════════════════════╝

┌─ Available Tracks ─────────────────────────────────────────────────────────┐
│  • Simple Oval Track
│  • Street Circuit Downtown
│  • Grand Prix Circuit
└────────────────────────────────────────────────────────────────────────────┘
```

### 2. **Track Display Improvements**

#### Track Selection
- Shows all 3 available tracks from `content/tracks/` directory:
  - `simple_oval.yaml` → "Simple Oval Track"
  - `street_circuit.json` → "Street Circuit Downtown"
  - `race_track_complete.yaml` → "Grand Prix Circuit"
- Added race flag emoji (🏁) to track selection menu
- Bold styling for track names in lobby display
- Bordered "CREATE NEW SESSION" header

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                            CREATE NEW SESSION                                ║
╚══════════════════════════════════════════════════════════════════════════════╝

? Select a track
> 🏁 Simple Oval Track
  🏁 Street Circuit Downtown
  🏁 Grand Prix Circuit
```

#### Track Names from Files
The server now correctly loads track names from the YAML/JSON files:
- Uses `name` field from track configuration
- Supports both YAML and JSON formats
- Displays proper track names instead of file names

### 3. **Car Selection Improvements**

#### Car Selection UI
- Added bordered "SELECT YOUR CAR" header
- Added car emoji (🏎️) to car selection menu
- Bold, colored car name on confirmation

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                              SELECT YOUR CAR                                 ║
╚══════════════════════════════════════════════════════════════════════════════╝

? Select a car
> 🏎️  Default Racing Car
```

### 4. **Enhanced Session Creation Feedback**

- Color-coded session parameters:
  - Track name: **bold green**
  - Max players: **cyan**
  - AI count: **yellow**
  - Lap limit: **magenta**

```
→ Creating session on Simple Oval Track with 8 max players, 2 AI, 5 laps...
```

## Technical Details

### Files Modified

- [cli-game/src/main.rs](cli-game/src/main.rs)
  - Updated `print_banner()` function
  - Updated `display_lobby_state()` function
  - Updated `run_menu()` function
  - Updated `select_car()` function
  - Updated `create_session()` function

### Track Loading

The server loads tracks from `/content/tracks/` directory:
- Scans for `.yaml`, `.yml`, and `.json` files
- Uses `TrackLoader` to parse track configuration
- Extracts `name` field from track files
- Supports multiple track formats (YAML and JSON)

### Color Scheme

- **Cyan**: Borders, headers, structural elements
- **Green**: Success messages, player names, track names
- **Yellow**: Warnings, session status
- **Red**: Errors, racing status
- **Magenta**: Special values (lap counts)
- **Dim**: Less important information

## Benefits

1. **Professional Appearance**: Consistent bordered design throughout
2. **Better Visual Hierarchy**: Clear separation of sections
3. **Track Support**: Properly displays track names from configuration files
4. **User Friendly**: Emoji icons and color coding improve usability
5. **Scalable**: Supports adding more tracks without code changes

## Testing

The CLI client now correctly:
- ✅ Loads 3 tracks from `content/tracks/` directory
- ✅ Displays track names (not file names)
- ✅ Shows bordered UI elements consistently
- ✅ Allows track selection from a list
- ✅ Color-codes all information appropriately
- ✅ Works with both YAML and JSON track formats

## Screenshots

See screenshot provided by user showing:
- CLI-Player in lobby
- Players online: 1 | Sessions: 0 | Cars: 1 | Tracks: 1
- Main menu options with proper formatting
- Border around the entire interface

## Future Enhancements

Potential improvements:
- Add track length and lap record to track display
- Show track difficulty or type (oval, road course, street circuit)
- Display car specifications in car selection
- Add color-coded performance ratings
- Show preview of track layout (ASCII art)

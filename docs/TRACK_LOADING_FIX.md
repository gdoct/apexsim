# Track Loading Fix

## Issue

The CLI game was displaying 4 tracks instead of the 3 tracks in the `content/tracks/` directory. One of the tracks ("Default Oval") was deleted but still appeared in the list.

## Root Cause

The server was creating a **hardcoded default track** in addition to loading tracks from the files.

In [server/src/main.rs:45-50](server/src/main.rs#L45-L50), the code was:

```rust
// Create default car and track
let default_car = CarConfig::default();
let default_track = TrackConfig::default();  // <-- This was the problem

car_configs.insert(default_car.id, default_car);
track_configs.insert(default_track.id, default_track);  // Adds "Default Oval"

// Load custom tracks from content/tracks directory
Self::load_custom_tracks(&mut track_configs);
```

The `TrackConfig::default()` in [server/src/data.rs:244-319](server/src/data.rs#L244-L319) creates a hardcoded track named **"Default Oval"**, which was being added to the track list before loading the custom tracks.

## Fix

Removed the default track creation, keeping only the default car:

```rust
// Create default car
let default_car = CarConfig::default();
car_configs.insert(default_car.id, default_car);

// Load custom tracks from content/tracks directory
Self::load_custom_tracks(&mut track_configs);
```

## Result

The server now loads **only** the 3 tracks from the `content/tracks/` directory:

1. ✅ **"Simple Oval Track"** (from `simple_oval.yaml`)
2. ✅ **"Monaco-Style Street Circuit"** (from `street_circuit.json`)
3. ✅ **"Complete Racing Circuit"** (from `race_track_complete.yaml`)

## Track Loading Details

### Track Files

```
/home/guido/apexsim/content/tracks/
├── simple_oval.yaml           → "Simple Oval Track"
├── street_circuit.json        → "Monaco-Style Street Circuit"
└── race_track_complete.yaml   → "Complete Racing Circuit"
```

### Track Loader

The server uses `TrackLoader::load_from_file()` ([server/src/track_loader.rs](server/src/track_loader.rs)) to:

1. Read `.yaml`, `.yml`, or `.json` files
2. Parse the track structure
3. Extract the `name` field
4. Build a `TrackConfig` with proper centerline, banking, elevation, etc.

### Supported Formats

Both YAML and JSON formats are supported:

**YAML format:**
```yaml
name: "Simple Oval Track"
default_width: 12.0
closed_loop: true
nodes:
  - x: 0.0
    y: 0.0
    z: 0.0
    banking: 0.0
```

**JSON format:**
```json
{
  "name": "Monaco-Style Street Circuit",
  "default_width": 10.0,
  "closed_loop": true,
  "nodes": [
    {"x": 0.0, "y": 0.0, "z": 0.0, "banking": 0.0}
  ]
}
```

## CLI Display

The CLI client now correctly displays all 3 tracks:

```
┌─ Available Tracks ─────────────────────────────────────────────────────────┐
│  • Simple Oval Track
│  • Monaco-Style Street Circuit
│  • Complete Racing Circuit
└────────────────────────────────────────────────────────────────────────────┘
```

When creating a session, users can select from:

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                            CREATE NEW SESSION                                ║
╚══════════════════════════════════════════════════════════════════════════════╝

? Select a track
  🏁 Simple Oval Track
  🏁 Monaco-Style Street Circuit
> 🏁 Complete Racing Circuit
```

## Testing

1. **Server build**: ✅ Successful
2. **Track count**: ✅ Shows exactly 3 tracks
3. **Track names**: ✅ Matches file content (not filenames)
4. **Track selection**: ✅ Can select any of the 3 tracks
5. **Session creation**: ✅ Works with all tracks

## Files Modified

- [server/src/main.rs](server/src/main.rs#L45-L50) - Removed default track creation

## Benefits

1. **Accurate track list**: Only shows tracks that actually exist
2. **Dynamic loading**: Add new tracks by just adding files to `content/tracks/`
3. **No ghost tracks**: Deleted tracks don't appear in the list
4. **Flexibility**: Supports both YAML and JSON formats

## Future Improvements

The `TrackConfig::default()` implementation still exists in [server/src/data.rs:244-319](server/src/data.rs#L244-L319) but is now unused. It could be:

1. Removed entirely if not needed for tests
2. Kept as a fallback if no tracks are found
3. Moved to a test-only module

For now, it's harmless since it's not being called.

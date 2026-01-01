# ApexSim Track Files

This directory contains track definitions for ApexSim racing simulator. Tracks are defined using a parametric centerline spline format with metadata.

## Available Tracks

### Real-World Tracks (Converted)

See [real/](real/) for **25+ professionally converted real-world race tracks** including:
- ✅ **Formula 1** circuits (Spa, Monza, Silverstone, Suzuka, and more)
- ✅ **DTM** circuits (Nürburgring, Brands Hatch, Zandvoort, etc.)
- ✅ **IndyCar** circuits (Indianapolis Motor Speedway)

All real-world tracks include accurate centerlines, track widths, and optimized racing lines.

### Example Tracks

- [simple_oval.yaml](simple_oval.yaml) - Basic oval track
- [street_circuit.json](street_circuit.json) - Street circuit example
- [race_track_complete.yaml](race_track_complete.yaml) - Full-featured example

## Quick Start

### Use Real-World Track

Edit `server/server.toml`:
```toml
[track]
track_file = "./content/tracks/real/Spa.yaml"
```

### Convert More Tracks

```bash
cd server
./convert_all_tracks.sh /path/to/racetrack-database ./content/tracks/real
```

See [../../docs/TRACK_CONVERTER.md](../../docs/TRACK_CONVERTER.md) for the track converter tool documentation.

## File Format

Tracks can be defined in either JSON or YAML format. The server automatically loads all `.json`, `.yaml`, and `.yml` files from this directory on startup.

### Complete Track Format

See [../../docs/TRACK_FILE_FORMAT.md](../../docs/TRACK_FILE_FORMAT.md) for the complete specification.

## Track Structure

### Required Fields

- **name**: String - Display name of the track
- **nodes**: Array - Centerline control points (minimum 2 required)
- **default_width**: Float - Default track width in meters (if not specified per-node)

### Optional Fields

- **closed_loop**: Boolean - Whether the track forms a closed loop (default: false)
- **checkpoints**: Array - Checkpoint definitions for lap timing
- **spawn_points**: Array - Custom starting grid positions
- **raceline**: Array - Optimal racing line for AI (automatically included in converted tracks)
- **metadata**: Object - Track information (location, year, category, etc.)

## Node Format

Each node defines a control point for the track centerline:

### Required per Node
- **x**: Float - X coordinate in meters
- **y**: Float - Y coordinate in meters

### Optional per Node
- **z**: Float - Z coordinate/elevation in meters (default: 0.0)
- **width**: Float - Total track width at this point (symmetric)
- **width_left**: Float - Width to left of centerline (for asymmetric tracks)
- **width_right**: Float - Width to right of centerline (for asymmetric tracks)
- **banking**: Float - Banking angle in radians (default: 0.0)
  - Positive = banked towards inside of turn
  - Example: 0.122 radians ≈ 7 degrees
- **friction**: Float - Grip modifier (default: 1.0)
  - 1.0 = normal grip
  - 0.9 = 10% less grip
- **surface_type**: String - Surface material (default: "Asphalt")
  - Options: "Asphalt", "Concrete", "Curb", "Grass", "Gravel", "Sand", "Wet"

## Checkpoints

Define lap counting checkpoints:

```yaml
checkpoints:
  - index_start: 0    # Node index where checkpoint starts
    index_end: 1      # Node index where checkpoint ends
```

## Spawn Points

Define custom starting grid positions:

```yaml
spawn_points:
  - position: 0       # Node index for this spawn point
    offset_x: 0.0     # Lateral offset in meters
    offset_y: 0.0     # Longitudinal offset in meters
```

If spawn_points are not specified, the server generates a default grid layout at the first node.

## Track Generation

The track loader performs the following operations:

1. **Spline Interpolation**: Uses Catmull-Rom splines to generate smooth curves between control nodes
   - 20 interpolated points per segment
   - Smooth transitions between straight and curved sections

2. **Derived Properties**: Automatically calculates:
   - Distance from start for each point
   - Heading (direction) at each point
   - Slope (grade) from elevation changes
   - Track progress meters for lap timing

3. **Mesh Generation**: Creates renderable 3D geometry:
   - Left and right track edges based on width
   - Banking applied as vertical offset
   - Smooth normals for realistic lighting
   - UV coordinates for texturing

## Example: Simple Oval (YAML)

```yaml
name: "Simple Oval Track"
default_width: 12.0
closed_loop: true

nodes:
  # Start straight
  - x: 0.0
    y: 0.0
    z: 0.0

  - x: 200.0
    y: 0.0
    z: 0.0

  # Turn 1 (banked)
  - x: 250.0
    y: 50.0
    z: 2.0
    banking: 0.122

  # Back straight
  - x: 270.0
    y: 400.0
    z: 2.0

  # Turn 2 (banked)
  - x: 200.0
    y: 500.0
    z: 0.0
    banking: 0.122

  # Complete the loop
  - x: 0.0
    y: 500.0
    z: 0.0

checkpoints:
  - index_start: 0
    index_end: 1
```

## Example: Street Circuit (JSON)

```json
{
  "name": "Monaco-Style Street Circuit",
  "default_width": 10.0,
  "closed_loop": true,
  "nodes": [
    {"x": 0.0, "y": 0.0, "z": 0.0},
    {"x": 100.0, "y": 0.0, "z": 0.0},
    {
      "x": 130.0,
      "y": 30.0,
      "z": 0.0,
      "width": 8.0,
      "friction": 0.95
    },
    {"x": 130.0, "y": 80.0, "z": 2.0},
    {"x": 0.0, "y": 140.0, "z": 5.0}
  ]
}
```

## Physics Integration

The track format integrates with ApexSim's physics engine:

- **Banking**: Creates centripetal force component helping cars through turns
- **Elevation**: Affects weight distribution and acceleration/braking
- **Surface Type**: Modifies tire grip coefficients
- **Friction Modifier**: Fine-tunes grip per section
- **Track Width**: Used for off-track detection and penalties

## Mesh Export

Track meshes can be exported for use in game engines:

- **OBJ format**: For Blender, Maya, and other 3D tools
- **glTF/GLB format**: For Unreal Engine, Unity, web viewers

The server's `track_mesh` module provides export functions.

## AI Integration

The AI driver system uses the centerline for:
- Path following and steering calculations
- Look-ahead distance for corner prediction
- Target speed adjustment based on curvature
- Optimal racing line approximation

## Tips for Track Design

1. **Use adequate nodes**: More nodes = smoother curves, but place them strategically at key points
2. **Banking for high-speed turns**: Use 5-15 degrees (0.087-0.262 rad) for oval turns
3. **Elevation changes**: Keep slopes under 10% (5.7 degrees) for realism
4. **Width variation**: Narrow sections increase difficulty
5. **Friction zones**: Lower friction for wet patches or marbles off-line
6. **Closed loop**: Ensure first and last nodes are close for seamless lap transitions

## Validation

The track loader validates:
- Minimum 2 nodes required
- Valid checkpoint indices (must reference existing nodes)
- Default width or per-node width must be specified
- Proper JSON/YAML syntax

Invalid tracks will log warnings and be skipped during server startup.

# ApexSim Track File Format

## Overview

ApexSim tracks are defined using a parametric centerline spline with rich metadata. Tracks can be created manually or converted from real-world data using the [Track Converter Tool](TRACK_CONVERTER.md).

## Core Concept

Store the track as:
- **Centerline spline** (polyline or Bézier) with per-node properties
- **Metadata tables** (checkpoints, spawn points, raceline)
- **Track information** (name, location, dimensions)

The server uses the spline to compute distances, lap times, and off-track detection.
The client (Unreal or custom engine) can generate track mesh procedurally from the spline.

## File Formats

Tracks can be stored in either:
- **YAML** (recommended for human editing)
- **JSON** (for programmatic generation)

## Complete Data Structure

```yaml
name: "Track Name"
default_width: 12.0
closed_loop: true

nodes:
  - x: 0.0
    y: 0.0
    z: 0.0
    width: 15.0              # Total width (symmetric)
    width_left: 7.5          # Width to left of centerline
    width_right: 7.5         # Width to right of centerline
    banking: 0.0             # Banking angle in radians
    friction: 1.0            # Grip modifier (1.0 = normal)
    surface_type: "Asphalt"  # Surface material

checkpoints:
  - index_start: 0
    index_end: 10

spawn_points:
  - position: 0
    offset_x: 0.0
    offset_y: 0.0

raceline:
  - x: 0.0
    y: 0.0
    z: 0.0

metadata:
  country: "Country Name"
  city: "City Name"
  length_m: 5000.0
  description: "Track description"
  year_built: 2020
  category: "F1"
```

See [TRACK_FILE_FORMAT.md](TRACK_FILE_FORMAT.md) for complete field descriptions.

## Quick Start

### Use Pre-converted Real-World Tracks

```bash
# 25+ F1, DTM, and IndyCar tracks included!
ls content/tracks/real/

# Edit server.toml
[track]
track_file = "./content/tracks/real/Spa.yaml"
```

### Convert Your Own Tracks

```bash
./server/convert_all_tracks.sh /path/to/racetrack-database ./content/tracks/real
```

See [TRACK_CONVERTER.md](TRACK_CONVERTER.md) for details.

# Track Converter Tool

## Overview

The ApexSim track converter tool converts race track data from the [racetrack-database](https://github.com/TUMFTM/racetrack-database) CSV format to ApexSim's native YAML/JSON track format. The converted tracks include:

- **Centerline data** with asymmetric track widths
- **Optimal raceline** data for AI drivers
- **Track metadata** (country, city, length, category, year built)
- **Physical properties** (banking, friction, surface type)

## Features

✅ **25+ Real-World Tracks** - F1, DTM, and IndyCar circuits  
✅ **Asymmetric Track Widths** - Precise left/right width data  
✅ **Optimal Racing Lines** - AI-optimized racing lines from minimum curvature optimization  
✅ **Rich Metadata** - Track information including location, category, and construction year  
✅ **Batch Conversion** - Convert entire database in one command  
✅ **Flexible Output** - YAML or JSON format support  

## Available Tracks

All tracks from the racetrack-database have been converted and are available in `content/tracks/real/`:

### Formula 1
- Circuit of The Americas (Austin, USA) - 5.51 km
- Albert Park Circuit (Melbourne, Australia) - 5.30 km
- Autódromo Hermanos Rodríguez (Mexico City, Mexico) - 4.30 km
- Circuit Gilles Villeneuve (Montreal, Canada) - 4.36 km
- Autodromo Nazionale di Monza (Italy) - 5.79 km
- Bahrain International Circuit (Sakhir, Bahrain) - 5.41 km
- Autódromo José Carlos Pace (São Paulo, Brazil) - 4.31 km
- Sepang International Circuit (Malaysia) - 5.54 km
- Shanghai International Circuit (China) - 5.45 km
- Silverstone Circuit (UK) - 5.89 km
- Sochi Autodrom (Russia) - 5.85 km
- Circuit de Spa-Francorchamps (Belgium) - 7.00 km ⭐
- Suzuka Circuit (Japan) - 5.81 km
- Hungaroring (Budapest, Hungary) - 4.38 km
- Circuit de Barcelona-Catalunya (Spain) - 4.66 km
- Hockenheimring (Germany) - 4.57 km
- Yas Marina Circuit (Abu Dhabi, UAE) - 5.55 km

### DTM
- Brands Hatch (UK) - 3.91 km
- Moscow Raceway (Russia) - 3.93 km
- Norisring (Germany) - 2.30 km
- Nürburgring (Germany) - 5.14 km
- Motorsport Arena Oschersleben (Germany) - 3.70 km
- Red Bull Ring (Spielberg, Austria) - 4.32 km
- Circuit Zandvoort (Netherlands) - 4.31 km

### IndyCar
- Indianapolis Motor Speedway (USA) - 4.02 km

## Installation

The converter is built as part of the ApexSim server:

```bash
cd server
cargo build --release --bin convert_track
```

## Usage

### Single Track Conversion

```bash
./target/release/convert_track \\
  --tracks-csv /path/to/tracks/Monza.csv \\
  --raceline-csv /path/to/racelines/Monza.csv \\
  --output ./content/tracks/real/monza.yaml \\
  --name "Autodromo Nazionale di Monza" \\
  --country "Italy" \\
  --city "Monza" \\
  --category "F1" \\
  --year-built 1922
```

### Batch Conversion

Convert all tracks from the racetrack-database:

```bash
./convert_all_tracks.sh /path/to/racetrack-database ./content/tracks/real
```

This will:
1. Build the converter tool
2. Process all 25+ tracks
3. Include raceline data where available
4. Add metadata for each track
5. Output YAML files ready for use

## Command-Line Options

| Option | Short | Description | Required |
|--------|-------|-------------|----------|
| `--tracks-csv` | `-t` | Path to track centerline CSV file | ✅ |
| `--raceline-csv` | `-r` | Path to raceline CSV file | ❌ |
| `--output` | `-o` | Output file path (.yaml or .json) | ✅ |
| `--name` | `-n` | Track display name | ✅ |
| `--country` | | Country where track is located | ❌ |
| `--city` | | City where track is located | ❌ |
| `--category` | | Track category (F1, DTM, IndyCar, etc.) | ❌ |
| `--year-built` | | Year the track was constructed | ❌ |
| `--description` | | Track description | ❌ |
| `--format` | `-f` | Output format (yaml/json, auto-detected) | ❌ |
| `--elevation` | | Elevation mode: flat or auto-compute | ❌ |
| `--friction` | | Default friction coefficient (default: 1.0) | ❌ |
| `--closed-loop` | | Track is a closed loop (default: true) | ❌ |

## Input Data Format

### Track Centerline CSV
Format: `x_m, y_m, w_tr_right_m, w_tr_left_m`

```csv
# x_m,y_m,w_tr_right_m,w_tr_left_m
0.960975,4.022273,7.565,7.361
4.935182,0.985988,7.584,7.382
8.909306,-2.050381,7.603,7.403
...
```

### Raceline CSV
Format: `x_m, y_m`

```csv
# x_m,y_m
-2.842561,-0.963418
1.142325,-3.976526
5.127553,-6.989182
...
```

## Output Track Format

The converter produces ApexSim track files with the following structure:

```yaml
name: "Autodromo Nazionale di Monza"
default_width: 11.671
closed_loop: true

nodes:
  - x: -0.320123
    y: 1.087714
    z: 0.0
    width_left: 5.932
    width_right: 5.739
    banking: 0.0
    friction: 1.0
    surface_type: Asphalt
  # ... more nodes

checkpoints: []
spawn_points: []

raceline:
  - x: 3.712166
    y: 0.315247
    z: 0.0
  # ... more raceline points

metadata:
  country: Italy
  city: Monza
  length_m: 5790.201
  description: null
  year_built: 1922
  category: F1
```

## Enhanced Track Format Features

### Asymmetric Track Widths

Unlike the previous format that used a single `width` value, the enhanced format supports independent left and right track widths:

```yaml
nodes:
  - x: 100.0
    y: 50.0
    width_left: 6.5   # Width to the left of centerline
    width_right: 8.2  # Width to the right of centerline
```

This allows for accurate representation of real-world tracks where the track width varies asymmetrically around the centerline.

### Raceline Data

The optimal racing line is included for AI drivers and can be visualized in the game client:

```yaml
raceline:
  - x: 3.712
    y: 0.315
    z: 0.0
```

These racing lines are computed using minimum curvature optimization algorithms from the TUM research team.

### Track Metadata

Rich metadata is preserved for each track:

```yaml
metadata:
  country: Belgium
  city: Stavelot
  length_m: 7000.044
  category: F1
  year_built: 1921
```

## Data Sources

The original data comes from the [TUM FTMR racetrack-database](https://github.com/TUMFTM/racetrack-database):

- **Centerlines**: GPS data from OpenStreetMap, smoothed
- **Track widths**: Extracted from satellite imagery
- **Racing lines**: Computed using minimum curvature optimization

## Using Converted Tracks

### In Server Configuration

Edit your `server.toml`:

```toml
[track]
# Use any of the real-world tracks
track_file = "./content/tracks/real/Spa.yaml"
```

### Load Programmatically

```rust
use apexsim_server::track_loader::TrackLoader;

let track = TrackLoader::load_from_file("content/tracks/real/Monza.yaml")?;
println!("Track: {}", track.name);
println!("Length: {:.2} km", track.metadata.length_m.unwrap() / 1000.0);
println!("Raceline points: {}", track.raceline.len());
```

## Examples

### Convert with JSON output

```bash
./target/release/convert_track \\
  --tracks-csv ~/racetrack-database/tracks/Spa.csv \\
  --raceline-csv ~/racetrack-database/racelines/Spa.csv \\
  --output spa.json \\
  --name "Circuit de Spa-Francorchamps" \\
  --format json
```

### Convert without raceline

```bash
./target/release/convert_track \\
  --tracks-csv ~/custom-track.csv \\
  --output custom.yaml \\
  --name "My Custom Track" \\
  --closed-loop false
```

## Track Statistics

After conversion, the tool displays:
- Number of centerline points loaded
- Number of raceline points loaded
- Calculated track length in meters and kilometers

Example output:
```
Converting track: Autodromo Nazionale di Monza
Reading centerline from: /path/to/Monza.csv
  Loaded 1159 centerline points
Reading raceline from: /path/to/Monza.csv
  Loaded 1152 raceline points
  Track length: 5790.20 meters (5.79 km)
Writing to: monza.yaml
✓ Conversion completed successfully!
```

## Troubleshooting

### Build Errors

If you get compilation errors, make sure you're in the server directory:
```bash
cd server
cargo clean
cargo build --release --bin convert_track
```

### Missing Raceline Data

If raceline CSV doesn't exist, the tool will still convert the track without it:
```bash
# Raceline is optional
./target/release/convert_track \\
  --tracks-csv track.csv \\
  --output track.yaml \\
  --name "Track Name"
```

### Invalid CSV Format

Ensure CSV files match the expected format:
- Track centerline: 4 columns (x, y, width_right, width_left)
- Raceline: 2 columns (x, y)
- Header line starting with `#` is automatically skipped

## Future Enhancements

Planned improvements:
- [ ] Elevation computation from GPS data
- [ ] Banking angle estimation from track geometry
- [ ] Automatic checkpoint generation
- [ ] Pit lane data extraction
- [ ] Surface type detection from satellite imagery

## Credits

- **Source Data**: [TUM FTMR racetrack-database](https://github.com/TUMFTM/racetrack-database)
- **Research Team**: Chair of Automotive Technology, Technical University of Munich
- **GPS Data**: OpenStreetMap contributors
- **Racing Line Optimization**: [Global Race Trajectory Optimization](https://github.com/TUMFTM/global_racetrajectory_optimization)

## License

The converter tool is part of ApexSim. The source track data is from the racetrack-database project and maintains its original license.

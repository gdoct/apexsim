# Track Data Conversion - Quick Reference

## What Was Created

### 1. Enhanced Track Format
- ✅ Added asymmetric track width support (`width_left`, `width_right`)
- ✅ Added raceline data for AI optimization
- ✅ Added rich metadata (country, city, length, category, year)
- ✅ Maintained backward compatibility with existing tracks

### 2. Track Converter Tool
**Location**: `server/src/bin/convert_track.rs`

Converts CSV track data from [racetrack-database](https://github.com/TUMFTM/racetrack-database) to ApexSim format.

### 3. Batch Conversion Script
**Location**: `server/convert_all_tracks.sh`

Converts all tracks in one command with full metadata.

### 4. Real-World Tracks
**Location**: `content/tracks/real/`

25+ professional race tracks from F1, DTM, and IndyCar championships.

## Quick Start

### Use Existing Converted Tracks

Edit `server/server.toml`:
```toml
[track]
track_file = "./content/tracks/real/Spa.yaml"
```

Available tracks:
- **F1**: Spa, Monza, Silverstone, Suzuka, Monaco, and 12 more
- **DTM**: Nürburgring, Brands Hatch, Norisring, and 4 more  
- **IndyCar**: Indianapolis Motor Speedway

### Convert Single Track

```bash
cd server

./target/release/convert_track \
  --tracks-csv /path/to/tracks/Monza.csv \
  --raceline-csv /path/to/racelines/Monza.csv \
  --output ../content/tracks/real/monza.yaml \
  --name "Autodromo Nazionale di Monza" \
  --country "Italy" \
  --city "Monza" \
  --category "F1" \
  --year-built 1922
```

### Batch Convert All Tracks

```bash
cd server
./convert_all_tracks.sh /path/to/racetrack-database ../content/tracks/real
```

Output:
```
Converting: Circuit de Spa-Francorchamps
  Track CSV: /path/to/tracks/Spa.csv
  Raceline CSV: /path/to/racelines/Spa.csv
  Output: ../content/tracks/real/Spa.yaml
  ✓ Success

...

Total tracks: 25
Successful: 25
Failed: 0
```

## Files Modified/Created

### Core Server Files
- `server/src/data.rs` - Added `RacelinePoint` and `TrackMetadata` structs
- `server/src/track_loader.rs` - Enhanced with raceline and metadata support
- `server/src/bin/convert_track.rs` - **NEW** Track converter tool

### Scripts
- `server/convert_all_tracks.sh` - **NEW** Batch conversion script

### Documentation
- `docs/TRACK_CONVERTER.md` - **NEW** Complete converter documentation
- `docs/TRACK_FILE_FORMAT.md` - Updated with new format features
- `content/tracks/README.md` - Updated with real track info
- `content/tracks/real/README.md` - **NEW** Real track catalog

### Track Data
- `content/tracks/real/*.yaml` - **NEW** 25 converted real-world tracks

### Examples
- `server/examples/load_real_track.rs` - **NEW** Example loading real tracks

## Track Format Examples

### Before (Simple)
```yaml
name: "Track"
nodes:
  - { x: 0, y: 0, width: 15.0 }
  - { x: 100, y: 0, width: 15.0 }
```

### After (Enhanced)
```yaml
name: "Circuit de Spa-Francorchamps"
default_width: 12.0
closed_loop: true

nodes:
  - x: 0.0
    y: 0.0
    width_left: 6.5
    width_right: 6.0
    banking: 0.0
    friction: 1.0
    surface_type: Asphalt

raceline:
  - { x: 1.2, y: 0.3, z: 0.0 }
  - { x: 5.4, y: 1.8, z: 0.0 }

metadata:
  country: Belgium
  city: Stavelot
  length_m: 7000.044
  category: F1
  year_built: 1921
```

## Verification

Test that a track loads correctly:
```bash
cd server
cargo run --release --example load_real_track
```

Expected output:
```
✓ Successfully loaded track: Circuit de Spa-Francorchamps
  Centerline points: 28020
  Raceline points: 1388
  Start positions: 16
  Length: 7.00 km
  Location: Stavelot, Belgium
  Category: F1
  Built: 1921

✓ Track validation passed!
```

## Data Sources

All converted tracks use data from [TUM FTMR racetrack-database](https://github.com/TUMFTM/racetrack-database):

- **Centerlines**: GPS coordinates from OpenStreetMap (smoothed)
- **Track widths**: Extracted from satellite imagery
- **Racing lines**: Minimum curvature optimization algorithm

## What You Can Do Now

1. **Use 25+ Real Tracks** - Just update `server.toml`
2. **Convert More Tracks** - Use the converter tool on any CSV track data
3. **Create Custom Tracks** - Use enhanced format with racelines and metadata
4. **AI Racing Lines** - All converted tracks include optimal racing lines
5. **Realistic Track Widths** - Asymmetric widths match real-world tracks

## Next Steps

To use a real-world track:

1. Edit `server/server.toml`:
   ```toml
   [track]
   track_file = "./content/tracks/real/Spa.yaml"
   ```

2. Run the server:
   ```bash
   cd server
   cargo run --release
   ```

3. The track will load with:
   - ✅ Accurate centerline (28,020 points for Spa)
   - ✅ Real track widths (asymmetric)
   - ✅ Optimal racing line (1,388 points)
   - ✅ Complete metadata

## Troubleshooting

### Build the converter
```bash
cd server
cargo build --release --bin convert_track
```

### Track not loading
Check the track file path in `server.toml` is relative to the server directory.

### Missing raceline
Raceline is optional - tracks work fine without it.

### CSV format errors
Ensure CSV files match the expected format:
- Centerline: `x_m,y_m,w_tr_right_m,w_tr_left_m`
- Raceline: `x_m,y_m`

## Documentation

For complete details, see:
- [TRACK_CONVERTER.md](TRACK_CONVERTER.md) - Converter tool documentation
- [TRACK_FILE_FORMAT.md](TRACK_FILE_FORMAT.md) - Track format specification
- [content/tracks/real/README.md](../content/tracks/real/README.md) - Track catalog

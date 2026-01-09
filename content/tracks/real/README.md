# Real-World Race Tracks

This directory contains 25+ professionally converted real-world race tracks from Formula 1, DTM, and IndyCar championships.

## Track List

### Formula 1 Circuits

| Track | Location | Length | Year Built |
|-------|----------|--------|------------|
| Circuit of The Americas | Austin, USA | 5.51 km | 2012 |
| Albert Park Circuit | Melbourne, Australia | 5.30 km | 1996 |
| Autódromo Hermanos Rodríguez | Mexico City, Mexico | 4.30 km | 1962 |
| Circuit Gilles Villeneuve | Montreal, Canada | 4.36 km | 1978 |
| Autodromo Nazionale di Monza | Monza, Italy | 5.79 km | 1922 |
| Bahrain International Circuit | Sakhir, Bahrain | 5.41 km | 2004 |
| Autódromo José Carlos Pace | São Paulo, Brazil | 4.31 km | 1940 |
| Sepang International Circuit | Sepang, Malaysia | 5.54 km | 1999 |
| Shanghai International Circuit | Shanghai, China | 5.45 km | 2004 |
| Silverstone Circuit | Silverstone, UK | 5.89 km | 1948 |
| Sochi Autodrom | Sochi, Russia | 5.85 km | 2014 |
| **Circuit de Spa-Francorchamps** ⭐ | Stavelot, Belgium | **7.00 km** | 1921 |
| Suzuka Circuit | Suzuka, Japan | 5.81 km | 1962 |
| Hungaroring | Budapest, Hungary | 4.38 km | 1986 |
| Circuit de Barcelona-Catalunya | Montmeló, Spain | 4.66 km | 1991 |
| Hockenheimring | Hockenheim, Germany | 4.57 km | 1932 |
| Yas Marina Circuit | Abu Dhabi, UAE | 5.55 km | 2009 |

### DTM Circuits

| Track | Location | Length | Year Built |
|-------|----------|--------|------------|
| Brands Hatch | West Kingsdown, UK | 3.91 km | 1926 |
| Moscow Raceway | Volokolamsk, Russia | 3.93 km | 2012 |
| Norisring | Nuremberg, Germany | 2.30 km | 1947 |
| Nürburgring | Nürburg, Germany | 5.14 km | 1927 |
| Motorsport Arena Oschersleben | Oschersleben, Germany | 3.70 km | 1997 |
| Red Bull Ring | Spielberg, Austria | 4.32 km | 1969 |
| Circuit Zandvoort | Zandvoort, Netherlands | 4.31 km | 1948 |

### IndyCar Circuits

| Track | Location | Length | Year Built |
|-------|----------|--------|------------|
| Indianapolis Motor Speedway | Indianapolis, USA | 4.02 km | 1909 |

## Data Quality

All tracks include:
- ✅ Accurate GPS-based centerlines
- ✅ Real track widths from satellite imagery
- ✅ Optimized racing lines (minimum curvature algorithm)
- ✅ Metadata (location, construction year, category)

## Usage

### In Server Config

Edit your `server.toml`:

```toml
[track]
track_file = "./content/tracks/real/Spa.yaml"
```

### Quick Test

Try the iconic Spa-Francorchamps:
```bash
# Update server.toml
track_file = "./content/tracks/real/Spa.yaml"

# Run server
cargo run --release
```

## Featured Tracks

### 🏎️ Spa-Francorchamps (Belgium)
- **Length**: 7.00 km - Longest track in the collection
- **Famous for**: Eau Rouge, Raidillon, Blanchimont
- **Characteristics**: Fast, flowing, elevation changes

### 🏎️ Monza (Italy)
- **Length**: 5.79 km - "Temple of Speed"
- **Famous for**: Parabolica, Lesmo corners
- **Characteristics**: High-speed straights, historic venue

### 🏎️ Suzuka (Japan)
- **Length**: 5.81 km - Figure-8 layout
- **Famous for**: 130R, Spoon Curve, Degner
- **Characteristics**: Technical, challenging, unique layout

### 🏎️ Silverstone (UK)
- **Length**: 5.89 km - Home of British GP
- **Famous for**: Copse, Maggots-Becketts, Stowe
- **Characteristics**: Fast corners, historic

## File Format

Each track file contains:

```yaml
name: "Track Name"
nodes: [...]              # Centerline points with widths
raceline: [...]           # Optimal racing line
default_width: 10.523189
closed_loop: true
metadata:
  country: "Country"
  city: "City"
  length_m: 5000.0
  year_built: 1922
  category: "F1"
```

nodes example:
```yaml
- x: -1.683339
  y: -1.878198
  z: 0.0
  width: null
  width_left: 5.271
  width_right: 5.074
  banking: 0.0
  friction: 1.0
  surface_type: Asphalt
- x: 0.151452
  y: 2.772507
  z: 0.0
  width: null
  width_left: 5.295
  width_right: 5.099
  banking: 0.0
  friction: 1.0
  surface_type: Asphalt
```


raceline example:
```yaml
raceline:
- x: -5.806014
  y: -0.260481
  z: 0.0
- x: -3.998254
  y: 4.401025
  z: 0.0

```

## Data Source

Tracks converted from the [TUM FTMR racetrack-database](https://github.com/TUMFTM/racetrack-database):
- GPS centerlines from OpenStreetMap
- Track widths from satellite imagery analysis
- Racing lines from minimum curvature optimization

## Regenerating Tracks

To reconvert tracks from the source database:

```bash
cd ../server
./convert_all_tracks.sh /path/to/racetrack-database ./content/tracks/real
```

See [../../docs/TRACK_CONVERTER.md](../../docs/TRACK_CONVERTER.md) for details.

## License

Source data from racetrack-database maintains its original licensing.
ApexSim track format and conversion tools are part of the ApexSim project.

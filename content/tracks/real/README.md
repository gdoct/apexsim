# Real-World Race Tracks

The circuits here are modelled on real ones, but the game never shows their
real names: "Circuit Zandvoort", "Spa-Francorchamps", "Suzuka Circuit",
"Nürburgring" and the rest are registered trademarks of the circuit
operators. Each YAML keeps the real name in `name` (source data) and carries
what the game shows in `display_name`, a parody that still says which
circuit it is; the server sends `display_name` as the track's name.
`metadata.description` says what it is modelled on by *place*, never by the
operator's name ("Modelled on the circuit in the dunes at Zandvoort,
Netherlands."). The same goes for the corner boards (`display_name` on each
corner of the `<Stem>.layout.json` dossier, from `CORNER_DISPLAY` in
`scripts/osm_layout.py`) and the categories, which the client shows as
Formula / Endurance / GT3 / Independent. When adding a track, give it both.
The file stem stays the place (`Zandvoort.yaml`); it is what scripts, the
docs and `-ApexTrack=` use.

| In game | Real name (not shown) | Stem | Modelled on the circuit at | Length | Category shown (data) |
|---------|------------------------|------|----------------------------|--------|-----------------------|
| Albert Parkour Circuit | Albert Park Circuit | `Melbourne` | Melbourne, Australia | 5.29 km | Formula (F1) |
| Autodromo Monzarella | Autodromo Nazionale di Monza | `Monza` | Monza, Italy | 5.78 km | Formula (F1) |
| Autódromo Hermanos Rodri-Queso | Autódromo Hermanos Rodríguez | `MexicoCity` | Mexico City, Mexico | 4.28 km | Formula (F1) |
| Autódromo Interlaggos | Autódromo José Carlos Pace | `SaoPaulo` | São Paulo, Brazil | 4.30 km | Formula (F1) |
| Bahrainless International Circuit | Bahrain International Circuit | `Sakhir` | Sakhir, Bahrain | 5.39 km | Formula (F1) |
| Brands Scratch | Brands Hatch | `BrandsHatch` | West Kingsdown, United Kingdom | 3.90 km | GT3 (DTM) |
| Circuit Chilly Villeneuve | Circuit Gilles Villeneuve | `Montreal` | Montreal, Canada | 4.35 km | Formula (F1) |
| Circuit de Barcelunatic | Circuit de Barcelona-Catalunya | `Catalunya` | Montmeló, Spain | 4.64 km | Formula (F1) |
| Circuit of the Armadillos | Circuit of The Americas | `Austin` | Austin, United States | 5.49 km | Formula (F1) |
| Endianapolis Motor Speedway | Indianapolis Motor Speedway | `IMS` | Indianapolis, United States | 4.02 km | Independent (IndyCar) |
| Gnocchi Autodrom | Sochi Autodrom | `Sochi` | Sochi, Russia | 5.83 km | Formula (F1) |
| Hockeyheimring | Hockenheimring | `Hockenheim` | Hockenheim, Germany | 4.56 km | Formula (F1) |
| Hungoverring | Hungaroring | `Budapest` | Budapest, Hungary | 4.37 km | Formula (F1) |
| Lemons – Circuit du Peuple | Circuit de la Sarthe (Le Mans 24 Hours) | `LeMans` | Le Mans, France | 13.62 km | Endurance (WEC) |
| Moscow Mule Raceway | Moscow Raceway | `MoscowRaceway` | Volokolamsk, Russia | 4.05 km | GT3 (DTM) |
| Motorsport Arena Oskarsleben | Motorsport Arena Oschersleben | `Oschersleben` | Oschersleben, Germany | 3.69 km | GT3 (DTM) |
| Nürburger Mordschleife | Nürburgring Nordschleife | `Nordschleife` | Nürburg, Germany | 20.76 km | Endurance (Endurance) |
| Nürburgerring | Nürburgring | `Nuerburgring` | Nürburg, Germany | 5.14 km | GT3 (DTM) |
| Red Pull Ring | Red Bull Ring | `Spielberg` | Spielberg, Austria | 4.31 km | Formula (F1) |
| Shanghaied International Circuit | Shanghai International Circuit | `Shanghai` | Shanghai, China | 5.43 km | Formula (F1) |
| Shebang International Circuit | Sepang International Circuit | `Sepang` | Sepang, Malaysia | 5.53 km | Formula (F1) |
| Shiverstone Circuit | Silverstone Circuit | `Silverstone` | Silverstone, United Kingdom | 5.88 km | Formula (F1) |
| Snorisring | Norisring | `Norisring` | Nuremberg, Germany | 2.28 km | GT3 (DTM) |
| Spa-Frankenchamps | Circuit de Spa-Francorchamps | `Spa` | Stavelot, Belgium | 6.99 km | Formula (F1) |
| Sudoku Circuit | Suzuka Circuit | `Suzuka` | Suzuka, Japan | 5.80 km | Formula (F1) |
| Yas Marinara Circuit | Yas Marina Circuit | `YasMarina` | Abu Dhabi, United Arab Emirates | 5.52 km | Formula (F1) |
| Zandervoort | Circuit Zandvoort | `Zandvoort` | Zandvoort, Netherlands | 4.31 km | GT3 (DTM) |

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

Try Spa-Frankenchamps:
```bash
# Update server.toml
track_file = "./content/tracks/real/Spa.yaml"

# Run server
cargo run --release
```

## Featured Tracks

### 🏎️ Spa-Frankenchamps (Spa, Belgium)
- **Length**: 7.00 km - Longest track in the collection
- **Famous for**: Eau Rouge, Raidillon, Blanchimont
- **Characteristics**: Fast, flowing, elevation changes

### 🏎️ Autodromo Monzarella (Monza, Italy)
- **Length**: 5.79 km
- **Famous for**: Parabolica, Lesmo corners
- **Characteristics**: High-speed straights, historic venue

### 🏎️ Sudoku Circuit (Suzuka, Japan)
- **Length**: 5.81 km - Figure-8 layout
- **Famous for**: 130R, Spoon Curve, Degner
- **Characteristics**: Technical, challenging, unique layout

### 🏎️ Shiverstone Circuit (Silverstone, UK)
- **Length**: 5.89 km - Home of British GP
- **Famous for**: Copse, Maggots-Becketts, Stowe
- **Characteristics**: Fast corners, historic

## File Format

Each track file contains:

```yaml
name: "Track Name"           # the real name: source data, never shown
display_name: "Trak Naim"    # what the game shows: a parody of a trademarked name
nodes: [...]              # Centerline points with widths
raceline: [...]           # Optimal racing line
default_width: 10.523189
closed_loop: true
metadata:
  country: "Country"
  city: "City"
  length_m: 5000.0
  description: "Modelled on the circuit at <place>, <country>."
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

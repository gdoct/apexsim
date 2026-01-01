#!/bin/bash
# Batch convert all tracks from racetrack-database to ApexSim format
#
# Usage: ./convert_all_tracks.sh /path/to/racetrack-database /path/to/output

set -e

if [ $# -ne 2 ]; then
    echo "Usage: $0 <racetrack-database-path> <output-directory>"
    echo "Example: $0 ~/racetrack-database ./content/tracks/real"
    exit 1
fi

DATABASE_PATH="$1"
OUTPUT_DIR="$2"

# Check if database path exists
if [ ! -d "$DATABASE_PATH" ]; then
    echo "Error: Database path does not exist: $DATABASE_PATH"
    exit 1
fi

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Track metadata mapping (track name -> country, city, category)
# Format: "filename|display_name|country|city|category|year"
declare -A TRACK_INFO=(
    ["Austin"]="Circuit of The Americas|United States|Austin|F1|2012"
    ["BrandsHatch"]="Brands Hatch|United Kingdom|West Kingsdown|DTM|1926"
    ["Budapest"]="Hungaroring|Hungary|Budapest|F1|1986"
    ["Catalunya"]="Circuit de Barcelona-Catalunya|Spain|Montmeló|F1|1991"
    ["Hockenheim"]="Hockenheimring|Germany|Hockenheim|F1|1932"
    ["IMS"]="Indianapolis Motor Speedway|United States|Indianapolis|IndyCar|1909"
    ["Melbourne"]="Albert Park Circuit|Australia|Melbourne|F1|1996"
    ["MexicoCity"]="Autódromo Hermanos Rodríguez|Mexico|Mexico City|F1|1962"
    ["Montreal"]="Circuit Gilles Villeneuve|Canada|Montreal|F1|1978"
    ["Monza"]="Autodromo Nazionale di Monza|Italy|Monza|F1|1922"
    ["MoscowRaceway"]="Moscow Raceway|Russia|Volokolamsk|DTM|2012"
    ["Norisring"]="Norisring|Germany|Nuremberg|DTM|1947"
    ["Nuerburgring"]="Nürburgring|Germany|Nürburg|DTM|1927"
    ["Oschersleben"]="Motorsport Arena Oschersleben|Germany|Oschersleben|DTM|1997"
    ["Sakhir"]="Bahrain International Circuit|Bahrain|Sakhir|F1|2004"
    ["SaoPaulo"]="Autódromo José Carlos Pace|Brazil|São Paulo|F1|1940"
    ["Sepang"]="Sepang International Circuit|Malaysia|Sepang|F1|1999"
    ["Shanghai"]="Shanghai International Circuit|China|Shanghai|F1|2004"
    ["Silverstone"]="Silverstone Circuit|United Kingdom|Silverstone|F1|1948"
    ["Sochi"]="Sochi Autodrom|Russia|Sochi|F1|2014"
    ["Spa"]="Circuit de Spa-Francorchamps|Belgium|Stavelot|F1|1921"
    ["Spielberg"]="Red Bull Ring|Austria|Spielberg|F1|1969"
    ["Suzuka"]="Suzuka Circuit|Japan|Suzuka|F1|1962"
    ["YasMarina"]="Yas Marina Circuit|United Arab Emirates|Abu Dhabi|F1|2009"
    ["Zandvoort"]="Circuit Zandvoort|Netherlands|Zandvoort|DTM|1948"
)

echo "======================================"
echo "ApexSim Track Batch Converter"
echo "======================================"
echo "Database: $DATABASE_PATH"
echo "Output: $OUTPUT_DIR"
echo ""

# Build the converter tool
echo "Building converter tool..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
cargo build --release --bin convert_track
echo ""

TRACKS_DIR="$DATABASE_PATH/tracks"
RACELINES_DIR="$DATABASE_PATH/racelines"

# Counter
TOTAL=0
SUCCESS=0
FAILED=0

for track_csv in "$TRACKS_DIR"/*.csv; do
    if [ ! -f "$track_csv" ]; then
        continue
    fi
    
    TOTAL=$((TOTAL + 1))
    
    # Extract track name (filename without extension)
    track_name=$(basename "$track_csv" .csv)
    
    # Check if raceline exists
    raceline_csv="$RACELINES_DIR/${track_name}.csv"
    raceline_arg=""
    if [ -f "$raceline_csv" ]; then
        raceline_arg="--raceline-csv $raceline_csv"
    fi
    
    # Get track metadata
    info="${TRACK_INFO[$track_name]}"
    if [ -z "$info" ]; then
        echo "⚠ Warning: No metadata for $track_name, using defaults"
        display_name="$track_name"
        country=""
        city=""
        category=""
        year=""
    else
        IFS='|' read -r display_name country city category year <<< "$info"
    fi
    
    # Output file
    output_file="$OUTPUT_DIR/${track_name}.yaml"
    
    echo "Converting: $display_name"
    echo "  Track CSV: $track_csv"
    if [ -n "$raceline_arg" ]; then
        echo "  Raceline CSV: $raceline_csv"
    fi
    echo "  Output: $output_file"
    
    # Build the command
    cmd="./target/release/convert_track"
    cmd="$cmd --tracks-csv \"$track_csv\""
    cmd="$cmd $raceline_arg"
    cmd="$cmd --output \"$output_file\""
    cmd="$cmd --name \"$display_name\""
    
    if [ -n "$country" ]; then
        cmd="$cmd --country \"$country\""
    fi
    if [ -n "$city" ]; then
        cmd="$cmd --city \"$city\""
    fi
    if [ -n "$category" ]; then
        cmd="$cmd --category \"$category\""
    fi
    if [ -n "$year" ]; then
        cmd="$cmd --year-built $year"
    fi
    
    # Execute conversion
    if eval $cmd > /dev/null 2>&1; then
        echo "  ✓ Success"
        SUCCESS=$((SUCCESS + 1))
    else
        echo "  ✗ Failed"
        FAILED=$((FAILED + 1))
    fi
    echo ""
done

echo "======================================"
echo "Conversion Summary"
echo "======================================"
echo "Total tracks: $TOTAL"
echo "Successful: $SUCCESS"
echo "Failed: $FAILED"
echo ""
echo "Output directory: $OUTPUT_DIR"
echo "======================================"

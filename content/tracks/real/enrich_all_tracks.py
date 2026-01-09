#!/usr/bin/env python3
"""
Standalone track elevation enrichment script.
Place this file in your tracks/real directory and run: python3 enrich_all_tracks.py
"""

import yaml
import math
import numpy as np
from pathlib import Path
import sys

# =============================================================================
# CONFIGURATION - Elevation and banking data for all circuits
# =============================================================================

TRACK_CONFIG = {
    "Austin": {
        "total_elevation_change": 41.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.05, "elevation": 12.0},
            {"progress": 0.15, "elevation": 20.0},
            {"progress": 0.30, "elevation": 8.0},
            {"progress": 0.40, "elevation": -5.0},
            {"progress": 0.60, "elevation": -8.0},
            {"progress": 0.80, "elevation": 5.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.05, "banking": 4.0, "name": "Turn 1"},
            {"progress": 0.92, "banking": 10.0, "name": "Turn 19-20"}
        ]
    },
    "BrandsHatch": {
        "total_elevation_change": 35.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.08, "elevation": 8.0},
            {"progress": 0.15, "elevation": -10.0},
            {"progress": 0.30, "elevation": 5.0},
            {"progress": 0.50, "elevation": -5.0},
            {"progress": 0.70, "elevation": 10.0},
            {"progress": 0.85, "elevation": 15.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.08, "banking": 3.0, "name": "Paddock Hill Bend"}
        ]
    },
    "Budapest": {
        "total_elevation_change": 7.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.05, "elevation": 3.0},
            {"progress": 0.20, "elevation": 5.0},
            {"progress": 0.50, "elevation": 2.0},
            {"progress": 0.80, "elevation": -2.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Catalunya": {
        "total_elevation_change": 33.5,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.10, "elevation": 8.0},
            {"progress": 0.20, "elevation": 15.0},
            {"progress": 0.35, "elevation": 20.0},
            {"progress": 0.50, "elevation": 12.0},
            {"progress": 0.75, "elevation": -5.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Hockenheim": {
        "total_elevation_change": 12.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.20, "elevation": 3.0},
            {"progress": 0.40, "elevation": 6.0},
            {"progress": 0.60, "elevation": 8.0},
            {"progress": 0.80, "elevation": 4.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.40, "banking": 5.0, "name": "Hairpin"}
        ]
    },
    "IMS": {
        "total_elevation_change": 8.5,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.15, "elevation": 2.0},
            {"progress": 0.40, "elevation": 4.0},
            {"progress": 0.70, "elevation": 2.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.02, "banking": 9.2, "name": "Turn 1 (oval)"},
            {"progress": 0.95, "banking": 9.2, "name": "Turn 4 (oval)"}
        ]
    },
    "Melbourne": {
        "total_elevation_change": 3.5,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.25, "elevation": 1.5},
            {"progress": 0.50, "elevation": 2.5},
            {"progress": 0.75, "elevation": 1.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "MexicoCity": {
        "total_elevation_change": 5.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.30, "elevation": 2.0},
            {"progress": 0.60, "elevation": 3.5},
            {"progress": 0.85, "elevation": 1.5},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.90, "banking": 15.0, "name": "Peraltada"}
        ]
    },
    "Montreal": {
        "total_elevation_change": 4.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.25, "elevation": 1.5},
            {"progress": 0.50, "elevation": 2.5},
            {"progress": 0.75, "elevation": 1.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Monza": {
        "total_elevation_change": 5.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.20, "elevation": 1.5},
            {"progress": 0.40, "elevation": 2.5},
            {"progress": 0.65, "elevation": 3.0},
            {"progress": 0.90, "elevation": 1.5},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.90, "banking": 10.0, "name": "Parabolica"}
        ]
    },
    "MoscowRaceway": {
        "total_elevation_change": 6.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.25, "elevation": 2.0},
            {"progress": 0.50, "elevation": 3.5},
            {"progress": 0.75, "elevation": 2.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Norisring": {
        "total_elevation_change": 3.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.30, "elevation": 1.5},
            {"progress": 0.70, "elevation": 2.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Nuerburgring": {
        "total_elevation_change": 27.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.10, "elevation": 5.0},
            {"progress": 0.25, "elevation": 12.0},
            {"progress": 0.45, "elevation": 5.0},
            {"progress": 0.55, "elevation": -2.0},
            {"progress": 0.75, "elevation": 8.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Oschersleben": {
        "total_elevation_change": 4.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.30, "elevation": 1.5},
            {"progress": 0.60, "elevation": 2.5},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Sakhir": {
        "total_elevation_change": 7.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.20, "elevation": 2.5},
            {"progress": 0.45, "elevation": 4.0},
            {"progress": 0.70, "elevation": 2.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "SaoPaulo": {
        "total_elevation_change": 41.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.10, "elevation": -12.0},
            {"progress": 0.25, "elevation": -18.0},
            {"progress": 0.40, "elevation": -8.0},
            {"progress": 0.60, "elevation": 8.0},
            {"progress": 0.80, "elevation": 15.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Sepang": {
        "total_elevation_change": 5.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.30, "elevation": 2.0},
            {"progress": 0.60, "elevation": 3.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Shanghai": {
        "total_elevation_change": 7.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.08, "elevation": 2.0},
            {"progress": 0.30, "elevation": 4.0},
            {"progress": 0.60, "elevation": 2.5},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Silverstone": {
        "total_elevation_change": 18.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.15, "elevation": 5.0},
            {"progress": 0.30, "elevation": 8.0},
            {"progress": 0.50, "elevation": 3.0},
            {"progress": 0.70, "elevation": -2.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Sochi": {
        "total_elevation_change": 5.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.25, "elevation": 2.0},
            {"progress": 0.50, "elevation": 3.0},
            {"progress": 0.75, "elevation": 1.5},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Spa": {
        "total_elevation_change": 104.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.08, "elevation": 40.0},
            {"progress": 0.15, "elevation": 55.0},
            {"progress": 0.35, "elevation": 25.0},
            {"progress": 0.55, "elevation": -20.0},
            {"progress": 0.70, "elevation": -35.0},
            {"progress": 0.85, "elevation": -15.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.08, "banking": 18.0, "name": "Eau Rouge/Raidillon"}
        ]
    },
    "Spielberg": {
        "total_elevation_change": 65.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.10, "elevation": 15.0},
            {"progress": 0.25, "elevation": 35.0},
            {"progress": 0.45, "elevation": 20.0},
            {"progress": 0.60, "elevation": 5.0},
            {"progress": 0.80, "elevation": 25.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    },
    "Suzuka": {
        "total_elevation_change": 40.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.10, "elevation": 8.0},
            {"progress": 0.25, "elevation": 15.0},
            {"progress": 0.35, "elevation": 5.0},
            {"progress": 0.50, "elevation": -5.0},
            {"progress": 0.60, "elevation": 10.0},
            {"progress": 0.75, "elevation": 18.0},
            {"progress": 0.90, "elevation": 8.0},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": [
            {"progress": 0.75, "banking": 5.0, "name": "130R"}
        ]
    },
    "YasMarina": {
        "total_elevation_change": 4.0,
        "key_points": [
            {"progress": 0.0, "elevation": 0.0},
            {"progress": 0.30, "elevation": 1.5},
            {"progress": 0.60, "elevation": 2.5},
            {"progress": 1.0, "elevation": 0.0}
        ],
        "banked_corners": []
    }
}

# =============================================================================
# PROCESSING FUNCTIONS
# =============================================================================

def calculate_distance(p1, p2):
    """Calculate Euclidean distance between two points."""
    return math.sqrt((p2['x'] - p1['x'])**2 + (p2['y'] - p1['y'])**2)

def calculate_cumulative_distances(nodes):
    """Calculate cumulative distance for each node."""
    distances = [0.0]
    for i in range(1, len(nodes)):
        dist = calculate_distance(nodes[i-1], nodes[i])
        distances.append(distances[-1] + dist)
    return distances

def interpolate_elevation(progress, key_points):
    """Interpolate elevation based on progress through lap and key points."""
    points = key_points
    
    # Find surrounding points
    for i in range(len(points) - 1):
        if points[i]['progress'] <= progress <= points[i+1]['progress']:
            p1, p2 = points[i], points[i+1]
            t = (progress - p1['progress']) / (p2['progress'] - p1['progress'])
            return p1['elevation'] + t * (p2['elevation'] - p1['elevation'])
    
    # Handle wraparound
    if progress > points[-1]['progress']:
        p1, p2 = points[-1], points[0]
        t = (progress - p1['progress']) / (1.0 - p1['progress'])
        return p1['elevation'] + t * (p2['elevation'] - p1['elevation'])
    else:
        p1, p2 = points[-1], points[0]
        t = (progress + 1.0 - p1['progress']) / (1.0 - p1['progress'])
        return p1['elevation'] + t * (p2['elevation'] - p1['elevation'])

def smooth_elevation(elevations, window=5):
    """Apply smoothing to elevation profile."""
    smoothed = np.copy(elevations)
    half_window = window // 2
    
    for i in range(len(elevations)):
        indices = [(i + j) % len(elevations) for j in range(-half_window, half_window + 1)]
        smoothed[i] = np.mean(elevations[indices])
    
    return smoothed

def create_elevation_profile(distances, total_distance, config):
    """Create elevation profile from configuration key points."""
    elevations = np.zeros(len(distances))
    
    for i, d in enumerate(distances):
        progress = d / total_distance if total_distance > 0 else 0.0
        elevations[i] = interpolate_elevation(progress, config['key_points'])
    
    elevations = smooth_elevation(elevations, window=7)
    return elevations

def add_banking(distances, total_distance, banked_corners):
    """Add banking values based on configured banked corners."""
    banking_values = np.zeros(len(distances))
    
    for corner in banked_corners:
        corner_progress = corner['progress']
        corner_banking = corner['banking']
        
        for i, d in enumerate(distances):
            progress = d / total_distance if total_distance > 0 else 0.0
            
            dist_from_corner = min(
                abs(progress - corner_progress),
                abs(progress - corner_progress + 1.0),
                abs(progress - corner_progress - 1.0)
            )
            
            if dist_from_corner < 0.05:
                factor = math.exp(-(dist_from_corner / 0.02) ** 2)
                banking_values[i] = max(banking_values[i], corner_banking * factor)
    
    return banking_values

def update_raceline_elevation(nodes, raceline):
    """Update raceline z-values based on nearest centerline nodes."""
    if not raceline:
        return raceline
    
    try:
        from scipy.spatial import cKDTree
        centerline_points = np.array([[n['x'], n['y']] for n in nodes])
        tree = cKDTree(centerline_points)
        
        for rp in raceline:
            dist, idx = tree.query([rp['x'], rp['y']])
            rp['z'] = float(nodes[idx]['z'])
    except ImportError:
        # Fallback without scipy
        for rp in raceline:
            min_dist = float('inf')
            nearest_idx = 0
            for i, node in enumerate(nodes):
                dist = math.sqrt((rp['x'] - node['x'])**2 + (rp['y'] - node['y'])**2)
                if dist < min_dist:
                    min_dist = dist
                    nearest_idx = i
            rp['z'] = float(nodes[nearest_idx]['z'])
    
    return raceline

def process_track(track_file, output_dir):
    """Process a single track file."""
    track_name = Path(track_file).stem
    
    print(f"\n{'='*60}")
    print(f"Processing: {track_name}")
    print(f"{'='*60}")
    
    if track_name not in TRACK_CONFIG:
        print(f"⚠️  No configuration found for {track_name}, skipping...")
        return None
    
    track_config = TRACK_CONFIG[track_name]
    
    try:
        with open(track_file, 'r') as f:
            track_data = yaml.safe_load(f)
    except Exception as e:
        print(f"❌ Error loading {track_file}: {e}")
        return False
    
    nodes = track_data.get('nodes', [])
    if not nodes:
        print(f"❌ No nodes found in {track_name}")
        return False
    
    print(f"   Nodes: {len(nodes)}")
    
    distances = calculate_cumulative_distances(nodes)
    total_distance = distances[-1]
    print(f"   Track length: {total_distance:.2f}m")
    
    elevations = create_elevation_profile(distances, total_distance, track_config)
    print(f"   Elevation range: {elevations.min():.2f}m to {elevations.max():.2f}m")
    print(f"   Total elevation change: {elevations.max() - elevations.min():.2f}m")
    
    banking_values = add_banking(distances, total_distance, track_config.get('banked_corners', []))
    if banking_values.max() > 0:
        print(f"   Banking range: {banking_values.min():.1f}° to {banking_values.max():.1f}°")
        print(f"   Banked corners: {len(track_config.get('banked_corners', []))}")
    
    for i, node in enumerate(nodes):
        node['z'] = float(round(elevations[i], 3))
        if 'banking' not in node or node.get('banking') == 0:
            node['banking'] = float(round(banking_values[i], 1))
    
    if 'raceline' in track_data and track_data['raceline']:
        print(f"   Updating raceline with {len(track_data['raceline'])} points...")
        track_data['raceline'] = update_raceline_elevation(nodes, track_data['raceline'])
        raceline_elevations = [p['z'] for p in track_data['raceline']]
        print(f"   Raceline elevation range: {min(raceline_elevations):.2f}m to {max(raceline_elevations):.2f}m")
    
    output_path = Path(output_dir) / f"{track_name}.yaml"
    try:
        with open(output_path, 'w') as f:
            yaml.dump(track_data, f, default_flow_style=False, sort_keys=False)
        print(f"   ✅ Saved to: {output_path}")
        return True
    except Exception as e:
        print(f"   ❌ Error saving {output_path}: {e}")
        return False

def main():
    """Main batch processing function."""
    script_dir = Path(__file__).parent
    output_dir = script_dir / "enriched"
    output_dir.mkdir(exist_ok=True)
    
    print("="*60)
    print("TRACK ELEVATION BATCH PROCESSOR")
    print("="*60)
    print(f"Output directory: {output_dir}")
    
    track_files = sorted(script_dir.glob('*.yaml'))
    track_files = [f for f in track_files if f.name != 'enrich_all_tracks.py']
    
    if not track_files:
        print(f"\n❌ No track files found in {script_dir}")
        print("Place this script in your tracks/real directory!")
        sys.exit(1)
    
    print(f"Found {len(track_files)} track files\n")
    
    success_count = 0
    skip_count = 0
    error_count = 0
    
    for track_file in track_files:
        result = process_track(track_file, output_dir)
        if result is True:
            success_count += 1
        elif result is None:
            skip_count += 1
        else:
            error_count += 1
    
    print(f"\n{'='*60}")
    print(f"BATCH PROCESSING COMPLETE")
    print(f"{'='*60}")
    print(f"✅ Successfully processed: {success_count}")
    print(f"⚠️  Skipped (no config): {skip_count}")
    print(f"❌ Errors: {error_count}")
    print(f"📁 Output directory: {output_dir}")
    
    if success_count > 0:
        print(f"\n🎉 {success_count} tracks enriched with elevation data!")
        print(f"\nEnriched files are in: {output_dir}")
        print("Copy them back to replace the originals when ready.")

if __name__ == '__main__':
    main()
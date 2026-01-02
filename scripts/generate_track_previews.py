#!/usr/bin/env python3
"""
Generate track preview images from track YAML files.
Creates a top-down view of each track's centerline.
"""

import os
import yaml
import json
from pathlib import Path
from PIL import Image, ImageDraw

TRACK_DIRS = [
    "content/tracks/real",
    "content/tracks/simple"
]

OUTPUT_DIR = "game-godot/assets/track_previews"
IMAGE_SIZE = (400, 300)  # Width x Height
BACKGROUND_COLOR = (20, 20, 30)  # Dark background
TRACK_COLOR = (80, 200, 120)  # Green track
TRACK_WIDTH = 3

def parse_track_file(filepath):
    """Parse a track file and extract centerline points."""
    _, ext = os.path.splitext(filepath)

    if ext in ['.yaml', '.yml']:
        with open(filepath, 'r') as f:
            data = yaml.safe_load(f)
    elif ext == '.json':
        with open(filepath, 'r') as f:
            data = json.load(f)
    else:
        return None, None

    track_id = data.get('track_id', '')
    name = data.get('name', '')
    nodes = data.get('nodes', [])

    if not nodes:
        return None, None

    # Extract x, y coordinates (ignoring z for top-down view)
    points = []
    for node in nodes:
        x = node.get('x', 0)
        y = node.get('y', 0)
        points.append((x, y))

    return {
        'id': track_id,
        'name': name,
        'points': points
    }, os.path.basename(filepath).replace(ext, '')

def generate_preview_image(track_data, output_path):
    """Generate a preview image for a track."""
    points = track_data['points']

    if len(points) < 2:
        print(f"  ⚠ Not enough points for {track_data['name']}")
        return False

    # Find bounds
    min_x = min(p[0] for p in points)
    max_x = max(p[0] for p in points)
    min_y = min(p[1] for p in points)
    max_y = max(p[1] for p in points)

    # Calculate scale and offset to fit image
    range_x = max_x - min_x
    range_y = max_y - min_y

    if range_x == 0 or range_y == 0:
        print(f"  ⚠ Zero range for {track_data['name']}")
        return False

    # Add padding
    padding = 20
    scale_x = (IMAGE_SIZE[0] - 2 * padding) / range_x
    scale_y = (IMAGE_SIZE[1] - 2 * padding) / range_y
    scale = min(scale_x, scale_y)

    # Center the track
    offset_x = padding + (IMAGE_SIZE[0] - range_x * scale) / 2 - min_x * scale
    offset_y = padding + (IMAGE_SIZE[1] - range_y * scale) / 2 - min_y * scale

    # Transform points to image coordinates
    img_points = []
    for x, y in points:
        img_x = x * scale + offset_x
        # Flip Y axis for image coordinates (Y increases downward in images)
        img_y = IMAGE_SIZE[1] - (y * scale + offset_y)
        img_points.append((img_x, img_y))

    # Create image
    img = Image.new('RGB', IMAGE_SIZE, BACKGROUND_COLOR)
    draw = ImageDraw.Draw(img)

    # Draw track centerline
    if len(img_points) > 1:
        draw.line(img_points, fill=TRACK_COLOR, width=TRACK_WIDTH)

        # Close the loop if it's a circuit
        first = img_points[0]
        last = img_points[-1]
        distance = ((first[0] - last[0])**2 + (first[1] - last[1])**2)**0.5
        if distance < 50:  # If start and end are close, it's a circuit
            draw.line([last, first], fill=TRACK_COLOR, width=TRACK_WIDTH)

    # Save image
    img.save(output_path)
    return True

def main():
    # Get project root (script is in scripts/ folder)
    project_root = Path(__file__).parent.parent

    # Create output directory
    output_dir = project_root / OUTPUT_DIR
    output_dir.mkdir(parents=True, exist_ok=True)

    print("🏁 Generating track preview images...")
    print(f"   Output directory: {output_dir}")

    generated = 0
    skipped = 0

    for track_dir in TRACK_DIRS:
        track_path = project_root / track_dir

        if not track_path.exists():
            print(f"⚠  Directory not found: {track_path}")
            continue

        print(f"\n📂 Processing: {track_dir}")

        # Find all track files
        track_files = list(track_path.glob('*.yaml')) + \
                     list(track_path.glob('*.yml')) + \
                     list(track_path.glob('*.json'))

        for track_file in track_files:
            if track_file.name == 'README.md':
                continue

            try:
                track_data, file_base = parse_track_file(track_file)

                if not track_data or not track_data.get('points'):
                    print(f"  ⏭  Skipping {track_file.name} (no data)")
                    skipped += 1
                    continue

                # Generate output filename
                output_filename = f"{file_base}.png"
                output_path = output_dir / output_filename

                # Generate image
                if generate_preview_image(track_data, output_path):
                    print(f"  ✓ Generated: {output_filename} ({len(track_data['points'])} points)")
                    generated += 1
                else:
                    print(f"  ✗ Failed: {track_file.name}")
                    skipped += 1

            except Exception as e:
                print(f"  ✗ Error processing {track_file.name}: {e}")
                skipped += 1

    print(f"\n✨ Done! Generated {generated} images, skipped {skipped}")
    print(f"   Images saved to: {output_dir.relative_to(project_root)}")

if __name__ == "__main__":
    main()

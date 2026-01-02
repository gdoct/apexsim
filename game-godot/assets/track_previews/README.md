# Track Preview Images

This directory contains auto-generated preview images for all tracks in the game.

## Regenerating Images

If you add new tracks or modify existing track layouts, regenerate the preview images:

```bash
cd /home/guido/apexsim
python3 scripts/generate_track_previews.py
```

The script will:
- Scan `content/tracks/real/` and `content/tracks/simple/` for track YAML/JSON files
- Extract the centerline coordinates from each track
- Generate a 400x300px top-down preview image
- Save images as PNG files in this directory

## Image Format

- **Size**: 400x300 pixels
- **Format**: PNG
- **Background**: Dark (RGB 20, 20, 30)
- **Track line**: Green (RGB 80, 200, 120), 3px width
- **Naming**: Matches the track filename (e.g., `Monza.yaml` → `Monza.png`)

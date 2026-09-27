# Custom tracks

Put your own circuits here: ones you have imported or built yourself.
Everything in this folder except this README is gitignored, and a release
build does not ship it unless you pass `-IncludeCustomTracks` to
`build_release.ps1` or `build_game_standalone.ps1`. An import may be derived
from another game's content that you may use but must not redistribute.

A custom track is laid out exactly like one in `../default/`: the files sit
side by side in this folder, named after the track's stem.

| File | What it is |
|---|---|
| `<Stem>.yaml` | The centerline and track data (`docs/TRACK_FILE_FORMAT.md`). It needs its own fixed `track_id`. |
| `<Stem>.ats` | The scene: surfaces, curbs, props, pit lane (`scripts/seed_scene.py` starts one). |
| `<Stem>.layout.json` | Optional layout dossier for `ats-dress`. |
| `<Stem>.{ground,curbs,walls,road}.msgpack` | The server's sidecars, written by `ats-export`. An importer that writes its own lists them in the `.ats` as `"external_sidecars"`, and the export leaves those alone. |

Two rules:

- **The stem must be unique across `default/` and `custom/`.** Exports and
  previews are keyed by the stem alone, so `ats-export --all` refuses a stem
  it finds in both folders.
- **The `track_id` must be unique too.** If a custom track reuses a shipped
  track's id, the server keeps the shipped one and logs a warning.

After adding or changing a track:

```powershell
./scripts/build_track_levels.ps1 -Track <Stem> -SkipMaterials   # dress, export, preview
```

Then restart the server, and restart the game or run `apexsim.track.Rescan`.

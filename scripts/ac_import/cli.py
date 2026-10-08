"""`python scripts/ac_import.py <ac-track-folder> [options]`: one command per
AC track layout, writing the server's files into `content/tracks/custom/`
and the client's export into `build/tracks/`, then checking the result.
See docs/AC_TRACK_IMPORT.md.
"""

from __future__ import annotations

import argparse
import io
import json
import re
import shutil
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import TOOL_VERSION, ai, centerline, ini, kn5, physics, scene, sidecars, textures, validate
from .export import source_crc, ue_location, ue_yaw_deg, write_export
from .sidecars import CONTACT_NAMES

SCRIPTS = Path(__file__).resolve().parent.parent
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
from track_dirs import CUSTOM_DIR, DEFAULT_DIR  # noqa: E402

EXPORT_DIR = REPO / "build" / "tracks"
NOTICE_MARKER = REPO / ".cache" / "ac_import" / "notice_shown"
PIT_LANE_WIDTH_M = 12.0
PIT_SPEED_LIMIT_KMH = 80.0

NOTICE = """\
ac_import converts a track from your own Assetto Corsa install for your own
use on this machine. The result is written to content/tracks/custom and
build/tracks, which ApexSim never packages or sends anywhere. It stays
yours: do not redistribute it, and note that some mod authors' terms forbid
converting their work to other games. Encrypted tracks are refused.
"""


class ImportError_(Exception):
    """A track that cannot be imported, with the reason."""


@dataclass
class Options:
    layout: str | None = None
    stem: str | None = None
    display_name: str | None = None
    textures: str = "kit"
    max_texture: int = 2048
    force: bool = False
    dry_run: bool = False
    out_dir: Path = EXPORT_DIR
    custom_dir: Path = CUSTOM_DIR
    default_dir: Path = DEFAULT_DIR  # shipped circuits: a stem here is refused


@dataclass
class Result:
    stem: str
    layout: str
    ok: bool
    summary: str
    checks: list[dict] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def camel(text: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", text)
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


def default_stem(layout: ini.Layout) -> str:
    stem = camel(layout.track_dir.name)
    if layout.name:
        stem += "_" + camel(layout.name)
    return stem or "AcTrack"


def show_notice_once() -> None:
    if NOTICE_MARKER.exists():
        return
    print(NOTICE)
    try:
        NOTICE_MARKER.parent.mkdir(parents=True, exist_ok=True)
        NOTICE_MARKER.write_text("shown\n", encoding="utf-8")
    except OSError:
        pass


def list_layouts(track_dir: Path) -> None:
    layouts = ini.find_layouts(track_dir)
    if not layouts:
        print(f"{track_dir}: no models.ini, not an AC track folder")
        return
    for lay in layouts:
        ui = ini.read_ui_track(lay.ui_dir / "ui_track.json")
        has_ai = (lay.ai_dir / "fast_lane.ai").is_file()
        print(f"  {lay.name or '(single layout)':<24} {ui.name or '?':<32} stem {default_stem(lay):<28}"
              f" {'fast_lane.ai' if has_ai else 'NO fast_lane.ai'}")


def import_layout(layout: ini.Layout, opts: Options, system_surfaces: Path | None) -> Result:
    t_start = time.time()
    warnings: list[str] = []
    stem = opts.stem or default_stem(layout)
    if not re.match(r"^[A-Za-z][A-Za-z0-9_\-]*$", stem):
        raise ImportError_(f"stem {stem!r} must be letters, digits, _ or -")
    ui = ini.read_ui_track(layout.ui_dir / "ui_track.json")
    name = ui.name or layout.track_dir.name
    if layout.name and ui.name and layout.name.lower() not in ui.name.lower():
        name = f"{ui.name} ({layout.name})"
    display_name = opts.display_name or name
    track_id = centerline.track_id_for(layout.track_dir.name, layout.name)

    # Where it goes, and whether it may.
    if (opts.default_dir / f"{stem}.yaml").exists():
        raise ImportError_(f"stem {stem!r} is a shipped circuit; pick another with --stem")
    yaml_path = opts.custom_dir / f"{stem}.yaml"
    if yaml_path.exists() and not opts.force:
        existing = _existing_import(opts.custom_dir / f"{stem}.import.json")
        if existing and existing != layout.label:
            raise ImportError_(f"{yaml_path.name} exists and came from {existing}; pick another --stem or --force")
        if not opts.force:
            raise ImportError_(f"{yaml_path.name} exists; --force replaces it")

    # Read.
    files_read: dict[str, int] = {}
    model_names = ini.model_files(layout)
    if not model_names:
        raise ImportError_(f"{layout.models_ini.name} lists no kn5 files")
    kn5s: list[kn5.Kn5File] = []
    for fn in model_names:
        path = layout.track_dir / fn
        if not path.is_file():
            warnings.append(f"{fn} is listed in {layout.models_ini.name} but missing")
            continue
        try:
            kn = kn5.read_kn5(path, textures=True)
        except kn5.EncryptedKn5 as e:
            raise ImportError_(f"refused: {e}") from e
        except kn5.Kn5Error as e:
            raise ImportError_(f"{fn}: {e}") from e
        kn5s.append(kn)
        files_read[fn] = source_crc_of(path)
    if not kn5s:
        raise ImportError_("none of the layout's kn5 files could be read")

    fast_lane_path = layout.ai_dir / "fast_lane.ai"
    if not fast_lane_path.is_file():
        raise ImportError_("no ai/fast_lane.ai: a centerline cannot be derived yet (out of scope)")
    try:
        fast_lane = ai.read_ai(fast_lane_path)
    except ai.AiError as e:
        raise ImportError_(str(e)) from e
    files_read[str(fast_lane_path.relative_to(layout.track_dir)).replace("\\", "/")] = source_crc_of(fast_lane_path)
    if not fast_lane.is_closed():
        raise ImportError_("the AI line does not close: point-to-point stages are out of scope")
    if fast_lane.count < 50:
        raise ImportError_(f"the AI line has only {fast_lane.count} points")

    surfaces_ini = layout.find_data("surfaces.ini")
    surface_paths = [p for p in (surfaces_ini, system_surfaces) if p]
    surfaces = ini.read_surfaces(surface_paths)
    for key, s in ini.SYSTEM_SURFACES.items():
        surfaces.setdefault(key, s)
    if surfaces_ini:
        files_read[str(surfaces_ini.relative_to(layout.track_dir)).replace("\\", "/")] = source_crc_of(surfaces_ini)
    else:
        warnings.append("no data/surfaces.ini; AC's system defaults were used")
    drs_ini = layout.find_data("drs_zones.ini")
    drs = ini.read_drs_zones(drs_ini)
    if drs_ini:
        files_read[str(drs_ini.relative_to(layout.track_dir)).replace("\\", "/")] = source_crc_of(drs_ini)

    # Markers.
    markers = _markers(kn5s)
    time0 = markers.get("AC_TIME_0")
    frame, _origin = centerline.make_frame(fast_lane.positions, True, time0, warnings)

    # Physics. The markers float above the road, so the origin's height is
    # taken from the physics mesh under the start line and the frame rebuilt
    # once with it: node 0 then sits at z = 0 like every shipped circuit's.
    world = physics.collect_physics(kn5s, surfaces, frame, [])
    if world.index.count == 0:
        raise ImportError_("no physics surfaces were found (no NNKEY meshes matching surfaces.ini)")
    z0, c0, _ = world.contacts_at(np.zeros(1), np.zeros(1), np.array([5.0]))
    if np.isfinite(z0[0]):
        frame = frame.with_origin_z(frame.origin[2] + float(z0[0]))
        world = physics.collect_physics(kn5s, surfaces, frame, warnings)
    else:
        warnings.append("no physics surface under the start line; heights are relative to the marker")
        world = physics.collect_physics(kn5s, surfaces, frame, warnings)
    spine = centerline.build_spine(fast_lane.positions, frame, True)
    measured = centerline.measure(spine, world, fast_lane.side_left, fast_lane.side_right, warnings)

    # Race data.
    sectors = []
    for n in (1, 2):
        pair = markers.get(f"AC_TIME_{n}")
        if pair is None:
            continue
        mid = frame.ac_points((pair[0] + pair[1]) / 2.0)
        i = centerline.nearest_station_index(spine, mid)
        sectors.append(int(round(i / int(centerline.NODE_STEP_M))))
    if len(sectors) == 1:
        # AC allows a two-sector track; ApexSim wants three. The second
        # boundary goes halfway between the first and the finish.
        node_count = spine.positions.shape[0] // int(centerline.NODE_STEP_M)
        sectors.append(int((sectors[0] + node_count) // 2))
        warnings.append("one sector line (AC_TIME_1) only; a second boundary was added halfway to the finish")
    elif len(sectors) != 2:
        warnings.append("no sector lines (AC_TIME_1/2); the lap is split into even thirds")
        sectors = []
    sectors = sorted(set(sectors))
    if len(sectors) != 2:
        sectors = []
    starts = sorted(((k, v) for k, v in markers.items() if k.startswith("AC_START_")),
                    key=lambda kv: int(kv[0].rsplit("_", 1)[1]))
    spawn_xy: list[tuple[float, float]] = []
    grid_entries: list[dict] = []
    for pos, (mname, ac_pos) in enumerate(starts, start=1):
        p = frame.ac_points(ac_pos)
        i = centerline.nearest_station_index(spine, p)
        yaw = float(np.arctan2(spine.tangents[i, 1], spine.tangents[i, 0]))
        z, _, _ = world.contacts_at(np.array([p[0]]), np.array([p[1]]), np.array([p[2] + 5.0]))
        zz = float(z[0]) if np.isfinite(z[0]) else float(measured.centre[i, 2])
        # The YAML's spawn offsets are measured from node 0, which is the
        # road's measured centre at the start line: within a sampling step
        # of the origin, but not the origin itself.
        spawn_xy.append((float(p[0] - measured.centre[0, 0]), float(p[1] - measured.centre[0, 1])))
        grid_entries.append({"position": pos, "location": ue_location((p[0], p[1], zz)), "yaw_deg": ue_yaw_deg(yaw)})
    if not starts:
        warnings.append("no AC_START markers; the server lays a generated grid")
    pit_count = sum(1 for k in markers if k.startswith("AC_PIT_"))
    drs_zones: list[tuple[float, float, float]] = []
    for z in drs:
        conv = lambda f: (f * spine.lap_m - spine.ai_start_station) % spine.lap_m  # noqa: E731
        drs_zones.append((conv(z.detection), conv(z.start), conv(z.end)))

    raceline = spine.positions[:: int(centerline.NODE_STEP_M)].copy()
    rz, _, _ = world.contacts_at(raceline[:, 0], raceline[:, 1], raceline[:, 2] + 1.5)
    raceline[:, 2] = np.where(np.isfinite(rz), rz, raceline[:, 2] - centerline.AI_HEIGHT_FALLBACK_M)

    metadata = {
        "country": ui.country or None,
        "city": ui.city or None,
        "length_m": round(float(spine.lap_m), 1),
        "description": f"Imported from Assetto Corsa: {name} ({layout.label}).",
        "year_built": None,
        "category": "Imported",
        "environment_type": "plains",
    }
    doc = centerline.track_yaml(spine, measured, raceline, name=name, display_name=display_name, track_id=track_id,
                                sectors=sectors, spawn_xy=spawn_xy, drs_zones=drs_zones, metadata=metadata)
    yaml_text = centerline.dump_yaml(doc)
    yaml_bytes = yaml_text.encode("utf-8")
    crc = source_crc(yaml_bytes)

    # The scene.
    scene_report = scene.SceneReport()
    scene_meshes, materials, needed = scene.collect_scene(kn5s, world, frame, opts.textures, scene_report)
    warnings.extend(scene_report.warnings[:20])
    if len(scene_report.warnings) > 20:
        warnings.append(f"... and {len(scene_report.warnings) - 20} more scene warnings")
    merged = scene.merge_meshes(scene_meshes)

    # Textures.
    texture_dir_name = f"{stem}.textures"
    texture_files: dict[str, tuple[str, textures.ConvertedTexture]] = {}
    texture_bytes = 0
    if opts.textures != "flat":
        for tex_name in sorted(needed):
            kn, _ = needed[tex_name]
            try:
                conv = textures.convert_texture(kn.textures[tex_name].data, opts.max_texture)
            except Exception as e:  # noqa: BLE001 - a broken texture must not sink the import
                warnings.append(f"texture {tex_name!r} could not be converted ({e}); its materials draw flat")
                continue
            file_name = _unique_file_name(tex_name, texture_files)
            texture_files[tex_name] = (file_name, conv)
            texture_bytes += textures.texture_bytes(conv)
    # Each scenery material either points at its converted texture or, in
    # the flat mode or when the texture failed, carries its average colour.
    averages: dict[str, tuple[float, float, float]] = {}
    for spec in materials.values():
        if spec.family != "scenery":
            continue
        tex_name = spec.texture
        spec.texture = None
        if not tex_name:
            spec.base_color = [0.5, 0.5, 0.5, 1.0]
            continue
        entry = texture_files.get(tex_name)
        if entry and opts.textures != "flat":
            spec.texture = f"{texture_dir_name}/{entry[0]}"
            continue
        if entry:
            avg = entry[1].average_linear
        else:
            avg = averages.get(tex_name)
            if avg is None and tex_name in needed:
                kn, _ = needed[tex_name]
                try:
                    avg = textures.average_linear(textures.decode_image(kn.textures[tex_name].data))
                except Exception:  # noqa: BLE001
                    avg = (0.5, 0.5, 0.5)
                averages[tex_name] = avg
        avg = avg or (0.5, 0.5, 0.5)
        spec.base_color = [round(avg[0], 4), round(avg[1], 4), round(avg[2], 4), 1.0]

    # Preview.
    preview_source, preview_how = _preview_source(layout)

    # Validation.
    slot_xy = [(x + float(measured.centre[0, 0]), y + float(measured.centre[0, 1])) for x, y in spawn_xy]
    checks = [
        validate.check_grid(world, slot_xy, measured),
        validate.check_coverage(world, spine, measured),
        validate.check_centerline(measured, spine),
        validate.check_walls(world, spine, measured),
        validate.check_budget(sum(m.triangles.shape[0] for m in merged), len(merged), texture_bytes),
    ]
    failed = [c for c in checks if c.status == "fail"]

    # Ground and curbs.
    gx, gy, heights = physics.ground_heightfield(world)
    if np.abs(heights).max() > 320.0:
        warnings.append("the ground rises or falls more than 320 m from the start line; the ground sidecar clamps there")
    step1 = spine.step_m
    ratio = 1.0 / step1
    samples = np.arange(0, int(round(spine.lap_m)))
    pick = np.clip(np.rint(samples * ratio).astype(np.int64), 0, spine.positions.shape[0] - 1)

    material_rows = sorted((s for s in materials.values()), key=lambda s: s.key)
    report = {
        "tool": TOOL_VERSION,
        "source": str(layout.track_dir),
        "layout": layout.name,
        "stem": stem,
        "track_id": track_id,
        "name": name,
        "display_name": display_name,
        "options": {"textures": opts.textures, "max_texture": opts.max_texture},
        "rebuild": _rebuild_command(layout, opts, stem),
        "files": files_read,
        "frame": {"origin_world": [round(float(v), 3) for v in frame.origin], "heading_rad": round(frame.heading, 6)},
        "lap_m": round(float(spine.lap_m), 2),
        "physics": {
            "meshes": world.physics_meshes, "wall_meshes": world.wall_meshes,
            "triangles": int(world.triangles.shape[0]), "wall_segments": int(world.wall_segments.shape[0]),
            "road_friction": world.road_friction,
            "surfaces": [{"key": s.key, "contact": CONTACT_NAMES[s.contact], "ac_friction": s.ac_friction,
                          "friction_multiplier": s.friction, "valid_track": s.valid_track, "pit_lane": s.pit_lane,
                          "triangles": s.triangles} for s in world.surfaces],
            "unknown_keys": world.unknown_keys,
        },
        "scene": {
            "meshes_kept": scene_report.kept, "dropped_lower_lods": scene_report.dropped_lod,
            "dropped_logic_objects": scene_report.dropped_logic, "dropped_overlays": scene_report.dropped_overlay,
            "dropped_hidden": scene_report.dropped_hidden, "triangles": scene_report.kept_triangles,
            "kit_triangles": scene_report.kit_triangles, "scenery_triangles": scene_report.scenery_triangles,
            "draw_calls": len(merged), "textures": len(texture_files), "texture_bytes": texture_bytes,
        },
        "materials": [
            {"key": s.key, "family": s.family, "ac_material": s.ac_material, "shader": s.shader,
             "ground_set": s.ground_set, "texture": s.texture, "blend": s.blend, "meshes": s.meshes,
             "triangles": s.triangles}
            for s in material_rows
        ],
        "textures": {tex: {"file": f, "how": c.how, "size": [c.width, c.height], "mips": c.mips,
                           "format": c.fourcc.decode("ascii")} for tex, (f, c) in sorted(texture_files.items())},
        "preview": preview_how,
        "grid_slots": len(spawn_xy), "pit_boxes": pit_count, "sectors": sectors,
        "drs_zones": len(drs_zones),
        "checks": [c.row() for c in checks],
        "warnings": warnings,
    }

    summary = (f"{stem}: {spine.lap_m:.0f} m, {world.triangles.shape[0]} physics tris, "
               f"{scene_report.kept_triangles} drawn tris in {len(merged)} meshes, {len(texture_files)} textures "
               f"({texture_bytes / 1e6:.0f} MB), {len(spawn_xy)} grid slots; "
               + ", ".join(f"{c.name} {c.status}" for c in checks))
    if opts.dry_run:
        _print_report_summary(report)
        print(f"  ({time.time() - t_start:.1f} s)")
        return Result(stem, layout.name, not failed, "(dry run) " + summary, [c.row() for c in checks], warnings)

    # Write: server first, then the client, then the report.
    custom = opts.custom_dir
    custom.mkdir(parents=True, exist_ok=True)
    _write_text(yaml_path, yaml_text)
    ats = {
        "format": "apex-track-scene",
        "version": 2,
        "source_track": yaml_path.name,
        "track_name": name,
        "imported": "ac",
        "surfaces": [],
        "curbs": [],
        "markings": [],
        "pit_lane": None,
        "props": [],
        "external_sidecars": ["ground", "curbs", "walls", "road"],
        "next_id": 1,
    }
    _write_text(custom / f"{stem}.ats", json.dumps(ats, indent=2) + "\n")
    sidecars.write_msgpack(custom / f"{stem}.road.msgpack", sidecars.road_mesh_payload(
        world.vertices, world.triangles, world.triangle_surface,
        [{"key": s.key, "contact": s.contact, "friction": s.friction, "valid_track": s.valid_track,
          "pit_lane": s.pit_lane} for s in world.surfaces],
        f"ac-import {layout.label}"))
    sidecars.write_msgpack(custom / f"{stem}.walls.msgpack", sidecars.walls_payload(world.wall_segments, world.wall_kinds))
    sidecars.write_msgpack(custom / f"{stem}.ground.msgpack", sidecars.ground_payload(gx, gy, 4.0, heights))
    sidecars.write_msgpack(custom / f"{stem}.curbs.msgpack", sidecars.curbs_payload(
        1.0, measured.curb_left[pick], measured.curb_right[pick], measured.runoff_left[pick], measured.runoff_right[pick]))

    out_dir = opts.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    tex_dir = out_dir / texture_dir_name
    if tex_dir.exists():
        shutil.rmtree(tex_dir)
    if texture_files:
        tex_dir.mkdir(parents=True)
        for _tex, (file_name, conv) in sorted(texture_files.items()):
            (tex_dir / file_name).write_bytes(conv.data)
    per_node = int(centerline.NODE_STEP_M)
    center_entries = []
    for i in range(0, spine.positions.shape[0], per_node):
        yaw = float(np.arctan2(spine.tangents[i, 1], spine.tangents[i, 0]))
        center_entries.append({
            "s_cm": round(float(spine.stations[i]) * 100.0, 1),
            "location": ue_location(measured.centre[i]),
            "yaw_deg": ue_yaw_deg(yaw),
            "half_left_cm": round(float(measured.width_left[i]) * 100.0, 1),
            "half_right_cm": round(float(measured.width_right[i]) * 100.0, 1),
        })
    start_finish = {"location": ue_location(measured.centre[0]), "yaw_deg": ue_yaw_deg(0.0),
                    "width_m": round(float(measured.width_left[0] + measured.width_right[0]), 3)}
    pit_lane = {"width_cm": PIT_LANE_WIDTH_M * 100.0, "box_count": int(pit_count or ui.pitboxes or 0),
                "speed_limit_kmh": PIT_SPEED_LIMIT_KMH} if pit_count else None
    manifest_metadata = {"country": ui.country or None, "city": ui.city or None, "category": "Imported",
                         "environment_type": "plains", "description": metadata["description"]}
    write_export(out_dir, stem, track_id=track_id, track_name=name, display_name=display_name,
                 source_track=yaml_path.name, source_crc_value=crc, closed_loop=True, length_m=spine.lap_m,
                 metadata=manifest_metadata, materials=[s.manifest_entry() for s in material_rows],
                 meshes=merged, grid=grid_entries, centerline=center_entries, pit_lane=pit_lane,
                 start_finish=start_finish)
    _write_preview(preview_source, out_dir / "previews" / f"{stem}.png", doc)
    # The report carries no clock, so a re-run writes the same bytes.
    _write_text(custom / f"{stem}.import.json", json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    _print_report_summary(report)
    print(f"  ({time.time() - t_start:.1f} s)")
    return Result(stem, layout.name, not failed, summary, [c.row() for c in checks], warnings)


def _print_report_summary(report: dict) -> None:
    for c in report["checks"]:
        print(f"  [{c['status']:>4}] {c['name']}: {c['detail']}")
    for w in report["warnings"]:
        print(f"  warning: {w}")


def _markers(kn5s: list[kn5.Kn5File]) -> dict:
    """`AC_START_n` / `AC_PIT_n` positions and `AC_TIME_n` (left, right)
    pairs, in AC's frame, from the dummies (or a marker mesh's centroid)."""
    found: dict[str, np.ndarray] = {}
    for kn in kn5s:
        for d in kn.dummies:
            if d.name.startswith(("AC_START_", "AC_PIT_", "AC_TIME_", "AC_HOTLAP_START_")):
                found.setdefault(d.name, d.position)
        for m in kn.meshes:
            if m.name.startswith(("AC_START_", "AC_PIT_", "AC_TIME_")) and m.name not in found and m.vertices.shape[0]:
                found[m.name] = m.world_positions().mean(axis=0)
    out: dict = {}
    for name, pos in found.items():
        if name.startswith("AC_TIME_"):
            m = re.match(r"^AC_TIME_(\d+)_([LR])$", name)
            if not m:
                continue
            key = f"AC_TIME_{m.group(1)}"
            pair = out.setdefault(key, [None, None])
            pair[0 if m.group(2) == "L" else 1] = pos
        else:
            out[name] = pos
    for key in [k for k in out if k.startswith("AC_TIME_")]:
        if out[key][0] is None or out[key][1] is None:
            del out[key]
        else:
            out[key] = (out[key][0], out[key][1])
    return out


def _existing_import(report_path: Path) -> str | None:
    try:
        data = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    src = data.get("source")
    layout = data.get("layout") or ""
    if not src:
        return None
    label = Path(src).name
    return f"{label}/{layout}" if layout else label


def _rebuild_command(layout: ini.Layout, opts: Options, stem: str) -> str:
    parts = ["python", "scripts/ac_import.py", f'"{layout.track_dir}"']
    if layout.name:
        parts += ["--layout", layout.name]
    elif opts.layout == ".":
        parts += ["--layout", "."]
    if opts.stem:
        parts += ["--stem", stem]
    if opts.display_name:
        parts += ["--display-name", f'"{opts.display_name}"']
    if opts.textures != "kit":
        parts += ["--textures", opts.textures]
    if opts.max_texture != 2048:
        parts += ["--max-texture", str(opts.max_texture)]
    parts.append("--force")
    return " ".join(parts)


def _unique_file_name(tex_name: str, existing: dict) -> str:
    base = re.sub(r"[^A-Za-z0-9_.\-]+", "_", Path(tex_name).stem).strip("_") or "texture"
    taken = {v[0] for v in existing.values()}
    name = f"{base}.dds"
    n = 2
    while name in taken:
        name = f"{base}_{n}.dds"
        n += 1
    return name


def _preview_source(layout: ini.Layout) -> tuple[Path | None, str]:
    for candidate, how in ((layout.ui_dir / "preview.png", "ui preview.png"),
                           (layout.ui_dir / "outline.png", "ui outline.png")):
        if candidate.is_file():
            return candidate, how
    return None, "drawn from the centerline"


def _write_preview(source: Path | None, out: Path, doc: dict) -> None:
    from PIL import Image
    out.parent.mkdir(parents=True, exist_ok=True)
    if source is not None:
        try:
            im = Image.open(source)
            im.load()
            im = im.convert("RGB")
            if im.width > 800:
                im = im.resize((800, max(1, int(im.height * 800 / im.width))), Image.LANCZOS)
            im.save(out, format="PNG", optimize=True)
            return
        except Exception:  # noqa: BLE001 - fall through to the drawn outline
            pass
    from generate_track_previews import generate_preview_image  # noqa: E402
    track = {"id": doc["track_id"], "name": doc["name"], "points": [(n["x"], n["y"]) for n in doc["nodes"]]}
    generate_preview_image(track, out)


def _write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(text.encode("utf-8"))
    tmp.replace(path)


def source_crc_of(path: Path) -> int:
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 22)
            if not chunk:
                break
            crc = zlib.crc32(chunk.replace(b"\r", b""), crc)
    return crc & 0xFFFFFFFF


def find_system_surfaces(track_dir: Path) -> Path | None:
    """`<ac>/system/data/surfaces.ini` for a track inside an AC install."""
    for parent in Path(track_dir).resolve().parents:
        candidate = parent / "system" / "data" / "surfaces.ini"
        if candidate.is_file():
            return candidate
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ac_import.py", description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("track", nargs="?", help="an AC track folder (content/tracks/<name>)")
    ap.add_argument("--all", metavar="DIR", help="import every importable layout under an AC content/tracks folder")
    ap.add_argument("--layout", help="which layout of a multi-layout track (default: every layout; "
                                     "`.` is the folder's own models.ini layout)")
    ap.add_argument("--list", action="store_true", help="list the layouts and exit")
    ap.add_argument("--stem", help="output stem (default: folder + layout, e.g. RtSuzuka_Gp)")
    ap.add_argument("--display-name", help="the name the game shows (default: from ui_track.json)")
    ap.add_argument("--textures", choices=("kit", "ac", "flat"), default="kit",
                    help="kit: ApexSim's ground sets for the ground, AC's textures for the rest (default); "
                         "ac: AC's textures everywhere; flat: average colours, no textures")
    ap.add_argument("--max-texture", type=int, default=2048, help="downscale textures larger than N pixels (default 2048)")
    ap.add_argument("--force", action="store_true", help="replace an existing import of this stem")
    ap.add_argument("--dry-run", action="store_true", help="read and report; write nothing")
    ap.add_argument("--out", type=Path, default=EXPORT_DIR, help=argparse.SUPPRESS)
    ap.add_argument("--custom-dir", type=Path, default=CUSTOM_DIR, help=argparse.SUPPRESS)
    ap.add_argument("--default-dir", type=Path, default=DEFAULT_DIR, help=argparse.SUPPRESS)
    ap.add_argument("--root", type=Path, help="an ApexSim install (the folder holding Game and Server): the client's files "
                                              "go to Game/Tracks, the server's to Server/content/tracks/custom")
    ap.add_argument("--no-notice", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if args.root:
        args.out = args.root / "Game" / "Tracks"
        args.custom_dir = args.root / "Server" / "content" / "tracks" / "custom"
        args.default_dir = args.root / "Server" / "content" / "tracks" / "default"

    if not args.track and not args.all:
        ap.error("give an AC track folder, or --all <folder>")
    if args.all and (args.stem or args.layout or args.display_name):
        ap.error("--all takes no --stem, --layout or --display-name")

    opts = Options(layout=args.layout, stem=args.stem, display_name=args.display_name, textures=args.textures,
                   max_texture=max(64, args.max_texture), force=args.force, dry_run=args.dry_run,
                   out_dir=args.out, custom_dir=args.custom_dir, default_dir=args.default_dir)

    if args.list:
        list_layouts(Path(args.track or args.all))
        return 0
    if not args.no_notice and not args.dry_run:
        show_notice_once()

    if args.all:
        root = Path(args.all)
        if not root.is_dir():
            print(f"{root}: not a folder", file=sys.stderr)
            return 2
        folders = sorted(p for p in root.iterdir() if p.is_dir() and ini.find_layouts(p))
    else:
        folders = [Path(args.track)]

    failures = 0
    total = 0
    for folder in folders:
        layouts = ini.find_layouts(folder)
        if not layouts:
            print(f"{folder}: no models.ini, not an AC track folder", file=sys.stderr)
            failures += 1
            continue
        if opts.layout:
            # `.` is the folder's own layout (models.ini), beside named ones.
            want = "" if opts.layout == "." else opts.layout
            layouts = [lay for lay in layouts if lay.name == want]
            if not layouts:
                print(f"{folder}: no layout {opts.layout!r} (see --list)", file=sys.stderr)
                failures += 1
                continue
        if opts.stem and len(layouts) > 1:
            print(f"{folder}: --stem with {len(layouts)} layouts; pick one with --layout", file=sys.stderr)
            return 2
        system_surfaces = find_system_surfaces(folder)
        for lay in layouts:
            total += 1
            try:
                result = import_layout(lay, opts, system_surfaces)
            except ImportError_ as e:
                print(f"{lay.label}: skipped: {e}")
                failures += 1
                continue
            print(result.summary)
            if not result.ok:
                failures += 1
    if total > 1:
        print(f"imported {total - failures} of {total} layout(s)")
    return 1 if failures else 0

"""The client export, format version 3: the `<Stem>.uescene.json` manifest
and the `<Stem>.uemesh` blob `ats-export` writes for a generated circuit
(`track-editor/TRACK_EDITOR.md` section 5), plus what version 3 adds for an
imported one:

- a material may carry `texture` (a DDS path relative to the manifest),
  `blend` (`opaque` / `masked` / `translucent`), `two_sided`, `roughness`
  and `alpha_cutoff` (family `scenery`), or a `ground_set` naming which of
  the kit's texture sets a `surface` family key samples;
- a mesh header may carry `draw_distance_m` and `collision: false`.

Positions go out in Unreal's frame the same way the exporter's do: track
`(x, y, z)` m -> `(x*100, -y*100, z*100)` cm, normals likewise, indices as
they are (the mirror flips the handedness for free).
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from pathlib import Path

import numpy as np

from .scene import MergedMesh

FORMAT = "apex-ue-scene"
VERSION = 3
MESH_BLOB_MAGIC = b"APEXMESH"
MESH_BLOB_VERSION = 1
MESH_FLAG_ZLIB = 1


def source_crc(data: bytes) -> int:
    """CRC-32 with every carriage return dropped (`content_crc.rs`)."""
    return zlib.crc32(data.replace(b"\r", b"")) & 0xFFFFFFFF


def to_ue_positions(p: np.ndarray) -> np.ndarray:
    out = np.empty_like(p, dtype=np.float32)
    out[:, 0] = p[:, 0] * 100.0
    out[:, 1] = -p[:, 1] * 100.0
    out[:, 2] = p[:, 2] * 100.0
    return out


def to_ue_normals(n: np.ndarray) -> np.ndarray:
    out = np.empty_like(n, dtype=np.float32)
    out[:, 0] = n[:, 0]
    out[:, 1] = -n[:, 1]
    out[:, 2] = n[:, 2]
    return out


def ue_location(p) -> list[float]:
    return [round(float(p[0]) * 100.0, 1), round(-float(p[1]) * 100.0, 1), round(float(p[2]) * 100.0, 1)]


def ue_yaw_deg(track_yaw_rad: float) -> float:
    return round(-float(np.degrees(track_yaw_rad)), 3)


def encode_blob(meshes: list[MergedMesh]) -> tuple[bytes, list[dict]]:
    """The blob and the manifest's mesh headers, in one order."""
    out = bytearray()
    out += MESH_BLOB_MAGIC
    out += struct.pack("<II", MESH_BLOB_VERSION, len(meshes))
    headers = []
    for m in meshes:
        pos = to_ue_positions(np.asarray(m.positions, dtype=np.float64))
        nrm = to_ue_normals(np.asarray(m.normals, dtype=np.float64))
        uv = np.asarray(m.uvs, dtype=np.float32)
        idx = np.asarray(m.triangles, dtype=np.uint32).reshape(-1)
        v = pos.shape[0]
        raw = pos.astype("<f4").tobytes() + nrm.astype("<f4").tobytes() + uv.astype("<f4").tobytes() + idx.astype("<u4").tobytes()
        packed = zlib.compress(raw, 6)
        flags, payload = (MESH_FLAG_ZLIB, packed) if len(packed) < len(raw) else (0, raw)
        name = m.name.encode("utf-8")
        key = m.key.encode("utf-8")
        out += struct.pack("<I", len(name)) + name
        out += struct.pack("<I", len(key)) + key
        out += struct.pack("<IIII", v, idx.shape[0], flags, len(payload))
        out += payload
        header = {"name": m.name, "material_key": m.key, "vertex_count": int(v), "index_count": int(idx.shape[0])}
        if m.draw_distance_m:
            header["draw_distance_m"] = round(float(m.draw_distance_m), 1)
        if not m.collision:
            header["collision"] = False
        headers.append(header)
    return bytes(out), headers


def write_export(out_dir: Path, stem: str, *, track_id: str, track_name: str, display_name: str,
                 source_track: str, source_crc_value: int, closed_loop: bool, length_m: float,
                 metadata: dict, materials: list[dict], meshes: list[MergedMesh], grid: list[dict],
                 centerline: list[dict], pit_lane: dict | None, start_finish: dict | None) -> tuple[Path, Path]:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    blob, headers = encode_blob(meshes)
    blob_name = f"{stem}.uemesh"
    manifest = {
        "format": FORMAT,
        "version": VERSION,
        "track_id": track_id,
        "track_name": track_name,
        "track_display_name": display_name,
        "source_track": source_track,
        "source_crc": int(source_crc_value),
        "closed_loop": bool(closed_loop),
        "length_cm": round(float(length_m) * 100.0, 1),
        "metadata": metadata,
        "dressing": {"season": "summer", "spectators": True},
        "imported": "ac",
        "mesh_blob": blob_name,
        "materials": sorted(materials, key=lambda m: m["key"]),
        "meshes": headers,
        "props": [],
        "grid": grid,
        "centerline": centerline,
        "pit_lane": pit_lane,
        "start_finish": start_finish,
    }
    text = json.dumps(manifest, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    blob_path = out_dir / blob_name
    scene_path = out_dir / f"{stem}.uescene.json"
    _write_atomic(blob_path, blob)
    _write_atomic(scene_path, text.encode("utf-8"))
    return scene_path, blob_path


def _write_atomic(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)

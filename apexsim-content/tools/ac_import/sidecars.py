"""The server's four sidecars, written the way `rmp_serde::to_vec_named`
writes them: MessagePack maps with the field names of `server/src/
road_mesh.rs`, `walls.rs`, `ground.rs` and `curbs.rs`. Every file goes
through a temp file and a rename, like the exporter's.
"""

from __future__ import annotations

import os
from pathlib import Path

import msgpack
import numpy as np

CONTACT_ROAD = 0
CONTACT_CURB = 1
CONTACT_RUNOFF = 2
CONTACT_OFF = 3
CONTACT_PIT_LANE = 4
CONTACT_NAMES = {CONTACT_ROAD: "road", CONTACT_CURB: "curb", CONTACT_RUNOFF: "runoff",
                 CONTACT_OFF: "off", CONTACT_PIT_LANE: "pit_lane"}

WALL_ARMCO = 0
WALL_TIRES = 1
WALL_CONCRETE = 2


def write_msgpack(path: Path, payload) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = msgpack.packb(payload, use_bin_type=True, use_single_float=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def road_mesh_payload(vertices: np.ndarray, triangles: np.ndarray, triangle_surface: np.ndarray,
                      surfaces: list[dict], source: str) -> dict:
    """`RoadMeshFile` version 1. `surfaces` entries: key, contact, friction,
    valid_track, pit_lane."""
    v = np.asarray(vertices, dtype=np.float32)
    t = np.asarray(triangles, dtype=np.uint32)
    s = np.asarray(triangle_surface, dtype=np.uint16)
    return {
        "version": 1,
        "vertices": [[float(a), float(b), float(c)] for a, b, c in v.tolist()],
        "triangles": [[int(a), int(b), int(c)] for a, b, c in t.tolist()],
        "triangle_surface": [int(x) for x in s.tolist()],
        "surfaces": [
            {
                "key": str(e["key"]),
                "contact": int(e["contact"]),
                "friction": float(e["friction"]),
                "valid_track": bool(e["valid_track"]),
                "pit_lane": bool(e["pit_lane"]),
            }
            for e in surfaces
        ],
        "source": source,
    }


def walls_payload(segments: np.ndarray, kinds: np.ndarray) -> dict:
    """`WallFile` version 1: segments as (x0, y0, x1, y1, z, height) rows."""
    seg = np.asarray(segments, dtype=np.float64).reshape(-1, 6)
    kinds = np.asarray(kinds, dtype=np.int64).reshape(-1)
    return {
        "version": 1,
        "segments": [
            {
                "x0": float(r[0]), "y0": float(r[1]), "x1": float(r[2]), "y1": float(r[3]),
                "z": float(r[4]), "height_m": float(r[5]), "kind": int(k),
            }
            for r, k in zip(seg.tolist(), kinds.tolist())
        ],
    }


def ground_payload(origin_x: float, origin_y: float, cell_m: float, heights_m: np.ndarray) -> dict:
    """`GroundHeightfield` version 1: `heights_m` is (rows, cols) metres,
    stored as i16 centimetres (so +-327 m about the start line)."""
    h = np.asarray(heights_m, dtype=np.float64)
    rows, cols = h.shape
    cm = np.clip(np.rint(h * 100.0), -32767, 32767).astype(np.int16)
    return {
        "version": 1,
        "origin_x": float(origin_x),
        "origin_y": float(origin_y),
        "cell_m": float(cell_m),
        "cols": int(cols),
        "rows": int(rows),
        "heights_cm": [int(x) for x in cm.reshape(-1).tolist()],
    }


def curbs_payload(step_m: float, left_m: np.ndarray, right_m: np.ndarray,
                  runoff_left_m: np.ndarray, runoff_right_m: np.ndarray) -> dict:
    """`CurbBands` version 2, one sample per `step_m` of station."""
    def cm(a: np.ndarray) -> list[int]:
        return [int(x) for x in np.clip(np.rint(np.asarray(a, dtype=np.float64) * 100.0), 0, 65535).astype(np.uint16).tolist()]

    return {
        "version": 2,
        "step_m": float(step_m),
        "left_cm": cm(left_m),
        "right_cm": cm(right_m),
        "runoff_left_cm": cm(runoff_left_m),
        "runoff_right_cm": cm(runoff_right_m),
    }

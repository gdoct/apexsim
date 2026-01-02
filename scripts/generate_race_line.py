import os
import yaml
import numpy as np
from dataclasses import dataclass
from typing import List, Optional
from scipy.interpolate import CubicSpline
from scipy.optimize import minimize


# ------------------------------------------------------------
# Data classes
# ------------------------------------------------------------

@dataclass
class TrackNode:
    x: float
    y: float
    z: float = 0.0
    width: Optional[float] = None
    width_left: Optional[float] = None
    width_right: Optional[float] = None
    banking: float = 0.0
    friction: float = 1.0
    surface_type: str = "Asphalt"


@dataclass
class IdealLineResult:
    s: np.ndarray
    x: np.ndarray
    y: np.ndarray
    offset: np.ndarray


# ------------------------------------------------------------
# Geometry helpers
# ------------------------------------------------------------

def build_arc_length(nodes: List[TrackNode]) -> np.ndarray:
    xs = np.array([n.x for n in nodes])
    ys = np.array([n.y for n in nodes])
    ds = np.hypot(np.diff(xs), np.diff(ys))
    return np.concatenate(([0.0], np.cumsum(ds)))


def interpolate_scalar(s: np.ndarray, values: np.ndarray) -> CubicSpline:
    return CubicSpline(s, values, bc_type='periodic')


def build_center_splines(nodes: List[TrackNode]):
    # Detect closed loop
    is_closed = False
    if hasattr(nodes[0], "x") and hasattr(nodes[-1], "x"):
        dx = nodes[0].x - nodes[-1].x
        dy = nodes[0].y - nodes[-1].y
        if (dx*dx + dy*dy) < 1e-6:  # already identical
            is_closed = True

    # If closed_loop but endpoints differ → append first node
    if not is_closed:
        # Check metadata if available
        if hasattr(nodes, "__dict__"):
            pass
        # Or simply assume all tracks are closed unless stated otherwise
        # (your YAML has closed_loop: true)
        if True:
            nodes = nodes + [TrackNode(**nodes[0].__dict__)]
            is_closed = True

    # Build arc length
    s = build_arc_length(nodes)
    xs = np.array([n.x for n in nodes])
    ys = np.array([n.y for n in nodes])

    # Use periodic BC only if closed
    bc = 'periodic' if is_closed else 'natural'

    sx = CubicSpline(s, xs, bc_type=bc)
    sy = CubicSpline(s, ys, bc_type=bc)

    # Widths
    w_left = np.array([n.width_left for n in nodes], dtype=float)
    w_right = np.array([n.width_right for n in nodes], dtype=float)

    def fill(arr, default=5.0):
        mask = np.isnan(arr)
        if np.all(mask):
            arr[:] = default
        else:
            idx = np.where(~mask, np.arange(len(arr)), 0)
            np.maximum.accumulate(idx, out=idx)
            arr[mask] = arr[idx[mask]]
        return arr

    w_left = fill(w_left)
    w_right = fill(w_right)

    sw_left = CubicSpline(s, w_left, bc_type=bc)
    sw_right = CubicSpline(s, w_right, bc_type=bc)

    return s, sx, sy, sw_left, sw_right


def compute_center_and_edges(s_grid, sx, sy, sw_left, sw_right):
    cx = sx(s_grid)
    cy = sy(s_grid)

    dx = sx.derivative()(s_grid)
    dy = sy.derivative()(s_grid)
    tlen = np.hypot(dx, dy)
    tlen[tlen == 0] = 1e-9

    tx = dx / tlen
    ty = dy / tlen

    nx = -ty
    ny = tx

    wl = sw_left(s_grid)
    wr = sw_right(s_grid)

    return cx, cy, nx, ny, wl, wr


def curvature_cost(x, y):
    dx = np.diff(x)
    dy = np.diff(y)
    seg_len = np.hypot(dx, dy)
    seg_len[seg_len == 0] = 1e-9

    ux = dx / seg_len
    uy = dy / seg_len

    dux = np.diff(ux)
    duy = np.diff(uy)
    return np.sum(dux**2 + duy**2)


def smoothness_cost(offset):
    d2 = np.diff(offset, n=2)
    return np.sum(d2**2)


# ------------------------------------------------------------
# Ideal racing line solver
# ------------------------------------------------------------

def ideal_racing_line(nodes: List[TrackNode],
                      num_samples=400,
                      lambda_smooth=2.0) -> IdealLineResult:

    s_raw, sx, sy, sw_left, sw_right = build_center_splines(nodes)
    total_length = s_raw[-1]

    s_grid = np.linspace(0, total_length, num_samples, endpoint=False)

    cx, cy, nx, ny, wl, wr = compute_center_and_edges(
        s_grid, sx, sy, sw_left, sw_right
    )

    bounds = [(-wr[i], wl[i]) for i in range(num_samples)]
    o0 = np.zeros(num_samples)

    def objective(o):
        x = cx + nx * o
        y = cy + ny * o
        return curvature_cost(x, y) + lambda_smooth * smoothness_cost(o)

    res = minimize(
        objective,
        o0,
        method='L-BFGS-B',
        bounds=bounds,
        options={'maxiter': 500}
    )

    o_opt = res.x
    x_opt = cx + nx * o_opt
    y_opt = cy + ny * o_opt

    return IdealLineResult(s=s_grid, x=x_opt, y=y_opt, offset=o_opt)


# ------------------------------------------------------------
# YAML I/O
# ------------------------------------------------------------

def load_track_yaml(path: str) -> List[TrackNode]:
    with open(path, "r") as f:
        data = yaml.safe_load(f)

    nodes = []
    for n in data["nodes"]:
        nodes.append(TrackNode(
            x=n["x"],
            y=n["y"],
            z=n.get("z", 0.0),
            width=n.get("width"),
            width_left=n.get("width_left"),
            width_right=n.get("width_right"),
            banking=n.get("banking", 0.0),
            friction=n.get("friction", 1.0),
            surface_type=n.get("surface_type", "Asphalt")
        ))
    return nodes


def save_raceline_yaml(path: str, result: IdealLineResult):
    out = {
        "raceline": [
            {
                "s": float(result.s[i]),
                "x": float(result.x[i]),
                "y": float(result.y[i]),
                "offset": float(result.offset[i])
            }
            for i in range(len(result.s))
        ]
    }

    with open(path, "w") as f:
        yaml.dump(out, f, sort_keys=False)


# ------------------------------------------------------------
# Batch processing
# ------------------------------------------------------------

def process_folder(folder: str, samples=400, smoothness=2.0):
    for file in os.listdir(folder):
        if not file.endswith(".yaml"):
            continue

        in_path = os.path.join(folder, file)
        out_path = os.path.join(folder, file.replace(".yaml", ".raceline.yaml"))

        print(f"Processing {file} → {os.path.basename(out_path)}")

        nodes = load_track_yaml(in_path)
        result = ideal_racing_line(nodes, num_samples=samples, lambda_smooth=smoothness)
        save_raceline_yaml(out_path, result)


# ------------------------------------------------------------
# CLI entry point
# ------------------------------------------------------------

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Generate ideal racing lines for track YAML files.")
    parser.add_argument("--input-folder", required=True, help="Folder containing track YAML files")
    parser.add_argument("--samples", type=int, default=400, help="Number of samples along the track")
    parser.add_argument("--smoothness", type=float, default=2.0, help="Smoothness weight")

    args = parser.parse_args()

    process_folder(args.input_folder, samples=args.samples, smoothness=args.smoothness)

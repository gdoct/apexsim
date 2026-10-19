import os
import yaml
import numpy as np
from dataclasses import dataclass
from typing import List, Optional


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
    z: np.ndarray
    offset: np.ndarray


# ------------------------------------------------------------
# Geometry helpers
# ------------------------------------------------------------

def build_arc_length(nodes: List[TrackNode]) -> np.ndarray:
    xs = np.array([n.x for n in nodes])
    ys = np.array([n.y for n in nodes])
    ds = np.hypot(np.diff(xs), np.diff(ys))
    return np.concatenate(([0.0], np.cumsum(ds)))


def interpolate_scalar(s: np.ndarray, values: np.ndarray):
    from scipy.interpolate import CubicSpline
    return CubicSpline(s, values, bc_type='periodic')


def build_center_splines(nodes: List[TrackNode]):
    # The old solver's dependency; the per-node --track mode needs only numpy.
    from scipy.interpolate import CubicSpline
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
    zs = np.array([n.z for n in nodes])

    # Use periodic BC only if closed
    bc = 'periodic' if is_closed else 'natural'

    sx = CubicSpline(s, xs, bc_type=bc)
    sy = CubicSpline(s, ys, bc_type=bc)
    sz = CubicSpline(s, zs, bc_type=bc)

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

    return s, sx, sy, sz, sw_left, sw_right


def compute_center_and_edges(s_grid, sx, sy, sz, sw_left, sw_right):
    cx = sx(s_grid)
    cy = sy(s_grid)
    cz = sz(s_grid)

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

    return cx, cy, cz, nx, ny, wl, wr


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

    s_raw, sx, sy, sz, sw_left, sw_right = build_center_splines(nodes)
    total_length = s_raw[-1]

    s_grid = np.linspace(0, total_length, num_samples, endpoint=False)

    cx, cy, cz, nx, ny, wl, wr = compute_center_and_edges(
        s_grid, sx, sy, sz, sw_left, sw_right
    )

    bounds = [(-wr[i], wl[i]) for i in range(num_samples)]
    o0 = np.zeros(num_samples)

    def objective(o):
        x = cx + nx * o
        y = cy + ny * o
        return curvature_cost(x, y) + lambda_smooth * smoothness_cost(o)

    from scipy.optimize import minimize
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
    z_opt = cz  # z is interpolated from centerline at the racing line's s positions

    return IdealLineResult(s=s_grid, x=x_opt, y=y_opt, z=z_opt, offset=o_opt)


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


def save_raceline_to_track(path: str, result: IdealLineResult):
    """Update or add raceline data in the existing track YAML file."""
    with open(path, "r") as f:
        data = yaml.safe_load(f)

    data["raceline"] = [
        {
            "x": float(result.x[i]),
            "y": float(result.y[i]),
            "z": float(result.z[i]),
        }
        for i in range(len(result.s))
    ]

    with open(path, "w") as f:
        yaml.dump(data, f, sort_keys=False, default_flow_style=False)


# ------------------------------------------------------------
# Per-node minimum-curvature line (the one tracks are given now)
# ------------------------------------------------------------

def min_curvature_line(data: dict, margin: float = 1.5, smooth: float = 1.0):
    """The minimum-curvature line through a closed track, one point per node.

    Each point is its node moved `o_i` along the node's left normal, and the
    offsets minimise the squared second difference of the points (at even
    node spacing, the curvature) plus `smooth` times that of the offsets,
    inside the road less `margin` on each side. That is a bounded linear
    least-squares problem, solved exactly (scipy's `lsq_linear`) rather than
    by the finite-difference descent `ideal_racing_line` uses, which is too
    slow and too coarse (400 samples) for a 13 km lap of 5 m nodes.
    Returns arrays x, y, z (z is the node's own height).
    """

    nodes = data["nodes"]
    n = len(nodes)
    cx = np.array([nd["x"] for nd in nodes], dtype=float)
    cy = np.array([nd["y"] for nd in nodes], dtype=float)
    cz = np.array([nd.get("z", 0.0) or 0.0 for nd in nodes], dtype=float)
    default_half = float(data.get("default_width") or 12.0) / 2.0

    def half(nd, key):
        v = nd.get(key)
        if v is None:
            w = nd.get("width")
            return float(w) / 2.0 if w else default_half
        return float(v)

    wl = np.array([half(nd, "width_left") for nd in nodes])
    wr = np.array([half(nd, "width_right") for nd in nodes])

    # Left normals from the neighbours (closed loop).
    tx = np.roll(cx, -1) - np.roll(cx, 1)
    ty = np.roll(cy, -1) - np.roll(cy, 1)
    tl = np.hypot(tx, ty)
    tl[tl == 0] = 1e-9
    nx, ny = -ty / tl, tx / tl

    # Second difference on the closed loop as a dense matrix (n is a few
    # thousand: a dense solve is a second or two, and needs only numpy).
    eye = np.eye(n)
    d2m = np.roll(eye, -1, axis=1) - 2.0 * eye + np.roll(eye, 1, axis=1)

    def d2(v):
        return np.roll(v, 1) - 2.0 * v + np.roll(v, -1)

    # Points P = C + diag(n) o: minimise |D2(Cx + nx o)|^2 + |D2(Cy + ny o)|^2
    # + smooth |D2 o|^2, i.e. the normal equations H o = g below.
    ax = d2m * nx[None, :]
    ay = d2m * ny[None, :]
    h = ax.T @ ax + ay.T @ ay + smooth * (d2m.T @ d2m) + np.eye(n) * 1e-9
    g = -(ax.T @ d2(cx) + ay.T @ d2(cy))

    lo = -np.maximum(wr - margin, 0.0)
    hi = np.maximum(wl - margin, 0.0)

    # Active set: solve with the clamped offsets held at their bound, clamp
    # whatever leaves the road, release a held offset whose gradient points
    # back inside; until nothing changes.
    at_lo = np.zeros(n, dtype=bool)
    at_hi = np.zeros(n, dtype=bool)
    o = np.zeros(n)
    for _ in range(300):
        fixed = at_lo | at_hi
        free = ~fixed
        o[at_lo] = lo[at_lo]
        o[at_hi] = hi[at_hi]
        rhs = g[free] - h[np.ix_(free, fixed)] @ o[fixed]
        o[free] = np.linalg.solve(h[np.ix_(free, free)], rhs)
        new_lo = free & (o < lo - 1e-6)
        new_hi = free & (o > hi + 1e-6)
        grad = h @ o - g  # gradient of the objective (halved)
        release = (at_lo & (grad < 0)) | (at_hi & (grad > 0))
        if not new_lo.any() and not new_hi.any() and not release.any():
            break
        at_lo = (at_lo | new_lo) & ~release
        at_hi = (at_hi | new_hi) & ~release
    o = np.clip(o, lo, hi)
    return cx + nx * o, cy + ny * o, cz, o


def write_raceline_in_place(path: str, x, y, z):
    """Replace the YAML's `raceline:` block (or `raceline: []`) with the line,
    in the shape every track file uses, leaving every other byte alone."""
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    start = next(i for i, l in enumerate(lines) if l.startswith("raceline:"))
    end = start + 1
    while end < len(lines) and (lines[end].startswith("- ") or lines[end].startswith("  ")):
        end += 1
    block = ["raceline:"]
    for xi, yi, zi in zip(x, y, z):
        block += [f"- x: {xi:.2f}", f"  y: {yi:.2f}", f"  z: {zi:.3f}"]
    lines[start:end] = block
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(nl.join(lines))


def process_track(path: str, margin: float = 1.5, smooth: float = 1.0):
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    x, y, z, o = min_curvature_line(data, margin=margin, smooth=smooth)
    write_raceline_in_place(path, x, y, z)
    print(f"{os.path.basename(path)}: {len(x)} points, offset {o.min():.1f} .. {o.max():.1f} m, "
          f"mean |offset| {np.abs(o).mean():.1f} m")


# ------------------------------------------------------------
# Batch processing
# ------------------------------------------------------------

def process_folder(folder: str, samples=400, smoothness=2.0):
    for file in os.listdir(folder):
        if not file.endswith(".yaml"):
            continue

        path = os.path.join(folder, file)

        print(f"Processing {file}")

        nodes = load_track_yaml(path)
        result = ideal_racing_line(nodes, num_samples=samples, lambda_smooth=smoothness)
        save_raceline_to_track(path, result)


# ------------------------------------------------------------
# CLI entry point
# ------------------------------------------------------------

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Generate ideal racing lines for track YAML files.")
    parser.add_argument("--input-folder", help="Folder containing track YAML files (the old 400-sample solver, rewrites each file)")
    parser.add_argument("--track", nargs="*", default=[], help="Track YAML(s): a per-node minimum-curvature line, written in place")
    parser.add_argument("--margin", type=float, default=1.5, help="Metres kept inside each road edge (--track)")
    parser.add_argument("--samples", type=int, default=400, help="Number of samples along the track")
    parser.add_argument("--smoothness", type=float, default=2.0, help="Smoothness weight")

    args = parser.parse_args()

    for track in args.track:
        process_track(track, margin=args.margin, smooth=args.smoothness)
    if args.input_folder:
        process_folder(args.input_folder, samples=args.samples, smoothness=args.smoothness)
    if not args.track and not args.input_folder:
        parser.error("pass --track or --input-folder")

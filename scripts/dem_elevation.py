#!/usr/bin/env python3
"""Re-derive a circuit's centerline elevation from its DEM sidecar.

Every real circuit's `z` came from `enrich_all_tracks.py`, which laid a
handful of invented keyframes ("12 m up at 5% of the lap") over the GPS
trace. At Spa that put a crest where the Eau Rouge dip is and had the road
descend up Raidillon: at Eau Rouge the YAML was 80 m above the ground the
Copernicus model puts there. `terrain.rs` lets the surveyed centerline win
within 40 m of the road, so the rendered road, the physics and the AI all
drove the invented profile through a real valley.

The DEM sidecar (`<Stem>.dem.msgpack`, `dem_fetch.py`) is the measurement,
already in the track frame and tied to the YAML's own datum at the
start/finish line. This script walks the road over its 10 m `inner` grid
and fits a profile to it:

- Each node takes the median of the model at the centerline and a few
  metres either side, so a line of trees along one verge is outvoted.
- The profile is a Whittaker smoother (second differences, periodic on a
  closed lap) whose half-power wavelength is `--wavelength` metres: the
  model's posts are 30 m apart, so nothing shorter than that is real
  relief, and a car feels every bump the fit leaves in.
- The model is a *surface* model, so where the road runs through woods it
  reads the canopy, which is only ever above the road. The fit is
  reweighted until samples more than `--canopy-tol` above it carry almost
  no weight: it hugs the lower envelope where trees poke up and the plain
  fit everywhere else.
- The start/finish node keeps its `z` to the bit, because `dem_fetch.py`
  ties the model to exactly that value; the fit's small disagreement there
  is tapered out over `DATUM_TAPER_M` either side.

The raceline is carried by the change of the road beneath it, as
`ats-smooth` carries it in plan, so its height above the centerline
(banking) is unchanged.

    python scripts/dem_elevation.py --report          # every circuit with a sidecar, worst first
    python scripts/dem_elevation.py Spa [--dry-run]   # rewrite Spa.yaml's node and raceline z

Only `z` lines are touched; the file is otherwise byte-identical. Run it
before the rest of the refresh order in CLAUDE.md (`ats-smooth` onward).
"""

from __future__ import annotations

import argparse
import math
import re
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO / "content" / "tracks" / "real"

# Half-power wavelength of the along-road fit. At Spa 250 m keeps the
# bottom of Eau Rouge within a metre of the model and the Les Combes crest
# within 0.2 m, while its tightest vertical radius is ~700 m (0.9 g at the
# bottom of Eau Rouge). 120 m followed the model's every wiggle: a 390 m
# radius on a Blanchimont hump that is more likely hillside trees than
# road, nearly 2 g at 300 km/h.
DEFAULT_WAVELENGTH_M = 250.0
# A sample this far above the fit is taken for canopy, not road.
DEFAULT_CANOPY_TOL_M = 1.5
CANOPY_WEIGHT = 0.02
# How far either side of the line the fit's disagreement with the datum is
# spread.
DATUM_TAPER_M = 400.0
# The lateral samples sit this fraction of the narrower half-width off the
# centerline, within these bounds: on the road, clear of its verges.
LATERAL_FRACTION = 0.5
LATERAL_MIN_M, LATERAL_MAX_M = 2.0, 6.0


# ------------------------------------------------------------------ reading


def read_dem(stem: str) -> dict | None:
    """The sidecar's `inner` grid as a dict with `h` (rows x cols), or None
    for a circuit without one. msgpack-python is only needed here."""
    path = TRACK_DIR / f"{stem}.dem.msgpack"
    if not path.exists():
        return None
    import msgpack

    doc = msgpack.unpackb(path.read_bytes(), raw=False)
    g = doc["inner"]
    g["h"] = np.asarray(g.pop("heights"), dtype=np.float64).reshape(g["rows"], g["cols"])
    return g


def sample(g: dict, xy: np.ndarray) -> np.ndarray:
    """Bilinear height, clamped at the border, as `dem.rs` samples it."""
    fx = (xy[:, 0] - g["origin_x"]) / g["cell_m"]
    fy = (xy[:, 1] - g["origin_y"]) / g["cell_m"]
    c = np.clip(np.floor(fx), 0, g["cols"] - 2).astype(int)
    r = np.clip(np.floor(fy), 0, g["rows"] - 2).astype(int)
    u = np.clip(fx - c, 0.0, 1.0)
    v = np.clip(fy - r, 0.0, 1.0)
    h = g["h"]
    top = h[r, c] * (1 - u) + h[r, c + 1] * u
    bottom = h[r + 1, c] * (1 - u) + h[r + 1, c + 1] * u
    return top * (1 - v) + bottom * v


_TOP = re.compile(r"^([A-Za-z_]\w*):")
_Z = re.compile(r"^(- |  )z: (.*)$")


class TrackText:
    """A track YAML kept as its lines, with the few fields this script reads
    parsed out of them. Rewriting `z` in place leaves every other byte of
    the file as the Rust writer made it."""

    def __init__(self, path: Path):
        self.path = path
        raw = path.read_bytes().decode("utf-8")
        self.eol = "\r\n" if "\r\n" in raw else "\n"
        self.lines = raw.split(self.eol)
        self.items: dict[str, list[dict]] = {"nodes": [], "raceline": []}
        self.closed = True
        section = None
        for i, line in enumerate(self.lines):
            m = _TOP.match(line)
            if m:
                section = m.group(1)
                if section == "closed_loop":
                    self.closed = line.split(":", 1)[1].strip() == "true"
                continue
            if section not in self.items:
                continue
            if line.startswith("- "):
                self.items[section].append({})
            if not self.items[section] or not line.strip():
                continue
            key, _, value = line[2:].partition(":")
            value = value.strip()
            item = self.items[section][-1]
            if key == "z":
                item["z_line"] = i
            try:
                item[key] = float(value)
            except ValueError:
                item[key] = None

    def column(self, section: str, key: str, default=0.0) -> np.ndarray:
        return np.array(
            [default if it.get(key) is None else it[key] for it in self.items[section]],
            dtype=np.float64,
        )

    def set_z(self, section: str, z: np.ndarray, decimals: int):
        for item, value in zip(self.items[section], z):
            i = item.get("z_line")
            if i is None:
                raise SystemExit(f"{self.path.name}: a {section} entry has no z line")
            v = round(float(value), decimals)
            v = 0.0 if v == 0 else v  # never "-0.0"
            m = _Z.match(self.lines[i])
            self.lines[i] = f"{m.group(1)}z: {v!r}"

    def write(self):
        self.path.write_bytes(self.eol.join(self.lines).encode("utf-8"))


# -------------------------------------------------------------------- maths


def stations(xy: np.ndarray, closed: bool) -> tuple[np.ndarray, float]:
    ring = np.vstack([xy, xy[:1]]) if closed else xy
    seg = np.hypot(*np.diff(ring, axis=0).T)
    s = np.concatenate([[0.0], np.cumsum(seg)])
    return s[: len(xy)], float(s[-1])


def normals(xy: np.ndarray, closed: bool) -> np.ndarray:
    """Unit left normals (+Y is left of the direction of travel)."""
    if closed:
        t = np.roll(xy, -1, axis=0) - np.roll(xy, 1, axis=0)
    else:
        t = np.gradient(xy, axis=0)
    t /= np.maximum(np.linalg.norm(t, axis=1), 1e-9)[:, None]
    return np.column_stack([-t[:, 1], t[:, 0]])


def road_samples(g: dict, t: TrackText) -> np.ndarray:
    """The model under each node: the median of the centerline and a point
    either side of it on the road."""
    xy = np.column_stack([t.column("nodes", "x"), t.column("nodes", "y")])
    half = np.minimum(
        t.column("nodes", "width_left", np.nan), t.column("nodes", "width_right", np.nan)
    )
    half = np.where(np.isfinite(half), half, 5.0)
    off = np.clip(half * LATERAL_FRACTION, LATERAL_MIN_M, LATERAL_MAX_M)[:, None]
    n = normals(xy, t.closed)
    return np.median(
        np.vstack([sample(g, xy), sample(g, xy + n * off), sample(g, xy - n * off)]), axis=0
    )


def second_differences(n: int, closed: bool) -> np.ndarray:
    if closed:
        d = np.zeros((n, n))
        idx = np.arange(n)
        d[idx, (idx - 1) % n] = 1.0
        d[idx, idx] = -2.0
        d[idx, (idx + 1) % n] = 1.0
        return d
    d = np.zeros((n - 2, n))
    idx = np.arange(n - 2)
    d[idx, idx], d[idx, idx + 1], d[idx, idx + 2] = 1.0, -2.0, 1.0
    return d


def fit_profile(
    y: np.ndarray, spacing_m: float, closed: bool, wavelength_m: float, canopy_tol_m: float
) -> np.ndarray:
    """Whittaker smoother, reweighted against canopy. Its transfer function
    is 1 / (1 + lam (w h)^4), so the half-power wavelength L gives
    lam = (L / (2 pi h))^4."""
    lam = (wavelength_m / (2.0 * math.pi * spacing_m)) ** 4
    d = second_differences(len(y), closed)
    penalty = lam * (d.T @ d)
    w = np.ones_like(y)
    z = y
    for _ in range(50):
        z = np.linalg.solve(np.diag(w) + penalty, w * y)
        nw = np.where(y - z > canopy_tol_m, CANOPY_WEIGHT, 1.0)
        if np.array_equal(nw, w):
            break
        w = nw
    return z


def loop_distance(s: np.ndarray, at: float, total: float, closed: bool) -> np.ndarray:
    d = np.abs(s - at)
    return np.minimum(d, total - d) if closed else d


def profile(stem: str, wavelength_m: float, canopy_tol_m: float):
    """(track text, stations, lap length, yaml z, dem samples, fitted z) or
    None without a sidecar."""
    g = read_dem(stem)
    if g is None:
        return None
    t = TrackText(TRACK_DIR / f"{stem}.yaml")
    xy = np.column_stack([t.column("nodes", "x"), t.column("nodes", "y")])
    s, total = stations(xy, t.closed)
    z_yaml = t.column("nodes", "z")
    y = road_samples(g, t)
    fit = fit_profile(y, total / len(s), t.closed, wavelength_m, canopy_tol_m)
    # Hold the datum: the model is tied to node 0's z, so node 0 keeps it.
    taper = np.clip(1.0 - loop_distance(s, 0.0, total, t.closed) / DATUM_TAPER_M, 0.0, 1.0)
    fit = fit + (z_yaml[0] - fit[0]) * taper
    return t, s, total, z_yaml, y, fit


def carry_raceline(t: TrackText, s: np.ndarray, total: float, dz: np.ndarray) -> np.ndarray:
    """Each raceline point's z moved by the road's change at its station.
    The raceline runs in lap order, so each point is looked for near the
    last one's node: a crossover (Suzuka) cannot capture it onto the other
    leg."""
    xy = np.column_stack([t.column("nodes", "x"), t.column("nodes", "y")])
    rl = np.column_stack([t.column("raceline", "x"), t.column("raceline", "y")])
    z = t.column("raceline", "z")
    n = len(xy)
    tangent = np.roll(xy, -1, axis=0) - xy if t.closed else np.gradient(xy, axis=0)
    tangent /= np.maximum(np.linalg.norm(tangent, axis=1), 1e-9)[:, None]
    out = z.copy()
    at = None
    for k, p in enumerate(rl):
        if at is None:
            cand = np.arange(n)
        else:
            cand = np.arange(at - 40, at + 41)
            cand = cand % n if t.closed else cand[(cand >= 0) & (cand < n)]
        at = int(cand[np.argmin(np.hypot(*(xy[cand] - p).T))])
        st = s[at] + float(np.dot(p - xy[at], tangent[at]))
        if t.closed:
            out[k] += np.interp(st % total, s, dz, period=total)
        else:
            out[k] += np.interp(st, s, dz)
    return out


# ------------------------------------------------------------------ driving


def report(wavelength_m: float, canopy_tol_m: float):
    rows = []
    for path in sorted(TRACK_DIR.glob("*.dem.msgpack")):
        stem = path.name.split(".")[0]
        if not (TRACK_DIR / f"{stem}.yaml").exists():
            continue
        _, s, _, z, _, fit = profile(stem, wavelength_m, canopy_tol_m)
        d = z - fit
        k = int(np.argmax(np.abs(d)))
        rows.append(
            (
                float(np.abs(d).max()),
                stem,
                math.sqrt(float((d**2).mean())),
                float(d[k]),
                float(s[k]),
                float(z.max() - z.min()),
                float(fit.max() - fit.min()),
            )
        )
    rows.sort(reverse=True)
    print(f"{'circuit':14} {'rms':>6} {'worst':>7} {'at m':>6} {'yaml relief':>12} {'dem relief':>11}")
    for worst, stem, rms, signed, at, ry, rd in rows:
        print(f"{stem:14} {rms:6.1f} {signed:+7.1f} {at:6.0f} {ry:11.1f}m {rd:10.1f}m")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracks", nargs="*", help="track stems to rewrite, e.g. Spa")
    ap.add_argument("--report", action="store_true", help="compare every circuit with a sidecar")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    ap.add_argument("--wavelength", type=float, default=DEFAULT_WAVELENGTH_M)
    ap.add_argument("--canopy-tol", type=float, default=DEFAULT_CANOPY_TOL_M)
    args = ap.parse_args()
    if args.report:
        report(args.wavelength, args.canopy_tol)
    if not args.tracks and not args.report:
        ap.print_help()
        return 1
    for stem in args.tracks:
        got = profile(stem, args.wavelength, args.canopy_tol)
        if got is None:
            raise SystemExit(f"{stem}: no {stem}.dem.msgpack; run dem_fetch.py first")
        t, s, total, z, y, fit = got
        fit = np.round(fit, 2)
        fit[0] = z[0]
        d = z - fit
        canopy = y - fit
        print(
            f"== {stem}: yaml vs model rms {math.sqrt(float((d**2).mean())):.1f} m, "
            f"worst {float(d[np.argmax(np.abs(d))]):+.1f} m at {float(s[np.argmax(np.abs(d))]):.0f} m; "
            f"relief {float(z.max() - z.min()):.0f} m -> {float(fit.max() - fit.min()):.0f} m; "
            f"{int((canopy > args.canopy_tol).sum())} of {len(s)} samples read as canopy "
            f"(up to {float(canopy.max()):.1f} m)"
        )
        grade = np.diff(fit) / np.maximum(np.diff(s), 1e-6)
        print(f"   steepest grade {100 * float(np.abs(grade).max()):.1f}%")
        if args.dry_run:
            continue
        rl = carry_raceline(t, s, total, fit - z)
        t.set_z("nodes", fit, 2)
        t.set_z("raceline", rl, 3)
        t.write()
        print(f"   wrote {t.path.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

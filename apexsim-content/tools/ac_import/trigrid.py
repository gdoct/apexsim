"""A uniform grid over triangles, for vectorised "what is under this point"
queries: the same rule as the server's `road_mesh.rs` (the highest triangle
under `(x, y)` whose surface is at or below a ceiling), over arrays of
points at once.
"""

from __future__ import annotations

import numpy as np


class TriangleIndex:
    def __init__(self, vertices: np.ndarray, triangles: np.ndarray, cell_m: float = 4.0):
        self.vertices = np.asarray(vertices, dtype=np.float64)
        self.triangles = np.asarray(triangles, dtype=np.int64).reshape(-1, 3)
        self.cell_m = float(cell_m)
        n = self.triangles.shape[0]
        self.count = n
        if n == 0:
            self.origin = np.zeros(2)
            self.cols = self.rows = 0
            self.starts = np.zeros(1, dtype=np.int64)
            self.entries = np.zeros(0, dtype=np.int64)
            return
        a = self.vertices[self.triangles[:, 0]]
        b = self.vertices[self.triangles[:, 1]]
        c = self.vertices[self.triangles[:, 2]]
        # Plane per triangle: z = pz - (nx*(x-ax) + ny*(y-ay)) / nz. Flat
        # (vertical) triangles get nz = 0 and are never hit.
        n_vec = np.cross(b - a, c - a)
        self.plane_n = n_vec
        self.a = a
        self.b = b
        self.c = c
        # 2D edge function denominator (twice the signed footprint).
        self.twice = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
        lo = np.minimum(np.minimum(a[:, :2], b[:, :2]), c[:, :2])
        hi = np.maximum(np.maximum(a[:, :2], b[:, :2]), c[:, :2])
        self.origin = lo.min(axis=0) - 1.0
        span = hi.max(axis=0) + 1.0 - self.origin
        cell = self.cell_m
        # Keep the grid under ~4M cells for the odd enormous mod.
        while (span[0] / cell) * (span[1] / cell) > 4_000_000:
            cell *= 2.0
        self.cell_m = cell
        self.cols = int(np.ceil(span[0] / cell)) + 1
        self.rows = int(np.ceil(span[1] / cell)) + 1
        c0 = np.floor((lo[:, 0] - self.origin[0]) / cell).astype(np.int64)
        c1 = np.floor((hi[:, 0] - self.origin[0]) / cell).astype(np.int64)
        r0 = np.floor((lo[:, 1] - self.origin[1]) / cell).astype(np.int64)
        r1 = np.floor((hi[:, 1] - self.origin[1]) / cell).astype(np.int64)
        c0 = np.clip(c0, 0, self.cols - 1)
        c1 = np.clip(c1, 0, self.cols - 1)
        r0 = np.clip(r0, 0, self.rows - 1)
        r1 = np.clip(r1, 0, self.rows - 1)
        spans_c = c1 - c0 + 1
        spans_r = r1 - r0 + 1
        per_tri = spans_c * spans_r
        total = int(per_tri.sum())
        tri_ids = np.repeat(np.arange(n), per_tri)
        # Offset of each (tri, cell) pair within its triangle's span.
        offsets = np.arange(total) - np.repeat(np.cumsum(per_tri) - per_tri, per_tri)
        cc = c0[tri_ids] + offsets % spans_c[tri_ids]
        rr = r0[tri_ids] + offsets // spans_c[tri_ids]
        cells = rr * self.cols + cc
        order = np.lexsort((tri_ids, cells))
        cells = cells[order]
        tri_ids = tri_ids[order]
        counts = np.bincount(cells, minlength=self.cols * self.rows)
        self.starts = np.concatenate([[0], np.cumsum(counts)]).astype(np.int64)
        self.entries = tri_ids.astype(np.int64)

    def query(self, xs: np.ndarray, ys: np.ndarray, ceiling: np.ndarray | float,
              chunk: int = 200_000) -> tuple[np.ndarray, np.ndarray]:
        """For each point the highest triangle containing it with surface
        height at or below `ceiling` (scalar or per point): `(z, index)`,
        index -1 (and z NaN) where there is none. Ties within 2 cm go to the
        later triangle, as on the server."""
        xs = np.asarray(xs, dtype=np.float64).reshape(-1)
        ys = np.asarray(ys, dtype=np.float64).reshape(-1)
        m = xs.shape[0]
        ceil = np.broadcast_to(np.asarray(ceiling, dtype=np.float64), (m,))
        z_out = np.full(m, np.nan)
        t_out = np.full(m, -1, dtype=np.int64)
        if self.count == 0 or m == 0:
            return z_out, t_out
        for s in range(0, m, chunk):
            e = min(m, s + chunk)
            self._query_chunk(xs[s:e], ys[s:e], ceil[s:e], z_out[s:e], t_out[s:e])
        return z_out, t_out

    def _query_chunk(self, xs, ys, ceil, z_out, t_out) -> None:
        fc = np.floor((xs - self.origin[0]) / self.cell_m)
        fr = np.floor((ys - self.origin[1]) / self.cell_m)
        inside = (fc >= 0) & (fr >= 0) & (fc < self.cols) & (fr < self.rows)
        pts = np.flatnonzero(inside)
        if pts.size == 0:
            return
        cells = (fr[pts].astype(np.int64) * self.cols + fc[pts].astype(np.int64))
        first = self.starts[cells]
        counts = self.starts[cells + 1] - first
        total = int(counts.sum())
        if total == 0:
            return
        pair_pt = np.repeat(pts, counts)
        offsets = np.arange(total) - np.repeat(np.cumsum(counts) - counts, counts)
        pair_tri = self.entries[np.repeat(first, counts) + offsets]
        x = xs[pair_pt]
        y = ys[pair_pt]
        a = self.a[pair_tri]
        b = self.b[pair_tri]
        c = self.c[pair_tri]
        twice = self.twice[pair_tri]
        ok = np.abs(twice) > 2e-4
        eps = 1e-4
        with np.errstate(divide="ignore", invalid="ignore"):
            w0 = ((b[:, 0] - x) * (c[:, 1] - y) - (b[:, 1] - y) * (c[:, 0] - x)) / twice
            w1 = ((c[:, 0] - x) * (a[:, 1] - y) - (c[:, 1] - y) * (a[:, 0] - x)) / twice
            w2 = 1.0 - w0 - w1
            n = self.plane_n[pair_tri]
            z = a[:, 2] - (n[:, 0] * (x - a[:, 0]) + n[:, 1] * (y - a[:, 1])) / n[:, 2]
        hit = ok & (w0 >= -eps) & (w1 >= -eps) & (w2 >= -eps) & np.isfinite(z) & (z <= ceil[pair_pt])
        if not hit.any():
            return
        pair_pt = pair_pt[hit]
        pair_tri = pair_tri[hit]
        z = z[hit]
        # Highest per point; a tie within 2 cm goes to the later triangle:
        # sort by (point, z rounded up to 2 cm, tri) and take the last.
        zq = np.floor(z / 0.02)
        order = np.lexsort((pair_tri, zq, pair_pt))
        pair_pt = pair_pt[order]
        last = np.flatnonzero(np.diff(pair_pt, append=-1) != 0)
        z_out[pair_pt[last]] = z[order][last]
        t_out[pair_pt[last]] = pair_tri[order][last]

    def bounds(self) -> tuple[np.ndarray, np.ndarray]:
        if self.count == 0:
            return np.zeros(3), np.zeros(3)
        used = self.vertices[self.triangles.reshape(-1)]
        return used.min(axis=0), used.max(axis=0)

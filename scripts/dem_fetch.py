#!/usr/bin/env python3
"""Fetch a public 30 m DEM and write a circuit's real terrain sidecar.

The ground around every circuit is currently invented: `terrain.rs` blankets
the world with an inverse-distance weighting of the centerline's own heights,
so the land can only ever be a smooth cushion around the road.  A driver at
the Red Bull Ring looks up the hill out of Turn 1 and sees sky where the
Styrian ridge should be, and the modelled relief of the ring itself is about
half the real one because the YAML's z came from a smoothed GPS altitude.

This script takes the measurement instead: it downloads the Copernicus GLO-30
tiles covering the circuit, puts them into the track's own coordinate frame
and writes `content/tracks/real/<Stem>.dem.msgpack` -- two grids of heights,
a 10 m one over the circuit itself and a 90 m one out to 8 km, which is the
horizon a driver sees from a valley floor.

    python scripts/dem_fetch.py Spielberg        # one circuit
    python scripts/dem_fetch.py --all            # every circuit with a dossier
    python scripts/dem_fetch.py Monza --offline  # reuse the cached tiles

The georeferencing is not re-derived here: `osm_layout.fit_track` returns the
very fit the layout dossier was built from, and the DEM rides on it.  A second
alignment agreeing to within a metre would still leave the hills a metre off
the grandstands standing on them.

Three data facts worth stating, because each is easy to get half a post, a
few kilometres or a whole datum wrong:

- The Copernicus tiles are `RasterPixelIsPoint`: the tie point is the *centre*
  of the first post, not a corner, so a 1-degree tile's 3600 posts run from
  its northern edge to one step short of its southern one and abutting tiles
  leave neither seam nor overlap.
- Longitude spacing is not 30 m everywhere: the product keeps one arcsecond
  of latitude but decimates longitude by latitude band, so a tile south of
  50 degrees is 3600 posts wide and one north of it only 2400 (Spa, Zandvoort,
  Silverstone, the Nurburgring).  Assuming square posts reads the ground
  kilometres east of where it is, silently and worse the further into the
  tile, which is why the mosaic takes its lattice from the tiles rather
  than from a constant.
- Their heights are orthometric (EGM2008), while a track YAML's z is metres
  above whatever its GPS trace called zero.  Rather than convert, the DEM is
  tied to the track: the height under the start/finish line is subtracted and
  the YAML's z there added back, so the two agree at the line by construction
  and the reported rms difference is honest disagreement about relief rather
  than a datum shift hiding in it.
"""

from __future__ import annotations

import argparse
import math
import struct
import urllib.error
import urllib.request
import zlib
from pathlib import Path

import numpy as np

from osm_layout import BBOXES, REPO, R_EARTH, TRACK_DIR, fit_track

DEM_CACHE = REPO / "content" / "tracks" / "dem-cache"
# AWS's open Copernicus bucket: no credentials, no request signing, one COG
# GeoTIFF per 1-degree tile.
BUCKET = "https://copernicus-dem-30m.s3.amazonaws.com"
SOURCE = (
    "Copernicus DEM GLO-30 (30 m), (c) DLR e.V. 2010-2014 and "
    "(c) Airbus Defence and Space GmbH 2014-2018, provided under COPERNICUS "
    "by the European Union and ESA, free of charge "
    "(https://doi.org/10.5270/ESA-c5d3d65)"
)
DEM_VERSION = 1

# The inner grid is what the car and the near scenery sit on, so it is finer
# than the source and covers the circuit plus a kilometre of its surroundings.
# The outer grid is the view: 8 km reaches the ridge that closes a valley, and
# 90 m posts keep it to a few hundred kilobytes.
INNER_CELL_M = 10.0
INNER_MARGIN_M = 1000.0
OUTER_CELL_M = 90.0
OUTER_MARGIN_M = 8000.0


# ---------------------------------------------------------------- fetching


def tile_id(ilat: int, ilon: int) -> str:
    """The bucket's name for the 1-degree tile whose south-west corner is at
    (ilat, ilon).  Latitude is two digits and longitude three, each signed by
    a hemisphere letter rather than by a minus."""
    ns = "N" if ilat >= 0 else "S"
    ew = "E" if ilon >= 0 else "W"
    return f"Copernicus_DSM_COG_10_{ns}{abs(ilat):02d}_00_{ew}{abs(ilon):03d}_00_DEM"


def fetch_tile(ilat: int, ilon: int, offline: bool) -> Path | None:
    """The cached tile, downloading it if need be.  `None` means the bucket
    has no such tile, which is how it says "all sea here": the product covers
    land only."""
    name = tile_id(ilat, ilon)
    path = DEM_CACHE / f"{name}.tif"
    absent = DEM_CACHE / f"{name}.absent"
    if path.exists():
        return path
    if absent.exists():
        return None
    if offline:
        raise SystemExit(f"no cached DEM tile at {path}")
    DEM_CACHE.mkdir(parents=True, exist_ok=True)
    url = f"{BUCKET}/{name}/{name}.tif"
    print(f"  fetching {url}")
    try:
        with urllib.request.urlopen(url, timeout=300) as r:
            body = r.read()
    except urllib.error.HTTPError as e:
        if e.code in (403, 404):
            # Remembered, so a coastal circuit does not ask the bucket about
            # the same stretch of sea on every run.
            absent.write_text("no such tile in the bucket\n", encoding="utf-8")
            return None
        raise
    # Written aside and renamed, so an interrupted download cannot leave a
    # half tile in the cache for the next run to read as real ground.
    tmp = path.with_suffix(".part")
    tmp.write_bytes(body)
    tmp.replace(path)
    return path


# ----------------------------------------------------------------- GeoTIFF


_TIFF_FMT = {1: "B", 3: "H", 4: "I", 11: "f", 12: "d"}
_TIFF_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 11: 4, 12: 8}


def _tiff_tags(buf: bytes, bo: str, off: int) -> dict:
    """One image file directory as {tag: value tuple}.  A value of four bytes
    or fewer sits in the entry itself; anything longer is a file offset."""
    n = struct.unpack(bo + "H", buf[off : off + 2])[0]
    tags: dict[int, object] = {}
    for i in range(n):
        tag, typ, cnt = struct.unpack(bo + "HHI", buf[off + 2 + i * 12 : off + 10 + i * 12])
        raw = buf[off + 10 + i * 12 : off + 14 + i * 12]
        size = _TIFF_SIZE.get(typ)
        if size is None:
            continue
        total = size * cnt
        if total > 4:
            at = struct.unpack(bo + "I", raw)[0]
            raw = buf[at : at + total]
        else:
            raw = raw[:total]
        if typ == 2:
            tags[tag] = raw.split(b"\0")[0].decode("ascii", "replace")
        elif typ == 5:
            v = struct.unpack(bo + "I" * (2 * cnt), raw)
            tags[tag] = tuple(
                v[j] / v[j + 1] if v[j + 1] else 0.0 for j in range(0, len(v), 2)
            )
        else:
            tags[tag] = struct.unpack(bo + _TIFF_FMT[typ] * cnt, raw)
    return tags


def _geo_key(tags: dict, key: int) -> int | None:
    """One key out of the GeoTIFF key directory, which is a flat array of
    (key, location, count, value) quads hidden inside tag 34735."""
    d = tags.get(34735)
    if not d:
        return None
    for i in range(4, len(d) - 3, 4):
        if d[i] == key and d[i + 1] == 0:
            return int(d[i + 3])
    return None


def _read_tile_tiff(path: Path):
    """A reader for exactly the profile these tiles use: one band of tiled,
    deflated, floating-point-predicted f32.

    Writing it beats adding rasterio -- and with it GDAL -- to a repo whose
    Python side is numpy and PyYAML and nothing else.  `read_tile` still
    prefers rasterio where it happens to be installed, and anything this
    reader does not recognise says so and points at it."""

    def need(ok, what: str):
        if not ok:
            raise SystemExit(
                f"{path.name}: {what}. This reader only handles the Copernicus "
                "COG profile; install rasterio (pip install rasterio) and rerun."
            )

    buf = path.read_bytes()
    need(buf[:2] in (b"II", b"MM"), "not a TIFF")
    bo = "<" if buf[:2] == b"II" else ">"
    need(struct.unpack(bo + "H", buf[2:4])[0] == 42, "BigTIFF, not classic TIFF")
    tags = _tiff_tags(buf, bo, struct.unpack(bo + "I", buf[4:8])[0])
    width, height = tags[256][0], tags[257][0]
    need(tags.get(277, (1,))[0] == 1, "more than one sample per pixel")
    need(tags.get(258, (0,))[0] == 32 and tags.get(339, (1,))[0] == 3, "not 32-bit float")
    need(322 in tags and 323 in tags, "stripped rather than tiled")
    comp = tags.get(259, (1,))[0]
    need(comp in (1, 8, 32946), f"compression {comp} is neither none nor deflate")
    predictor = tags.get(317, (1,))[0]
    need(predictor in (1, 3), f"predictor {predictor} is neither none nor floating point")

    tw, th = tags[322][0], tags[323][0]
    offsets, counts = tags[324], tags[325]
    across = (width + tw - 1) // tw
    out = np.full((height, width), np.nan, np.float32)
    for k, (at, count) in enumerate(zip(offsets, counts)):
        blob = buf[at : at + count]
        if comp in (8, 32946):
            blob = zlib.decompress(blob)
        arr = np.frombuffer(blob, np.uint8)
        need(arr.size == th * tw * 4, f"tile {k} is {arr.size} bytes, not {th * tw * 4}")
        arr = arr.reshape(th, tw * 4)
        if predictor == 3:
            # The floating-point predictor stores a row as byte planes of
            # first differences: undo the differences along the row, then
            # re-interleave the planes, most significant byte first.
            arr = np.cumsum(arr, axis=1, dtype=np.uint32).astype(np.uint8)
            planes = np.ascontiguousarray(arr.reshape(th, 4, tw).transpose(0, 2, 1))
            vals = planes.reshape(th, tw * 4).view(">f4")
        else:
            vals = np.ascontiguousarray(arr).view(bo + "f4")
        r0, c0 = (k // across) * th, (k % across) * tw
        rr, cc = min(th, height - r0), min(tw, width - c0)
        out[r0 : r0 + rr, c0 : c0 + cc] = vals[:rr, :cc]

    nodata = tags.get(42113)
    if nodata:
        try:
            out[out == np.float32(float(nodata))] = np.nan
        except ValueError:
            pass
    scale, tie = tags[33550], tags[33922]
    lon0, lat0 = tie[3] - tie[0] * scale[0], tie[4] + tie[1] * scale[1]
    if _geo_key(tags, 1025) != 2:
        # RasterPixelIsArea: the tie point is the corner of the first pixel,
        # so its centre is half a post in and down.
        lon0, lat0 = lon0 + scale[0] / 2, lat0 - scale[1] / 2
    return out, float(lon0), float(lat0), float(scale[0]), float(scale[1])


def _read_tile_rasterio(path: Path):
    import rasterio

    with rasterio.open(path) as src:
        band = src.read(1, masked=True).filled(np.nan).astype(np.float32)
        lon0, lat0 = src.xy(0, 0)  # the centre of the first post
        return band, float(lon0), float(lat0), float(src.transform.a), float(-src.transform.e)


def read_tile(path: Path):
    """`(heights, lon and lat of the first post, degrees per post east and
    south)`.  Rasterio when it is installed, because it reads every COG there
    is; the reader above otherwise."""
    try:
        import rasterio  # noqa: F401
    except ImportError:
        return _read_tile_tiff(path)
    return _read_tile_rasterio(path)


# -------------------------------------------------------------- the mosaic


class Dem:
    """Every degree tile touching one window, pasted into a single grid of
    posts, with bilinear lookup by longitude and latitude.

    The lattice is taken from the first tile read rather than assumed: post
    centres sit at whole multiples of the tile's own spacing measured from
    the meridian and the pole, so tiles of the same latitude band paste in by
    integer index arithmetic and a query straddling a seam interpolates
    across it without noticing.  A window that crosses a band boundary (a
    circuit within 8 km of 50 degrees north) gets the other band's tiles
    resampled onto the lattice, which is worth up to half a post of longitude
    error at that one seam and nothing anywhere else."""

    def __init__(self, lon_lo, lon_hi, lat_lo, lat_hi, offline: bool):
        self.window = (lon_lo, lon_hi, lat_lo, lat_hi)
        self.grid = None
        self.tiles: list[str] = []
        self.absent: list[str] = []
        for ilat in range(int(math.floor(lat_lo)), int(math.floor(lat_hi)) + 1):
            for ilon in range(int(math.floor(lon_lo)), int(math.floor(lon_hi)) + 1):
                path = fetch_tile(ilat, ilon, offline)
                if path is None:
                    self.absent.append(tile_id(ilat, ilon))
                    continue
                self.tiles.append(tile_id(ilat, ilon))
                self._paste(path, ilat, ilon)
        if self.grid is None:
            raise SystemExit(
                "the bucket has no Copernicus tile anywhere over this circuit "
                "- check the fit before believing that"
            )

    def _allocate(self, dlon: float, dlat: float):
        self.lon_px = int(round(1.0 / dlon))
        self.lat_px = int(round(1.0 / dlat))
        lon_lo, lon_hi, lat_lo, lat_hi = self.window
        # A post of slack all round, so a lookup at the very edge of the
        # window still has its four corners.
        self.c0 = int(math.floor((lon_lo + 180.0) * self.lon_px)) - 1
        self.r0 = int(math.floor((90.0 - lat_hi) * self.lat_px)) - 1
        cols = int(math.ceil((lon_hi + 180.0) * self.lon_px)) + 2 - self.c0
        rows = int(math.ceil((90.0 - lat_lo) * self.lat_px)) + 2 - self.r0
        self.grid = np.full((rows, cols), np.nan, np.float32)

    def _paste(self, path: Path, ilat: int, ilon: int):
        band, lon0, lat0, dlon, dlat = read_tile(path)
        # The tile's own header has to agree with where its name says it is,
        # to within half a post, and its spacing has to divide a degree; one
        # that does neither is not this product.
        if (
            abs(1.0 / dlon - round(1.0 / dlon)) > 1e-6
            or abs(1.0 / dlat - round(1.0 / dlat)) > 1e-6
            or abs(lon0 - ilon) > 0.5 * dlon
            or abs(lat0 - (ilat + 1)) > 0.5 * dlat
        ):
            raise SystemExit(
                f"{path.name}: header puts the first post at {lon0:.6f},{lat0:.6f} "
                f"at {dlon:.9f} x {dlat:.9f} deg, its name says {ilon},{ilat + 1}"
            )
        if self.grid is None:
            self._allocate(dlon, dlat)
        if int(round(1.0 / dlat)) != self.lat_px:
            raise SystemExit(f"{path.name}: latitude spacing differs across the window")

        h, w = band.shape
        gr = int(round((90.0 - lat0) * self.lat_px)) - self.r0
        r0, r1 = max(gr, 0), min(gr + h, self.grid.shape[0])
        if r1 <= r0:
            return
        if int(round(1.0 / dlon)) == self.lon_px:
            gc = int(round((lon0 + 180.0) * self.lon_px)) - self.c0
            c0, c1 = max(gc, 0), min(gc + w, self.grid.shape[1])
            if c1 <= c0:
                return
            self.grid[r0:r1, c0:c1] = band[r0 - gr : r1 - gr, c0 - gc : c1 - gc]
            return
        # Another latitude band: the rows still line up, so only longitude is
        # resampled, and only over the columns this tile actually covers.
        lon_hi = lon0 + (w - 1) * dlon
        c0 = max(int(math.ceil((lon0 + 180.0) * self.lon_px)) - self.c0, 0)
        c1 = min(int(math.floor((lon_hi + 180.0) * self.lon_px)) - self.c0 + 1, self.grid.shape[1])
        if c1 <= c0:
            return
        lon_t = (np.arange(c0, c1) + self.c0) / self.lon_px - 180.0
        f = (lon_t - lon0) / dlon
        i0 = np.clip(np.floor(f), 0, w - 2).astype(int)
        f = np.clip(f - i0, 0.0, 1.0)
        rows = band[r0 - gr : r1 - gr]
        self.grid[r0:r1, c0:c1] = rows[:, i0] * (1 - f) + rows[:, i0 + 1] * f

    def sample(self, lon, lat) -> np.ndarray:
        """Bilinear height at each (lon, lat), NaN where no tile covers it."""
        fc = (np.asarray(lon, dtype=float) + 180.0) * self.lon_px - self.c0
        fr = (90.0 - np.asarray(lat, dtype=float)) * self.lat_px - self.r0
        c = np.clip(np.floor(fc), 0, self.grid.shape[1] - 2).astype(int)
        r = np.clip(np.floor(fr), 0, self.grid.shape[0] - 2).astype(int)
        u = np.clip(fc - c, 0.0, 1.0)
        v = np.clip(fr - r, 0.0, 1.0)
        g = self.grid
        top = g[r, c] * (1 - u) + g[r, c + 1] * u
        bottom = g[r + 1, c] * (1 - u) + g[r + 1, c + 1] * u
        return top * (1 - v) + bottom * v


class Frame:
    """WGS-84 and the track frame, through the dossier's own fit.

    `fit_track` gives the fit as `track = osm_enu @ a.T + t`, where the ENU
    frame is the extract's own flat-earth projection about its centre; `a` is
    a rotation, so the way back is a transpose rather than a solve."""

    def __init__(self, osm, a, t):
        self.lon0, self.lat0 = osm.lon0, osm.lat0
        self.a, self.t = a, t
        self.m_per_deg_lon = math.radians(1.0) * R_EARTH * math.cos(math.radians(self.lat0))
        self.m_per_deg_lat = math.radians(1.0) * R_EARTH

    def to_lonlat(self, xy):
        e = (np.asarray(xy, dtype=float).reshape(-1, 2) - self.t) @ self.a
        return (
            self.lon0 + e[:, 0] / self.m_per_deg_lon,
            self.lat0 + e[:, 1] / self.m_per_deg_lat,
        )


# ---------------------------------------------------------------- the grids


def grid_spec(pts: np.ndarray, margin_m: float, cell_m: float) -> dict:
    """A grid covering the road's bounding box plus a margin, its origin
    snapped to a whole number of cells so a circuit's two grids -- and a
    circuit re-run a year later -- land on the same lattice."""
    lo = np.floor((pts.min(0) - margin_m) / cell_m) * cell_m
    hi = np.ceil((pts.max(0) + margin_m) / cell_m) * cell_m
    return {
        "cell_m": float(cell_m),
        "cols": int(round((hi[0] - lo[0]) / cell_m)) + 1,
        "rows": int(round((hi[1] - lo[1]) / cell_m)) + 1,
        "origin_x": float(lo[0]),
        "origin_y": float(lo[1]),
    }


def sample_grid(dem: Dem, frame: Frame, spec: dict) -> np.ndarray:
    """The DEM under every post of a grid: `heights[row * cols + col]` is the
    ground at `(origin_x + col * cell_m, origin_y + row * cell_m)`, so rows
    run along +Y and the array is row-major as the sidecar stores it."""
    cell = spec["cell_m"]
    xs = spec["origin_x"] + np.arange(spec["cols"]) * cell
    ys = spec["origin_y"] + np.arange(spec["rows"]) * cell
    gx, gy = np.meshgrid(xs, ys)
    lon, lat = frame.to_lonlat(np.column_stack([gx.ravel(), gy.ravel()]))
    return dem.sample(lon, lat).reshape(spec["rows"], spec["cols"])


def centerline_z(track) -> np.ndarray:
    """The YAML's own z at each of `Track`'s resampled stations.  `Track`
    keeps x and y only, and z is the whole point of the comparison this
    script reports."""
    nodes = track.data["nodes"]
    raw = np.array([[n["x"], n["y"]] for n in nodes])
    z = np.array([float(n.get("z") or 0.0) for n in nodes])
    seg = np.hypot(*np.diff(np.vstack([raw, raw[:1]]) if track.closed else raw, axis=0).T)
    s_raw = np.concatenate([[0.0], np.cumsum(seg)])[: len(raw)]
    if track.closed:
        return np.interp(track.station, s_raw, z, period=track.total)
    return np.interp(track.station, s_raw, z)


# --------------------------------------------------------------- msgpack out


class Raw:
    """Bytes that are msgpack already, so a grid's heights can be packed as
    they are stored instead of going through a list of Python floats."""

    __slots__ = ("data",)

    def __init__(self, data: bytes):
        self.data = data


def _header(n: int, fix: int, mid: int, big: int) -> bytes:
    if n < 16:
        return bytes([fix | n])
    if n < 1 << 16:
        return bytes([mid]) + struct.pack(">H", n)
    return bytes([big]) + struct.pack(">I", n)


def pack(v) -> bytes:
    """Just enough msgpack to write this sidecar.

    Hand-rolled rather than msgpack-python because the heights must be f32
    while everything else must not: `use_single_float` is a property of the
    whole packer there, and f32 longitudes would throw away half the
    georeferencing the file records."""
    if isinstance(v, Raw):
        return v.data
    if isinstance(v, bool):
        return b"\xc3" if v else b"\xc2"
    if isinstance(v, int):
        if 0 <= v < 128:
            return bytes([v])
        if -32 <= v < 0:
            return bytes([v & 0xFF])
        return b"\xd3" + struct.pack(">q", v)
    if isinstance(v, float):
        return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        if len(b) < 32:
            return bytes([0xA0 | len(b)]) + b
        if len(b) < 256:
            return b"\xd9" + bytes([len(b)]) + b
        return b"\xda" + struct.pack(">H", len(b)) + b
    if isinstance(v, dict):
        out = [_header(len(v), 0x80, 0xDE, 0xDF)]
        for key, val in v.items():
            out.append(pack(str(key)))
            out.append(pack(val))
        return b"".join(out)
    if isinstance(v, (list, tuple)):
        return _header(len(v), 0x90, 0xDC, 0xDD) + b"".join(pack(x) for x in v)
    raise TypeError(f"cannot pack {type(v).__name__}")


def pack_f32(a: np.ndarray) -> Raw:
    """An array of f32, built as bytes rather than element by element: a
    circuit's inner grid runs to a million posts."""
    flat = np.ascontiguousarray(a.ravel(), dtype=">f4")
    body = np.empty((flat.size, 5), dtype=np.uint8)
    body[:, 0] = 0xCA
    body[:, 1:] = flat.view(np.uint8).reshape(-1, 4)
    return Raw(_header(flat.size, 0x90, 0xDC, 0xDD) + body.tobytes())


# -------------------------------------------------------------------- build


def build(stem: str, offline: bool) -> dict:
    print(f"== {stem}")
    track, osm, a, t, report = fit_track(stem, offline)
    frame = Frame(osm, a, t)

    inner = grid_spec(track.pts, INNER_MARGIN_M, INNER_CELL_M)
    outer = grid_spec(track.pts, OUTER_MARGIN_M, OUTER_CELL_M)
    # The frame is affine, so the outer grid's four corners bound the window
    # the DEM has to cover.
    span = np.array(
        [
            [outer["origin_x"], outer["origin_y"]],
            [outer["origin_x"] + (outer["cols"] - 1) * OUTER_CELL_M, outer["origin_y"]],
            [outer["origin_x"], outer["origin_y"] + (outer["rows"] - 1) * OUTER_CELL_M],
            [
                outer["origin_x"] + (outer["cols"] - 1) * OUTER_CELL_M,
                outer["origin_y"] + (outer["rows"] - 1) * OUTER_CELL_M,
            ],
        ]
    )
    lon, lat = frame.to_lonlat(span)
    dem = Dem(lon.min(), lon.max(), lat.min(), lat.max(), offline)
    print(
        f"   {len(dem.tiles)} tile(s): "
        + ", ".join(n[len("Copernicus_DSM_COG_10_") : -len("_DEM")] for n in dem.tiles)
        + (f" (+{len(dem.absent)} not in the bucket)" if dem.absent else "")
    )

    # Tie the DEM to the track's own datum at the start/finish line: sidecar
    # and YAML then agree there by construction, whatever the YAML's zero was.
    line_lon, line_lat = frame.to_lonlat(track.pts[:1])
    at_line = float(dem.sample(line_lon, line_lat)[0])
    if not math.isfinite(at_line):
        raise SystemExit(f"{stem}: no DEM under the start/finish line")
    yaml_z = centerline_z(track)
    dz = float(yaml_z[0]) - at_line

    grids = {}
    gaps = 0
    for name, spec in (("inner", inner), ("outer", outer)):
        h = sample_grid(dem, frame, spec)
        hole = ~np.isfinite(h)
        # A hole is sea: the bucket holds land tiles only, and the product is
        # zero over the water inside the ones it does hold.
        gaps += int(hole.sum())
        h[hole] = 0.0
        h += dz
        spec["heights"] = pack_f32(h)
        grids[name] = spec
        print(
            f"   {name} {spec['cell_m']:.0f} m: {spec['cols']} x {spec['rows']} posts, "
            f"{float(h.min()):.0f} m to {float(h.max()):.0f} m"
        )
    if gaps:
        print(f"   {gaps} post(s) had no tile and were taken as sea level")

    lon_c, lat_c = frame.to_lonlat(track.pts)
    along = dem.sample(lon_c, lat_c) + dz
    good = np.isfinite(along)
    d = along[good] - yaml_z[good]
    print(
        f"   centerline: dem vs yaml rms {math.sqrt(float((d**2).mean())):.1f} m, "
        f"{float(d.min()):+.1f} m to {float(d.max()):+.1f} m "
        f"(tied at the line, dz {dz:+.1f} m)"
    )
    print(
        f"   yaml z {float(yaml_z.min()):.0f} m to {float(yaml_z.max()):.0f} m, "
        f"dem along the road {float(along[good].min()):.0f} m to "
        f"{float(along[good].max()):.0f} m"
    )

    return {
        "version": DEM_VERSION,
        "source": SOURCE,
        "georef": {
            "lon0": float(osm.lon0),
            "lat0": float(osm.lat0),
            "a": [float(x) for x in a.ravel()],
            "t": [float(x) for x in t],
            "datum_offset_m": dz,
            "fit_rmse_m": float(report["rmse_m"]),
        },
        "inner": grids["inner"],
        "outer": grids["outer"],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("tracks", nargs="*", help="track stems, e.g. Spielberg")
    ap.add_argument("--all", action="store_true", help="every circuit with a dossier")
    ap.add_argument("--offline", action="store_true", help="use the cached tiles only")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    args = ap.parse_args()

    stems = (
        sorted(p.name.split(".")[0] for p in TRACK_DIR.glob("*.layout.json"))
        if args.all
        else args.tracks
    )
    if not stems:
        ap.print_help()
        return 1
    for stem in stems:
        if stem not in BBOXES:
            raise SystemExit(f"no bbox for {stem}; add one to osm_layout.BBOXES")
        doc = build(stem, args.offline)
        if args.dry_run:
            continue
        out = TRACK_DIR / f"{stem}.dem.msgpack"
        out.write_bytes(pack(doc))
        print(f"   wrote {out.relative_to(REPO)} ({out.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

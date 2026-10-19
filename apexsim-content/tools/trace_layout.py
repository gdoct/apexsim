#!/usr/bin/env python3
"""An aerial-imagery tracing aid for the layout dossiers.

Writes one HTML page per circuit, ``build/trace/<Stem>.html``, that lays the
track's centerline, its station marks and its dossier
(``<Stem>.layout.json``: stands, buildings, crossings, landmarks, pit lane,
woods, water, barriers) over aerial imagery, through the same georeference
the dossier was built from (the DEM sidecar's ``georef``, as
``osm_layout.py --manual-only`` uses it; a circuit without a DEM sidecar,
such as Marina Bay, takes the dossier's own fit, ``osm_layout.fit_track``
over the cached OSM extract). Clicking on the imagery traces a
run of points; the panel gives each one as longitude/latitude, as track
``x``/``y``, and as station, lateral offset and side, and writes the trace
out as the manual entries ``osm_layout.py`` takes: a ``MANUAL_STANDS`` span
(``from_m`` / ``to_m`` / ``side`` / ``gap_m``), a ``(lon, lat)`` anchor for
``MANUAL_LANDMARKS`` / ``MANUAL_CROSSINGS``, or a ring in either frame.

    python tools/trace_layout.py Spa            # build/trace/Spa.html
    python tools/trace_layout.py --all          # every circuit with a georeference
    python tools/trace_layout.py Spa --open     # and open it in the browser

The page loads Leaflet and the imagery tiles (Esri World Imagery, with
OpenStreetMap as the other base layer) from the web when opened; nothing
here fetches anything, except a circuit without a DEM sidecar whose OSM
extract is not cached yet (``.cache/osm``): its fit fetches the extract.
The page reads the dossier, it never writes one: what is traced goes into the
script's manual tables by hand, and ``osm_layout.py <Stem> --manual-only``
(or a refetch) lays it into the dossier.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import webbrowser
from pathlib import Path

import msgpack
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from osm_layout import R_EARTH, Track  # noqa: E402
from track_dirs import track_dir, track_glob  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "build" / "trace"
LEAFLET_VERSION = "1.9.4"


class Georef:
    """The DEM sidecar's fit: a point in the extract's ENU frame lands in
    the track frame at ``p @ a.T + t``; this class goes both ways."""

    def __init__(self, georef: dict):
        self.a = np.array(georef["a"], dtype=float).reshape(2, 2)
        self.t = np.array(georef["t"], dtype=float)
        self.lon0 = float(georef["lon0"])
        self.lat0 = float(georef["lat0"])
        self.inv = np.linalg.inv(self.a)

    def to_lonlat(self, xy) -> np.ndarray:
        xy = np.asarray(xy, dtype=float).reshape(-1, 2)
        enu = (xy - self.t) @ self.inv.T
        lon = self.lon0 + np.degrees(enu[:, 0] / (R_EARTH * math.cos(math.radians(self.lat0))))
        lat = self.lat0 + np.degrees(enu[:, 1] / R_EARTH)
        return np.column_stack([lon, lat])


def load_georef(stem: str) -> Georef | None:
    """The DEM sidecar's georeference, else the dossier's own fit (the one
    ``dem_fetch.py`` would record), else none."""
    dem_path = track_dir(stem) / f"{stem}.dem.msgpack"
    if dem_path.exists():
        return Georef(msgpack.unpackb(dem_path.read_bytes(), raw=False)["georef"])
    import osm_layout

    if stem not in osm_layout.BBOXES:
        return None
    print(f"{stem}: no DEM sidecar; fitting the OSM extract as the dossier did")
    offline = all((osm_layout.CACHE_DIR / f"{stem}.{i}.json").exists() for i in range(len(osm_layout.BBOXES[stem])))
    try:
        _, osm, a, t, _ = osm_layout.fit_track(stem, offline)
    except SystemExit as e:
        print(f"{stem}: no fit ({e})")
        return None
    return Georef({"a": a.ravel().tolist(), "t": t.tolist(), "lon0": osm.lon0, "lat0": osm.lat0})


def latlon_list(geo: Georef, xy) -> list[list[float]]:
    """Leaflet's [lat, lon] pairs, rounded to a centimetre or so."""
    ll = geo.to_lonlat(xy)
    return [[round(float(lat), 7), round(float(lon), 7)] for lon, lat in ll]


def rect_ring(centre, length_m: float, depth_m: float, yaw_rad: float) -> np.ndarray:
    """The four corners of a footprint centred on `centre`, its length along the yaw."""
    c, s = math.cos(yaw_rad), math.sin(yaw_rad)
    hl, hd = length_m / 2.0, depth_m / 2.0
    corners = np.array([[-hl, -hd], [hl, -hd], [hl, hd], [-hl, hd]])
    rot = np.array([[c, -s], [s, c]])
    return corners @ rot.T + np.asarray(centre, dtype=float)


def page_data(stem: str, track: Track, geo: Georef, layout: dict) -> dict:
    """Everything the page draws and computes with, in Leaflet's frame."""
    # The centerline every sample (about 2 m), with station, half widths
    # and heading, so the page can locate a click the way the script does.
    line = latlon_list(geo, track.pts)
    corners = []
    for corner in layout.get("corners", []):
        i = track.index_at(float(corner["station_m"]))
        corners.append(
            {
                "name": corner.get("display_name") or corner.get("name") or "",
                "station_m": corner["station_m"],
                "at": line[i],
            }
        )
    stands = []
    for stand in layout.get("stands", []):
        front = stand.get("front") or []
        entry = {
            "name": stand.get("name") or "(unnamed)",
            "source": stand.get("source", ""),
            "station_m": stand.get("station_m"),
            "side": stand.get("side"),
            "length_m": stand.get("length_m"),
            "depth_m": stand.get("depth_m"),
        }
        if len(front) >= 2:
            entry["front"] = latlon_list(geo, front)
        if stand.get("centre") and stand.get("length_m") and stand.get("depth_m"):
            entry["ring"] = latlon_list(
                geo, rect_ring(stand["centre"], stand["length_m"], stand["depth_m"], stand.get("yaw_rad", 0.0))
            )
        stands.append(entry)
    structures = []
    for st in layout.get("structures", []):
        if not (st.get("centre") and st.get("length_m") and st.get("depth_m")):
            continue
        structures.append(
            {
                "name": st.get("name") or st.get("osm_building") or "(unnamed)",
                "station_m": st.get("station_m"),
                "side": st.get("side"),
                "ring": latlon_list(
                    geo, rect_ring(st["centre"], st["length_m"], st["depth_m"], st.get("yaw_rad", 0.0))
                ),
            }
        )
    crossings = []
    for cr in layout.get("crossings", []):
        i = track.index_at(float(cr["station_m"]))
        crossings.append(
            {"name": cr.get("name") or cr.get("kind") or "crossing", "kind": cr.get("kind", ""),
             "station_m": cr["station_m"], "at": line[i]}
        )
    landmarks = [
        {"name": lm.get("name") or lm.get("kind"), "kind": lm.get("kind", ""), "station_m": lm.get("station_m"),
         "at": latlon_list(geo, [lm["centre"]])[0]}
        for lm in layout.get("landmarks", [])
        if lm.get("centre")
    ]
    pit = layout.get("pit_lane") or {}
    pit_lane = latlon_list(geo, pit["nodes"]) if pit.get("nodes") else []
    rings = lambda key, field: [  # noqa: E731
        {"name": e.get("name") or e.get("kind") or key, "kind": e.get(field, ""), "ring": latlon_list(geo, e["ring"])}
        for e in layout.get(key, [])
        if e.get("ring")
    ]
    woods = rings("woods", "leaf")
    areas = rings("areas", "kind")
    water = [
        {"name": w.get("name") or "water", "rings": [latlon_list(geo, r) for r in w.get("rings", []) if r]}
        for w in layout.get("water", [])
    ]
    lines = [
        {"kind": b.get("kind", ""), "line": latlon_list(geo, b["line"])}
        for b in layout.get("barriers", [])
        if len(b.get("line") or []) >= 2
    ]
    return {
        "stem": stem,
        "display_name": layout.get("track_display_name") or track.data.get("display_name") or stem,
        "fit": layout.get("fit", {}),
        "georef": {"a": geo.a.ravel().tolist(), "t": geo.t.tolist(), "lon0": geo.lon0, "lat0": geo.lat0, "r": R_EARTH},
        "line": line,
        "station": [round(float(s), 2) for s in track.station],
        "heading": [round(float(h), 5) for h in track.heading],
        "width_left": [round(float(w), 2) for w in track.width_left],
        "width_right": [round(float(w), 2) for w in track.width_right],
        "total_m": round(track.total, 1),
        "closed": track.closed,
        "corners": corners,
        "stands": stands,
        "structures": structures,
        "crossings": crossings,
        "landmarks": landmarks,
        "pit_lane": pit_lane,
        "pit_side": pit.get("side", ""),
        "woods": woods,
        "areas": areas,
        "water": water,
        "barriers": lines,
    }


HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@__LEAFLET__/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@__LEAFLET__/dist/leaflet.js"></script>
<style>
  html, body { margin: 0; height: 100%; font: 13px/1.4 system-ui, sans-serif; background: #111; color: #ddd; }
  #map { position: absolute; inset: 0 360px 0 0; }
  #panel { position: absolute; top: 0; right: 0; bottom: 0; width: 360px; box-sizing: border-box; padding: 12px; overflow: auto; background: #181818; border-left: 1px solid #333; }
  h1 { font-size: 15px; margin: 0 0 4px; } h2 { font-size: 12px; margin: 14px 0 4px; color: #9ac; text-transform: uppercase; letter-spacing: .05em; }
  .muted { color: #888; } code, textarea, input { font: 12px/1.4 ui-monospace, Consolas, monospace; }
  textarea { width: 100%; box-sizing: border-box; height: 120px; background: #0e0e0e; color: #cde; border: 1px solid #333; padding: 6px; }
  input[type=text], input[type=number] { background: #0e0e0e; color: #eee; border: 1px solid #333; padding: 3px 6px; width: 110px; }
  button { background: #2a3a4a; color: #eee; border: 1px solid #456; padding: 4px 10px; margin: 2px 4px 2px 0; cursor: pointer; } button:hover { background: #3a4a5a; }
  table { border-collapse: collapse; width: 100%; } td { padding: 1px 4px; border-bottom: 1px solid #262626; vertical-align: top; } td:first-child { color: #9ac; white-space: nowrap; }
  .station-label { background: rgba(0,0,0,.55); color: #ffe27a; border: 0; box-shadow: none; font: 11px ui-monospace, monospace; padding: 0 3px; }
  .corner-label { background: rgba(0,0,0,.6); color: #fff; border: 0; box-shadow: none; font: 12px system-ui; padding: 1px 4px; }
  .leaflet-container { background: #000; }
</style>
</head>
<body>
<div id="map"></div>
<div id="panel">
  <h1>__TITLE__</h1>
  <div class="muted">lap <span id="total"></span> m · fit rmse <span id="rmse"></span> m · click the imagery to trace; the dossier is drawn, never written</div>
  <h2>Go to</h2>
  <div>station <input type="number" id="goto" step="10" min="0"> m <button id="goto-btn">GO</button> <button id="fit-btn">WHOLE LAP</button></div>
  <h2>Trace</h2>
  <div>name <input type="text" id="name" placeholder="Tribune ..."> depth <input type="number" id="depth" value="14" step="1" style="width:60px"> m</div>
  <div style="margin-top:4px"><button id="undo">UNDO</button><button id="clear">CLEAR</button><button id="copy">COPY SNIPPET</button></div>
  <table id="points"></table>
  <h2>As a manual entry</h2>
  <textarea id="snippet" readonly></textarea>
  <h2>Layers</h2>
  <div class="muted">toggle them top right. Stands are amber (OSM) or orange (manual), buildings blue, pit lane magenta, crossings green marks, landmarks white, woods dark green, water blue, barriers thin grey. Station marks every 100 m, labelled every 500 m.</div>
  <h2>Last click</h2>
  <table id="last"></table>
</div>
<script>
const D = __DATA__;
const g = D.georef;
const A = g.a, T = g.t, cosLat0 = Math.cos(g.lat0 * Math.PI / 180);
// lon/lat -> ENU -> track frame, the fit forward, as osm_layout.py applies it.
function toTrack(lat, lon) {
  const e = (lon - g.lon0) * Math.PI / 180 * g.r * cosLat0;
  const n = (lat - g.lat0) * Math.PI / 180 * g.r;
  return [A[0] * e + A[1] * n + T[0], A[2] * e + A[3] * n + T[1]];
}
// Track frame samples once, for the nearest-point search.
const XY = D.line.map(([lat, lon]) => toTrack(lat, lon));
function locate(x, y) {
  let best = 0, bd = Infinity;
  for (let i = 0; i < XY.length; i++) {
    const dx = XY[i][0] - x, dy = XY[i][1] - y, d = dx * dx + dy * dy;
    if (d < bd) { bd = d; best = i; }
  }
  const h = D.heading[best], dx = x - XY[best][0], dy = y - XY[best][1];
  const lat = -Math.sin(h) * dx + Math.cos(h) * dy;
  const along = Math.cos(h) * dx + Math.sin(h) * dy;
  let s = D.station[best] + along;
  if (D.closed) s = ((s % D.total_m) + D.total_m) % D.total_m;
  const side = lat >= 0 ? 'left' : 'right';
  const half = side === 'left' ? D.width_left[best] : D.width_right[best];
  return { station: s, lateral: lat, side, gap: Math.abs(lat) - half, index: best };
}

const map = L.map('map', { zoomControl: true });
const imagery = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
  maxZoom: 20, maxNativeZoom: 19, attribution: 'Imagery: Esri, Maxar, Earthstar Geographics, and the GIS User Community' }).addTo(map);
const osm = L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 19, attribution: '&copy; OpenStreetMap contributors' });

const centerline = L.polyline(D.line, { color: '#ff3b3b', weight: 2.5, opacity: 0.9 });
const marks = L.layerGroup();
for (let s = 0; s < D.total_m; s += 100) {
  let i = 0; while (i < D.station.length - 1 && D.station[i] < s) i++;
  const big = s % 500 === 0;
  const m = L.circleMarker(D.line[i], { radius: big ? 4 : 2.5, color: '#ffe27a', weight: 1, fillOpacity: 0.9 });
  m.bindTooltip(s + ' m', { permanent: big, direction: 'right', className: 'station-label' });
  marks.addLayer(m);
}
const corners = L.layerGroup(D.corners.map(c => L.marker(c.at, { opacity: 0 }).bindTooltip(c.name + ' · ' + c.station_m + ' m', { permanent: true, direction: 'top', className: 'corner-label' })));
const stands = L.layerGroup();
D.stands.forEach(s => {
  const manual = s.source && s.source !== 'osm';
  const colour = manual ? '#ff8c1a' : '#ffbf47';
  const tip = `${s.name} · ${s.side} · ${s.station_m} m · ${s.length_m} x ${s.depth_m} m${s.source ? ' · ' + s.source : ''}`;
  if (s.ring) stands.addLayer(L.polygon(s.ring, { color: colour, weight: 1, fillOpacity: 0.15 }).bindTooltip(tip));
  if (s.front) stands.addLayer(L.polyline(s.front, { color: colour, weight: 3 }).bindTooltip(tip));
});
const structures = L.layerGroup(D.structures.map(s => L.polygon(s.ring, { color: '#4aa3ff', weight: 1, fillOpacity: 0.15 }).bindTooltip(`${s.name} · ${s.side} · ${s.station_m} m`)));
const pit = L.layerGroup(D.pit_lane.length ? [L.polyline(D.pit_lane, { color: '#ff4bd8', weight: 3, dashArray: '6 4' }).bindTooltip('pit lane · ' + D.pit_side)] : []);
const crossings = L.layerGroup(D.crossings.map(c => L.circleMarker(c.at, { radius: 6, color: '#3ddc84', weight: 2, fillOpacity: 0.3 }).bindTooltip(`${c.name} (${c.kind}) · ${c.station_m} m`)));
const landmarks = L.layerGroup(D.landmarks.map(l => L.circleMarker(l.at, { radius: 6, color: '#fff', weight: 2, fillOpacity: 0.2 }).bindTooltip(`${l.name} (${l.kind}) · ${l.station_m} m`)));
const woods = L.layerGroup(D.woods.map(w => L.polygon(w.ring, { color: '#2e8b3a', weight: 1, fillOpacity: 0.12 }).bindTooltip(`${w.name} · ${w.kind}`)));
const areas = L.layerGroup(D.areas.map(a => L.polygon(a.ring, { color: '#b9b9b9', weight: 1, fillOpacity: 0.05, dashArray: '3 3' }).bindTooltip(`${a.name} · ${a.kind}`)));
const water = L.layerGroup(D.water.flatMap(w => w.rings.map(r => L.polygon(r, { color: '#3fa7ff', weight: 1, fillOpacity: 0.2 }).bindTooltip(w.name))));
const barriers = L.layerGroup(D.barriers.map(b => L.polyline(b.line, { color: '#aaa', weight: 1 }).bindTooltip(b.kind)));
[centerline, marks, corners, stands, structures, pit, crossings, landmarks, woods, water].forEach(l => l.addTo(map));
L.control.layers({ 'Imagery': imagery, 'OpenStreetMap': osm }, {
  'Centerline': centerline, 'Station marks': marks, 'Corners': corners, 'Stands': stands, 'Buildings': structures,
  'Pit lane': pit, 'Crossings': crossings, 'Landmarks': landmarks, 'Woods': woods, 'Areas': areas, 'Water': water, 'Barriers': barriers,
}, { collapsed: false }).addTo(map);
map.fitBounds(centerline.getBounds());
L.control.scale({ imperial: false }).addTo(map);

document.getElementById('total').textContent = D.total_m;
document.getElementById('rmse').textContent = D.fit.rmse_m ?? '?';
document.getElementById('fit-btn').onclick = () => map.fitBounds(centerline.getBounds());
document.getElementById('goto-btn').onclick = () => {
  const s = parseFloat(document.getElementById('goto').value);
  if (!isFinite(s)) return;
  let i = 0; while (i < D.station.length - 1 && D.station[i] < s) i++;
  map.setView(D.line[i], Math.max(map.getZoom(), 18));
};

// --- the trace -------------------------------------------------------------
const trace = [];
const traceLine = L.polyline([], { color: '#00e5ff', weight: 3 }).addTo(map);
const traceDots = L.layerGroup().addTo(map);
const fmt = (v, d) => (Math.round(v * Math.pow(10, d)) / Math.pow(10, d)).toFixed(d);
function render() {
  traceLine.setLatLngs(trace.map(p => [p.lat, p.lon]));
  traceDots.clearLayers();
  trace.forEach((p, i) => traceDots.addLayer(L.circleMarker([p.lat, p.lon], { radius: 5, color: '#00e5ff', fillOpacity: 0.8 }).bindTooltip(`#${i + 1} · ${fmt(p.loc.station, 0)} m · ${p.loc.side} ${fmt(Math.abs(p.loc.lateral), 1)} m`)));
  const rows = trace.map((p, i) => `<tr><td>#${i + 1}</td><td>${fmt(p.loc.station, 0)} m · ${p.loc.side} ${fmt(Math.abs(p.loc.lateral), 1)} m (gap ${fmt(p.loc.gap, 1)})<br><span class="muted">x ${fmt(p.xy[0], 1)} y ${fmt(p.xy[1], 1)} · ${fmt(p.lon, 6)}, ${fmt(p.lat, 6)}</span></td></tr>`);
  document.getElementById('points').innerHTML = rows.join('');
  document.getElementById('snippet').value = snippet();
}
function snippet() {
  if (!trace.length) return '# click the imagery: one point is an anchor, two or more a span or a ring';
  const name = document.getElementById('name').value || 'Name me';
  const depth = parseFloat(document.getElementById('depth').value) || 14;
  const lines = [];
  if (trace.length === 1) {
    const p = trace[0];
    lines.push(`# MANUAL_LANDMARKS / MANUAL_CROSSINGS anchor, station ${fmt(p.loc.station, 0)} m, ${p.loc.side}`);
    lines.push(`dict(kind="...", name="${name}", at=(${fmt(p.lon, 6)}, ${fmt(p.lat, 6)})),`);
    lines.push(`# or by station: dict(name="${name}", station_m=${fmt(p.loc.station, 0)}, kind="footbridge"),`);
    return lines.join('\n');
  }
  // A span along the road: the stations of the first and last point, in
  // lap order, the side most points fall on and the gap from the road edge.
  const first = trace[0].loc, last = trace[trace.length - 1].loc;
  let from = Math.min(first.station, last.station), to = Math.max(first.station, last.station);
  if (D.closed && to - from > D.total_m / 2) { [from, to] = [to, from]; }
  const left = trace.filter(p => p.loc.side === 'left').length;
  const side = left * 2 >= trace.length ? 'left' : 'right';
  const gap = Math.max(0, Math.min(...trace.map(p => p.loc.gap)));
  lines.push(`# MANUAL_STANDS span (the front is laid along the centerline between the stations)`);
  lines.push(`dict(name="${name}", from_m=${fmt(from, 0)}, to_m=${fmt(to, 0)}, side="${side}", depth_m=${depth}, gap_m=${fmt(gap, 0)}),`);
  if (trace.length >= 3) {
    lines.push(`# the same points as a ring, track frame [x, y] (MANUAL_WOODS / MANUAL_STRUCTURES)`);
    lines.push('[' + trace.map(p => `[${fmt(p.xy[0], 1)}, ${fmt(p.xy[1], 1)}]`).join(', ') + ']');
    lines.push(`# and as (lon, lat)`);
    lines.push('[' + trace.map(p => `(${fmt(p.lon, 6)}, ${fmt(p.lat, 6)})`).join(', ') + ']');
  }
  return lines.join('\n');
}
map.on('click', e => {
  const xy = toTrack(e.latlng.lat, e.latlng.lng);
  const loc = locate(xy[0], xy[1]);
  trace.push({ lat: e.latlng.lat, lon: e.latlng.lng, xy, loc });
  document.getElementById('last').innerHTML = `<tr><td>station</td><td>${fmt(loc.station, 1)} m</td></tr><tr><td>side</td><td>${loc.side}, ${fmt(Math.abs(loc.lateral), 1)} m off the centerline, ${fmt(loc.gap, 1)} m off the road edge</td></tr><tr><td>track x, y</td><td>${fmt(xy[0], 2)}, ${fmt(xy[1], 2)}</td></tr><tr><td>lon, lat</td><td>${fmt(e.latlng.lng, 6)}, ${fmt(e.latlng.lat, 6)}</td></tr>`;
  render();
});
document.getElementById('undo').onclick = () => { trace.pop(); render(); };
document.getElementById('clear').onclick = () => { trace.length = 0; render(); };
document.getElementById('copy').onclick = () => navigator.clipboard && navigator.clipboard.writeText(document.getElementById('snippet').value);
['name', 'depth'].forEach(id => document.getElementById(id).addEventListener('input', () => { document.getElementById('snippet').value = snippet(); }));
render();
</script>
</body>
</html>
"""


def build_page(stem: str) -> Path | None:
    layout_path = track_dir(stem) / f"{stem}.layout.json"
    if not layout_path.exists():
        print(f"{stem}: no dossier ({layout_path.name}); the page needs one to draw")
        return None
    geo = load_georef(stem)
    if geo is None:
        print(f"{stem}: no georeference (no DEM sidecar and no OSM fit); skipped")
        return None
    track = Track(stem)
    layout = json.loads(layout_path.read_text(encoding="utf-8"))
    data = page_data(stem, track, geo, layout)
    title = f"{data['display_name']} · layout trace"
    html = (
        HTML.replace("__TITLE__", title)
        .replace("__LEAFLET__", LEAFLET_VERSION)
        .replace("__DATA__", json.dumps(data, separators=(",", ":")))
    )
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUT_DIR / f"{stem}.html"
    out.write_text(html, encoding="utf-8")
    print(
        f"{stem}: {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KB): "
        f"{len(data['stands'])} stands, {len(data['structures'])} buildings, "
        f"{len(data['crossings'])} crossings, {len(data['landmarks'])} landmarks"
    )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("stems", nargs="*", help="circuit stems (tracks/default/<Stem>)")
    ap.add_argument("--all", action="store_true", help="every circuit with a dossier")
    ap.add_argument("--open", action="store_true", help="open each page in the browser")
    args = ap.parse_args()
    stems = list(args.stems)
    if args.all:
        stems += sorted(
            p.parent.name
            for p in track_glob("*.layout.json")
            if p.parent.name not in stems
        )
    if not stems:
        ap.error("name a circuit or pass --all")
    written = 0
    for stem in stems:
        out = build_page(stem)
        if out is None:
            continue
        written += 1
        if args.open:
            webbrowser.open(out.resolve().as_uri())
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())

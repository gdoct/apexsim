"""Where an imported circuit is: its height above sea level, its latitude
and longitude, and true north in its frame.

AC stores none of this reliably. `ui_track.json`'s `geotags` are the
placeholder `["lat", "lon"]` in most tracks, Kunos' own included; nothing
in a track names its height; and `data/lighting.ini`'s `SUN_HEADING_ANGLE`
is where AC's own fixed sun path stands, not a compass (Silverstone's is
45, Monza's -150, with both tracks modelled north-up). So the place comes,
in order, from:

1. `KNOWN_SITES`, AC's own circuits by folder name: the position and
   height of the shipped circuit of the same place (its YAML's
   `track_location.py` figures) or, for a circuit ApexSim does not ship, the
   Copernicus height at its start line; and true north measured by fitting
   the AC AI line onto the shipped circuit's centerline, whose DEM
   georeference knows north.
2. `geotags`, when the mod filled them in, for the latitude and longitude,
   and the Copernicus DEM (`tools/dem_fetch.py`, its tile cache under
   `.cache/dem`, downloaded unless `dem="cache"`) for the height there.
3. Otherwise no height (the server's sea-level air) and no position (the
   server's 50 N).

North defaults to AC's own convention: Kunos model a circuit with -z (the
ApexSim frame's +Y before the start-line yaw) towards true north. Measured
against the shipped circuits, Silverstone, Monza, Spa, the Red Bull Ring,
Barcelona, Brands Hatch and the Nordschleife sit within 3 degrees of it,
as do the Le Mans mod; Zandvoort (292), the Nurburgring GP (135) and the
Interlagos, Suzuka and Bahrain mods do not, and are in the table with
their measured figure. An unknown mod gets the convention, which may be
wrong for it; the report says which source each figure came from.
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from pathlib import Path

#: True north's yaw in the Z-up world (`frame.ac_to_world`), degrees
#: counter-clockwise from +X: AC's -z.
AC_NORTH_DEG = 90.0


@dataclass(frozen=True)
class Site:
    latitude_deg: float | None
    longitude_deg: float | None
    altitude_m: float | None
    #: Measured north in the Z-up world, degrees CCW from +X; None takes
    #: `AC_NORTH_DEG`.
    north_deg: float | None = None


#: AC folder (lower case) -> site. Heights and positions of circuits ApexSim
#: ships are those of the shipped YAML (`track_location.py`), Zandvoort's
#: position excepted (its YAML carries the OSM extract's centre, a
#: follow-up); the others are the Copernicus height at the start line.
#: `north_deg` is the fit of `ai/fast_lane.ai` onto the shipped centerline
#: (rms 4-12 m), measured on the layout named in the comment.
KNOWN_SITES: dict[str, Site] = {
    "ks_silverstone": Site(52.08446, -1.02032, 144.2, 91.0),  # gp / Shiverstone
    "ks_silverstone1967": Site(52.08446, -1.02032, 144.2),
    "monza": Site(45.6534, 9.27053, 181.6, 90.2),  # Monzarella
    "ks_monza66": Site(45.6534, 9.27053, 181.6),
    "ks_zandvoort": Site(52.3888, 4.5409, 4.9, 291.5),  # Zandervoort
    "ks_red_bull_ring": Site(47.23067, 14.68468, 678.6, 90.8),  # layout_gp / RedPullRing
    "ks_barcelona": Site(41.7448, 2.51654, 120.7, 89.3),  # layout_gp / Barcelunatic
    "ks_brands_hatch": Site(51.35547, 0.25766, 130.0, 87.7),  # gp / BrandsScratch
    "spa": Site(50.44922, 5.97574, 413.2, 92.4),  # SpaFrankenchamps
    "ks_nurburgring": Site(50.34791, 6.94988, 617.8, 134.9),  # layout_gp_a / Nuerburgerring
    "ks_nordschleife": Site(50.3575, 6.96, 624.6, 88.6),  # nordschleife / Mordschleife
    "ks_laguna_seca": Site(36.5843, -121.7535, 229.5),
    "ks_vallelunga": Site(42.1583, 12.3697, 190.9),
    "mugello": Site(43.9975, 11.3719, 260.4),
    "imola": Site(44.3439, 11.7167, 38.4),
    "magione": Site(43.1386, 12.2181, 234.8),
    # Mods of shipped circuits, measured the same way.
    "lemans_2017": Site(47.96812, 0.20709, 53.5, 90.2),  # lights / Lemons
    "interlagos": Site(-23.69298, -46.64727, 780.4, 75.9),  # Interlaggos
    "suzuka_0.9": Site(34.81435, 136.50484, 39.5, 50.3),  # Sudoku
    "bahrain_international_circuit": Site(26.03485, 50.50819, 8.9, 2.3),  # grandprix / Bahrainless
}


def dem_height(latitude_deg: float, longitude_deg: float, mode: str) -> tuple[float | None, str]:
    """The Copernicus height at a point, m, and how it was had: `mode`
    "fetch" downloads the tile when it is not cached, "cache" reads the
    cache only, "off" never looks. A failure is a None, never an error: the
    import goes on at sea level."""
    if mode == "off":
        return None, "off"
    tools = Path(__file__).resolve().parent.parent
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    try:
        import dem_fetch  # noqa: PLC0415 - heavy, and only wanted here
        ilat, ilon = int(math.floor(latitude_deg)), int(math.floor(longitude_deg))
        name = dem_fetch.tile_id(ilat, ilon)
        cached = (dem_fetch.DEM_CACHE / f"{name}.tif").exists()
        if mode == "cache" and not cached:
            return None, f"no cached tile {name}"
        if dem_fetch.fetch_tile(ilat, ilon, offline=mode == "cache") is None:
            return 0.0, "sea (no tile)"
        d = 0.002
        dem = dem_fetch.Dem(longitude_deg - d, longitude_deg + d, latitude_deg - d, latitude_deg + d,
                            offline=True)
        h = float(dem.sample([longitude_deg], [latitude_deg])[0])
    except (Exception, SystemExit) as e:  # noqa: BLE001 - the height is optional
        return None, f"DEM failed: {e}"
    if not math.isfinite(h):
        return None, "DEM has no post there"
    return h, name


def resolve(folder: str, latitude_deg: float | None, longitude_deg: float | None, heading_rad: float,
            dem_mode: str) -> tuple[dict, dict, list[str]]:
    """(the YAML's location keys, the report's `site`, warnings) for a
    layout of AC folder `folder` whose `ui_track.json` geotags gave
    `latitude_deg` / `longitude_deg` (None when they are a placeholder), in
    a track frame yawed by `heading_rad` from the Z-up world. The YAML
    keys are `altitude_m`, `latitude_deg`, `longitude_deg` (any of them
    absent when unknown); the report carries `north_yaw_deg` for the
    client's manifest."""
    warnings: list[str] = []
    known = KNOWN_SITES.get(folder.lower())
    report: dict = {}
    keys: dict = {}
    if known is not None:
        lat, lon, alt = known.latitude_deg, known.longitude_deg, known.altitude_m
        report["source"] = "known AC circuit"
    elif latitude_deg is not None:
        lat, lon = latitude_deg, longitude_deg
        alt, how = dem_height(lat, lon, dem_mode)
        report["source"] = "geotags"
        report["altitude_source"] = how
        if alt is None:
            warnings.append(f"no height for the geotags ({how}): the air is sea level's")
    else:
        lat = lon = alt = None
        report["source"] = "none"
        warnings.append("no geotags and not a known AC circuit: sea-level air, the sun at the server's 50 N")
    if alt is not None:
        keys["altitude_m"] = round(float(alt), 1)
    if lat is not None:
        keys["latitude_deg"] = round(float(lat), 5)
        keys["longitude_deg"] = round(float(lon), 5)
    north_world = known.north_deg if known is not None and known.north_deg is not None else AC_NORTH_DEG
    report["north_source"] = "measured" if known is not None and known.north_deg is not None else "AC convention (-z)"
    report["north_yaw_deg"] = round((north_world - math.degrees(heading_rad)) % 360.0, 2)
    report.update(keys)
    return keys, report, warnings

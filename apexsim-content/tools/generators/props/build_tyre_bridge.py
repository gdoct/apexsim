"""bridge/tyre_bridge: a giant tyre's flat arc over the road on two low legs,
with a footbridge deck under it, the kind at a long-distance circuit's
chicane (about two and a half times wider than tall).

Headless:  blender -b --python tools/generators/props/build_tyre_bridge.py
(from apexsim-content/). Writes props/bridge/tyre_bridge.{glb,blend} and a
preview in props/_preview/.

The span (23.7 m across local Y), the pivot (road centre on the ground) and
the slot names stay as the kit has them: `tyre_bridge_brand` is a brand
slot the client fills at runtime, its UVs one brand per 0..1 in u and the
band's height in v, so a brand reads upright on both sidewalls.
"""
import importlib.util, math, os, sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)

# Overall shape.
HALF_SPAN = 11.85     # concrete pads' outer faces, across Y
TOP = 9.5             # top of the tyre's tread
BAND = 1.9            # the tyre's radial thickness
HALF_DEPTH = 1.6      # the tyre's half width along the track (X)
END_Y = 11.1          # where the band's centre meets the legs
END_Z = 5.0
SEGMENTS = 48

# Legs and deck.
LEG_IN, LEG_OUT = 10.2, 11.6
LEG_HALF_X = 1.4
LEG_TOP = 4.6
DECK_Z0, DECK_Z1 = 4.4, 4.9
DECK_HALF_X = 1.0
RAIL_Z = 6.0

# The sidewall: rubber lips at the inner and outer edge, the brand between.
LIP_IN, LIP_OUT = 0.22, 0.32
CHAMFER = 0.18


def arc():
    """Centre and radius of the band's mid-line circle, and its half angle."""
    rise = (TOP - BAND / 2) - END_Z
    radius = (END_Y ** 2 + rise ** 2) / (2 * rise)
    centre_z = TOP - BAND / 2 - radius
    half = math.asin(END_Y / radius)
    return centre_z, radius, half


def build():
    apex.reset_scene()
    rubber = apex.material("tyre_bridge_rubber", (0.035, 0.035, 0.04), roughness=0.85)
    tread = apex.material("tyre_bridge_tread", (0.05, 0.05, 0.055), roughness=0.92)
    brand = apex.image_material(
        "tyre_bridge_brand", os.path.join(apex.PROPS_ROOT, "board", "brands", "piretti.png"), roughness=0.7)
    concrete = apex.material("bridge_concrete", (0.55, 0.55, 0.52), roughness=0.9)
    dark = apex.material("bridge_dark", (0.09, 0.09, 0.1), roughness=0.8)
    steel = apex.material("bridge_steel", (0.38, 0.4, 0.43), metallic=0.6, roughness=0.45)

    b = apex.Builder("tyre_bridge")
    cz, rc, half = arc()
    r_in, r_out = rc - BAND / 2, rc + BAND / 2

    def at(angle, r, x):
        # angle 0 at the top; positive toward +Y.
        return (x, r * math.sin(angle), cz + r * math.cos(angle))

    angles = [-half + 2 * half * i / SEGMENTS for i in range(SEGMENTS + 1)]
    arc_len = 2 * half * rc

    # The tyre's cross-section, walked round: inner face, the -X sidewall
    # up to the tread, the tread, the +X sidewall down. Each piece is its own
    # strip so it gets its own slot and UVs.
    def strip(mat, pts_xr, uv_of):
        """Loft a polyline of (x, r) across the arc; uv_of(i, k) gives the UV
        of section i, profile point k."""
        rows = [[at(a, r, x) for (x, r) in pts_xr] for a in angles]
        for i in range(SEGMENTS):
            for k in range(len(pts_xr) - 1):
                quad = [rows[i][k], rows[i + 1][k], rows[i + 1][k + 1], rows[i][k + 1]]
                uvs = [uv_of(i, k), uv_of(i + 1, k), uv_of(i + 1, k + 1), uv_of(i, k + 1)]
                b.quad_uv(mat, quad, uvs)

    along = lambda i: arc_len * i / SEGMENTS  # metres along the band's mid-line
    hx = HALF_DEPTH
    # Inner face (seen from under the arc), rubber.
    strip(rubber, [(hx, r_in), (-hx, r_in)], lambda i, k: (along(i), (hx, -hx)[k]))
    # Tread on top, between the chamfers.
    strip(tread, [(-hx + CHAMFER, r_out), (hx - CHAMFER, r_out)],
          lambda i, k: ((-hx + CHAMFER, hx - CHAMFER)[k], along(i)))
    # Chamfers, rubber.
    strip(rubber, [(-hx, r_out - CHAMFER), (-hx + CHAMFER, r_out)], lambda i, k: (along(i), k * CHAMFER))
    strip(rubber, [(hx - CHAMFER, r_out), (hx, r_out - CHAMFER)], lambda i, k: (along(i), k * CHAMFER))

    # Sidewalls: lip, brand band, lip. The brand repeats a whole number of
    # times along the arc, one 3:1 brand per repeat.
    band_lo, band_hi = r_in + LIP_IN, r_out - CHAMFER - LIP_OUT
    band_h = band_hi - band_lo
    repeats = max(1, round(arc_len / (3.0 * band_h)))
    for x, sign in ((-hx, -1), (hx, 1)):
        # Seen from -X (the approach), the viewer's right is -Y: u runs from
        # +Y to -Y there, and the other way on the back.
        u_of = (lambda i: repeats * (1 - i / SEGMENTS)) if sign < 0 else (lambda i: repeats * i / SEGMENTS)
        prof_lo = [(x, r_in), (x, band_lo)]
        prof_hi = [(x, band_hi), (x, r_out - CHAMFER)]
        if sign > 0:
            prof_lo, prof_hi = prof_lo[::-1], prof_hi[::-1]
        strip(rubber, prof_lo, lambda i, k: (along(i), k * LIP_IN))
        strip(rubber, prof_hi, lambda i, k: (along(i), k * LIP_OUT))
        prof_band = [(x, band_lo), (x, band_hi)]
        if sign > 0:
            strip(brand, prof_band[::-1], lambda i, k: (u_of(i), 1 - k))
        else:
            strip(brand, prof_band, lambda i, k: (u_of(i), k))

    # End caps where the tyre sits on the legs.
    for a in (angles[0], angles[-1]):
        ring = [at(a, r_in, -hx), at(a, r_in, hx), at(a, band_lo, hx), at(a, band_hi, hx),
                at(a, r_out - CHAMFER, hx), at(a, r_out, hx - CHAMFER), at(a, r_out, -hx + CHAMFER),
                at(a, r_out - CHAMFER, -hx), at(a, band_hi, -hx), at(a, band_lo, -hx)]
        f = b.bm.faces.new([b.bm.verts.new(p) for p in ring])
        f.material_index = b.slot(rubber)

    # Legs: concrete pads, then tapered steel-clad pillars up under the ends.
    for s in (-1, 1):
        y0, y1 = sorted((s * (LEG_IN - 0.3), s * HALF_SPAN))
        b.box(concrete, (-2.0, y0, 0.0), (2.0, y1, 0.5))
        y0, y1 = sorted((s * LEG_IN, s * LEG_OUT))
        b.box(concrete, (-LEG_HALF_X, y0, 0.5), (LEG_HALF_X, y1, LEG_TOP))
        # A cap plate the tyre rests on.
        y0, y1 = sorted((s * (LEG_IN - 0.1), s * (LEG_OUT + 0.15)))
        b.box(dark, (-LEG_HALF_X - 0.1, y0, LEG_TOP), (LEG_HALF_X + 0.1, y1, LEG_TOP + 0.25))

    # The footbridge deck between the legs, with a rail each side.
    b.box(dark, (-DECK_HALF_X, -LEG_IN, DECK_Z0), (DECK_HALF_X, LEG_IN, DECK_Z1))
    for x in (-DECK_HALF_X + 0.05, DECK_HALF_X - 0.05):
        b.bar(steel, (x, -LEG_IN, RAIL_Z), (x, LEG_IN, RAIL_Z), 0.05)
        b.bar(steel, (x, -LEG_IN, (DECK_Z1 + RAIL_Z) / 2), (x, LEG_IN, (DECK_Z1 + RAIL_Z) / 2), 0.03)
        n = int(2 * LEG_IN / 2.0)
        for k in range(n + 1):
            y = -LEG_IN + 2 * LEG_IN * k / n
            b.bar(steel, (x, y, DECK_Z1), (x, y, RAIL_Z), 0.04)

    # The strips are separate quads with their own UVs: weld them (UVs live
    # on the loops, so seams keep theirs) so the arc shades smooth, and wind
    # every closed piece outward.
    apex.bmesh.ops.remove_doubles(b.bm, verts=b.bm.verts[:], dist=1e-4)
    apex.bmesh.ops.recalc_face_normals(b.bm, faces=b.bm.faces[:])
    ob = b.finish(planar_uv=True, recalc=False)
    return ob


if __name__ == "__main__":
    ob = build()
    print("STATS", apex.stats())
    apex.preview_ground()
    glb, size = apex.export_one("bridge", "tyre_bridge")
    print("GLB", glb, size)
    apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", "tyre_bridge.png"),
                 look_from=(-30, -14, 4), look_at=(0, 0, 5))
    apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", "tyre_bridge_b.png"),
                 look_from=(-45, 0, 2), look_at=(0, 0, 5))
    # The preview objects stay out of the GLB (export_one selects the asset).
    apex.save_blend("bridge", "tyre_bridge")

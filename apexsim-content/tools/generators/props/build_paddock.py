"""The paddock buildings the dress pass lays for every real building near a
circuit (`dress::building_asset`): `building/media_centre` (a long office or
media block), `building/hospitality_3f` (a three-storey hospitality
terrace) and `building/control_tower` (race control, also laid for tall
square towers). Rebuilt with their footprints, pivots and slots unchanged,
so no scene, KIT row or wall bake moves: floor slabs, mullions and fins, a
recessed and colonnaded ground floor, the media centre's raked glass deck
and canopy, the hospitality block's terraces, the tower's raked cab, end
cores and roof plant.

Headless:  blender -b --python tools/generators/props/build_paddock.py
(from apexsim-content/). ASSET = "all" or one key.

Long footprints are filled with a row of these (`lay_structure`), so the
ends are solid cores that butt cleanly. Road on -Y, pivot on the road-facing
edge, the block reaching +Y. The slots are shared by name across the
building kind (the first GLB imported owns one), so the existing ones are
taken from clubhouse.glb and control_tower.glb, not redefined; glass UVs
are 0.25 a metre like the rest of the kind, one `pit_glass` window cell a
metre for the night glow.
"""
import importlib.util, math, os, sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)

import bpy

ASSET = os.environ.get("ASSET", globals().get("ASSET", "all"))
UV = 0.25


def _mat_from_glb(name, glb):
    """A slot as an existing building GLB has it (same material kind-wide)."""
    m = bpy.data.materials.get(name)
    if m is not None:
        return m
    before = set(bpy.data.objects)
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=os.path.join(apex.PROPS_ROOT, "building", glb))
    for ob in set(bpy.data.objects) - before:
        bpy.data.objects.remove(ob, do_unlink=True)
    for m in bpy.data.materials:
        if m.name.split(".")[0] == name:
            m.name = name
            return m
    raise KeyError(name)


def mats():
    return dict(
        glass=_mat_from_glb("pit_glass", "clubhouse.glb"),
        wall=_mat_from_glb("pit_wall", "clubhouse.glb"),
        conc=_mat_from_glb("bridge_concrete", "clubhouse.glb"),
        steel=_mat_from_glb("bridge_steel", "clubhouse.glb"),
        dark=_mat_from_glb("bridge_dark", "control_tower.glb"),
        rail=apex.material("building_glass_rail", (0.5, 0.58, 0.62), roughness=0.08),
    )


def fins(b, mat, x0, x1, pitch, y0, y1, z0, z1, w=0.08):
    n = max(1, round((x1 - x0) / pitch))
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        b.box(mat, (x - w / 2, y0, z0), (x + w / 2, y1, z1))


def roof_plant(b, m, x0, x1, y0, y1, z, overrun=2.8):
    """Parapet, plant boxes, a lift overrun, a vent row."""
    t = 0.25
    b.box(m["wall"], (x0, y0, z), (x1, y0 + t, z + 0.9))
    b.box(m["wall"], (x0, y1 - t, z), (x1, y1, z + 0.9))
    b.box(m["wall"], (x0, y0, z), (x0 + t, y1, z + 0.9))
    b.box(m["wall"], (x1 - t, y0, z), (x1, y1, z + 0.9))
    cx = (x0 + x1) / 2
    b.box(m["dark"], (cx - 6.5, y1 - 6.0, z), (cx - 2.0, y1 - 2.5, z + min(2.1, overrun)))
    b.box(m["dark"], (cx + 3.0, y1 - 5.5, z), (cx + 6.0, y1 - 2.0, z + 1.6))
    for k in range(4):   # condenser fans on the second box
        b.cylinder(m["steel"], (cx + 3.75 + (k % 2) * 1.5, y1 - 4.6 + (k // 2) * 1.6, z + 1.6), 0.55, 0.12, segs=10)
    b.box(m["wall"], (x1 - 5.5, y1 - 5.0, z), (x1 - 1.5, y1 - 0.8, z + overrun))   # lift and stair overrun
    for k in range(5):
        b.box(m["steel"], (x0 + 2 + k * 1.6, y0 + 3.0, z), (x0 + 2.6 + k * 1.6, y0 + 3.6, z + 0.7))


def build_media_centre():
    """40.6 x 15.6 x 21: colonnaded ground floor, two office floors behind
    slab bands and fins, a raked media deck under a canopy, roof plant."""
    apex.reset_scene()
    m = mats()
    b = apex.Builder("media_centre")
    X0, X1, Y1 = -20.3, 20.3, 15.3
    core = 1.6                        # the solid end cores
    xi0, xi1 = X0 + core, X1 - core
    G, F1, F2, F3, R = 0.0, 4.6, 8.6, 12.6, 17.2

    # End cores, full height, with a slit of stair glass on each face.
    for xa, xb in ((X0, xi0), (xi1, X1)):
        b.box(m["wall"], (xa, 0.3, 0.0), (xb, Y1, R + 0.9))
        xc = (xa + xb) / 2
        b.box(m["glass"], (xc - 0.3, 0.22, 1.0), (xc + 0.3, 0.3, R - 1.0))
    # Ground floor: glass recessed to y 2.4, a colonnade at the slab edge.
    b.box(m["glass"], (xi0, 2.4, G), (xi1, Y1 - 0.4, F1))
    b.box(m["wall"], (xi0, Y1 - 0.4, G), (xi1, Y1, F1))
    for i in range(7):
        x = xi0 + 0.6 + (xi1 - xi0 - 1.2) * i / 6
        b.box(m["conc"], (x - 0.3, 0.6, G), (x + 0.3, 1.2, F1))
    b.box(m["dark"], (-4.0, 0.9, 3.4), (4.0, 2.4, 3.7))          # entrance canopy
    # Office floors: glass at y 1.0, a slab band at each floor and fins.
    for z0, z1 in ((F1, F2), (F2, F3)):
        b.box(m["wall"], (xi0, 0.3, z0), (xi1, Y1, z0 + 0.45))     # slab edge / spandrel
        b.box(m["glass"], (xi0, 1.0, z0 + 0.45), (xi1, Y1 - 0.3, z1))
        fins(b, m["steel"], xi0 + 0.3, xi1 - 0.3, 1.6, 0.35, 1.0, z0 + 0.45, z1)
        fins(b, m["wall"], xi0 + 0.3, xi1 - 0.3, 3.2, Y1 - 0.3, Y1, z0 + 0.45, z1, w=0.25)  # back piers
    # The media deck: a slab, then glass raked out toward the road under
    # the canopy (wider at the top, as press rooms are, to look down).
    b.box(m["wall"], (xi0, 0.3, F3), (xi1, Y1, F3 + 0.45))
    zb, zt = F3 + 0.45, R
    yb, yt = 1.0, -0.4
    b.quad_uv(m["glass"], [(xi0, yb, zb), (xi1, yb, zb), (xi1, yt, zt), (xi0, yt, zt)],
              [(xi0 * UV, zb * UV), (xi1 * UV, zb * UV), (xi1 * UV, zt * UV), (xi0 * UV, zt * UV)])
    for x, k in ((xi0, -1), (xi1, 1)):   # close the rake's wedge at the cores, facing out
        pts = [(x, yb, zb), (x, yt, zt), (x, yb, zt)]
        if k > 0:
            pts = pts[::-1]
        b.quad_uv(m["wall"], pts, [(0, 0), (1, 0), (1, 1)])
    b.box(m["glass"], (xi0, 1.0, zb), (xi1, Y1 - 0.3, zt))       # the deck's body behind the rake
    n = 24
    for i in range(n + 1):
        x = xi0 + (xi1 - xi0) * i / n
        b.bar(m["steel"], (x, yb - 0.05, zb), (x, yt - 0.05, zt), 0.05, segs=4)
    b.bar(m["steel"], (xi0, yb - 0.08, zb + 1.1), (xi1, yb - 0.08 - 0.22, zb + 1.1), 0.04, segs=4)
    # Roof slab and canopy reaching past the rake, dark soffit.
    b.box(m["wall"], (X0 - 0.2, -1.6, R), (X1 + 0.2, Y1 + 0.2, R + 0.35))
    b.box(m["dark"], (X0 - 0.1, -1.5, R - 0.12), (X1 + 0.1, 0.0, R))
    roof_plant(b, m, X0 + 0.4, X1 - 0.4, 0.0, Y1, R + 0.35)
    b.bar(m["steel"], (X1 - 3.5, Y1 - 3.0, R + 3.1), (X1 - 3.5, Y1 - 3.0, 21.0), 0.06, segs=6)   # aerial
    return b.finish(planar_uv=True, uv_scale=UV)


def build_hospitality():
    """30.6 x 12.6 x 12.4: a glazed ground floor, two suite floors with
    terraces behind glass balustrades and party walls, a thin roof."""
    apex.reset_scene()
    m = mats()
    b = apex.Builder("hospitality_3f")
    X0, X1, Y0, Y1 = -15.3, 15.3, -0.3, 12.3
    core = 1.2
    xi0, xi1 = X0 + core, X1 - core
    G, F1, F2, R = 0.0, 4.2, 8.0, 11.6
    for xa, xb in ((X0, xi0), (xi1, X1)):
        b.box(m["wall"], (xa, Y0 + 0.2, 0.0), (xb, Y1, R))
    # Ground floor: recessed glazing, columns at the terrace edge.
    b.box(m["glass"], (xi0, 2.2, G), (xi1, Y1 - 0.3, F1))
    b.box(m["wall"], (xi0, Y1 - 0.3, G), (xi1, Y1, R))
    for i in range(6):
        x = xi0 + 0.4 + (xi1 - xi0 - 0.8) * i / 5
        b.cylinder(m["conc"], (x, 0.4, G), 0.25, F1, segs=10)
    suites = 5
    for z0, z1 in ((F1, F2), (F2, R)):
        # Terrace slab out to the front, the suites' glass set back 2 m.
        b.box(m["wall"], (xi0, Y0, z0), (xi1, Y1 - 0.3, z0 + 0.4))
        b.box(m["glass"], (xi0, 2.2, z0 + 0.4), (xi1, Y1 - 0.3, z1))
        fins(b, m["steel"], xi0 + 0.2, xi1 - 0.2, 1.4, 2.12, 2.2, z0 + 0.4, z1, w=0.06)
        # Party walls between suites, out to the terrace edge.
        for i in range(1, suites):
            x = xi0 + (xi1 - xi0) * i / suites
            b.box(m["conc"], (x - 0.12, Y0, z0 + 0.4), (x + 0.12, 2.2, z1))
        # Glass balustrade on posts, a steel cap rail.
        b.box(m["rail"], (xi0, Y0 + 0.05, z0 + 0.4), (xi1, Y0 + 0.1, z0 + 1.45))
        b.box(m["steel"], (xi0, Y0 + 0.0, z0 + 1.45), (xi1, Y0 + 0.15, z0 + 1.52))
    # Roof: a thin slab overhanging the top terrace, fascia, plant.
    b.box(m["wall"], (X0 - 0.15, Y0 - 0.3, R), (X1 + 0.15, Y1 + 0.15, R + 0.3))
    b.box(m["dark"], (X0 - 0.15, Y0 - 0.3, R - 0.1), (X1 + 0.15, Y0, R))
    roof_plant(b, m, X0 + 0.3, X1 - 0.3, Y0 + 0.2, Y1, R + 0.3, overrun=1.2)
    return b.finish(planar_uv=True, uv_scale=UV)


def square(h, z):
    return [(-h, -h, z), (h, -h, z), (h, h, z), (-h, h, z)]


def build_control_tower():
    """13.2 x 13.2 x 29, centred: a podium, a clad shaft with a glass slot
    up each face, the brand panel over the road, an observation floor and
    race control's cab with its glass raked outward under a deep roof."""
    apex.reset_scene()
    m = mats()
    brand = _mat_from_glb("board_brand", "control_tower.glb")
    b = apex.Builder("control_tower")
    P, S = 6.6, 4.0              # podium and shaft half-widths
    # Podium: a glazed ground floor behind piers, a cladding fascia.
    b.box(m["glass"], (-P + 0.6, -P + 0.6, 0.0), (P - 0.6, P - 0.6, 3.4))
    for x0 in (-P, P - 0.8):
        for y0 in (-P, P - 0.8):
            b.box(m["conc"], (x0, y0, 0.0), (x0 + 0.8, y0 + 0.8, 3.4))
    b.box(m["wall"], (-P, -P, 3.4), (P, P, 4.2))
    b.box(m["dark"], (-2.0, -P - 1.4, 3.0), (2.0, -P + 0.6, 3.25))   # entrance canopy
    # Shaft with a slot of glass up the middle of each face, floor bands.
    b.box(m["wall"], (-S, -S, 4.2), (S, S, 18.0))
    for z in (7.6, 11.0, 14.4):
        b.box(m["dark"], (-S - 0.06, -S - 0.06, z), (S + 0.06, S + 0.06, z + 0.18))
    for (x0, y0, x1, y1) in ((-0.7, -S - 0.08, 0.7, -S), (-0.7, S, 0.7, S + 0.08),
                             (-S - 0.08, -0.7, -S, 0.7), (S, -0.7, S + 0.08, 0.7)):
        b.box(m["glass"], (x0, y0, 4.6), (x1, y1, 14.0))
    # The brand panel over the road, 3:1, in a dark frame (u 0..1 across it).
    bw, bh, bz = 6.9, 2.3, 14.9
    b.box(m["dark"], (-bw / 2 - 0.15, -S - 0.35, bz - 0.15), (bw / 2 + 0.15, -S, bz + bh + 0.15))
    b.quad_uv(brand, [(-bw / 2, -S - 0.37, bz), (bw / 2, -S - 0.37, bz), (bw / 2, -S - 0.37, bz + bh),
                      (-bw / 2, -S - 0.37, bz + bh)], [(0, 0), (1, 0), (1, 1), (0, 1)])
    # Observation floor: a glazed band under a slab, mullions at 1.2 m.
    b.box(m["wall"], (-5.2, -5.2, 18.0), (5.2, 5.2, 18.4))
    b.box(m["glass"], (-5.0, -5.0, 18.4), (5.0, 5.0, 21.2))
    for k in range(9):
        t = -4.8 + 9.6 * k / 8
        for (x, y) in ((t, -5.02), (t, 5.02), (-5.02, t), (5.02, t)):
            b.box(m["steel"], (x - 0.04, y - 0.04, 18.4), (x + 0.04, y + 0.04, 21.2))
    b.box(m["wall"], (-5.8, -5.8, 21.2), (5.8, 5.8, 21.7))
    # Race control: glass raked outward (5.4 at the sill to 6.1 at the head).
    zs, zh = 21.7, 25.0
    b.loft(m["glass"], [square(5.4, zs), square(6.1, zh)], closed=True, cap_ends=False)
    for k in range(11):
        t = -1 + 2 * k / 10
        for (ax, ay) in ((t, -1), (t, 1), (-1, t), (1, t)):
            b.bar(m["steel"], (ax * 5.4, ay * 5.4, zs), (ax * 6.1, ay * 6.1, zh), 0.045, segs=4)
    # Roof: a deep overhang with a dark soffit, plant and the mast.
    b.box(m["wall"], (-P, -P, zh), (P, P, zh + 0.5))
    b.box(m["dark"], (-P + 0.05, -P + 0.05, zh - 0.1), (P - 0.05, P - 0.05, zh))
    b.box(m["dark"], (-3.0, -1.5, zh + 0.5), (1.0, 2.5, zh + 1.6))
    b.cylinder(m["steel"], (3.2, 3.0, zh + 0.5), 0.5, 0.25, segs=10)
    b.bar(m["steel"], (3.2, 3.0, zh + 0.75), (3.2, 3.0, 29.0), 0.07, segs=6)
    for z in (27.0, 28.0):
        b.bar(m["steel"], (2.6, 3.0, z), (3.8, 3.0, z), 0.025, segs=4)
    return b.finish(planar_uv=True, uv_scale=UV)


BUILDS = {"media_centre": build_media_centre, "hospitality_3f": build_hospitality,
          "control_tower": build_control_tower}
VIEWS = {"media_centre": [((-30, -42, 9), (2, 6, 9)), ((-55, -14, 3), (0, 6, 9))],
         "hospitality_3f": [((-24, -32, 7), (1, 5, 6)), ((-42, -10, 2.5), (0, 5, 6))],
         "control_tower": [((-22, -40, 10), (0, 0, 14)), ((-50, -25, 3), (0, 0, 15))]}

if __name__ == "__main__":
    out = {}
    for key, fn in BUILDS.items():
        if ASSET not in ("all", key):
            continue
        fn()
        out[key] = apex.stats()[key]
        apex.export_one("building", key)
        apex.save_blend("building", key)
        apex.preview_ground()
        for i, (lf, la) in enumerate(VIEWS[key]):
            apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", key + ("" if i == 0 else "_b") + ".png"),
                         look_from=lf, look_at=la)
    print("STATS", out)

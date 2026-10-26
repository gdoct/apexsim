"""bridge/tyre_bridge: a giant tyre's arc over the road, the kind at a
long-distance circuit's chicane. The arc springs from the ground well beyond
the run-off on both sides (about three and a half times wider than tall); a
footbridge deck crosses straight under its crown, and the sidewall above the
deck is solid, a brand panel on each face. Below the deck the arc is open.

Headless:  blender -b --python tools/generators/props/build_tyre_bridge.py
(from apexsim-content/). Writes props/bridge/tyre_bridge.{glb,blend} and a
preview in props/_preview/.

The pivot is the road centre on the ground, the span across local Y. The
client places it at its authored span (`ApexProps::BridgeKeepsSpan`): it
stands over the run-off, not the road's edges. `tyre_bridge_brand` is a
brand slot the client fills at runtime, one 3:1 brand across 0..1 in u.
"""
import importlib.util, math, os, sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)

# The outer arc: through the ground at +-HALF_SPAN and the crown at TOP.
HALF_SPAN = 22.0
TOP = 13.0
BAND = 2.6            # the tyre's radial thickness where the arc is open
HALF_DEPTH = 2.2      # the tyre's half width along the track (X)
FOOT_Z = -2.0         # the arc runs on into the ground, for uneven verges
RIM = 0.35            # the coloured rim along the sidewall's outer edge
SEGMENTS = 64

# The footbridge deck under the crown: its underside is the arc's opening.
DECK_Z0, DECK_Z1 = 5.6, 6.8
FASCIA_PROUD = 0.15
FASCIA_DROP = 0.08

# The brand panel on each sidewall, above the deck.
PANEL_GAP = 0.3       # above the deck's fascia
PANEL_MARGIN = 0.45   # below the rim at its upper corners
PANEL_PROUD = 0.03


def circle():
    """Centre height and radius of the outer arc."""
    radius = (HALF_SPAN ** 2 + TOP ** 2) / (2 * TOP)
    return TOP - radius, radius


def build():
    apex.reset_scene()
    sidewall = apex.material("tyre_bridge_sidewall", (0.07, 0.2, 0.3), roughness=0.6)
    rim = apex.material("tyre_bridge_rim", (0.85, 0.65, 0.02), roughness=0.55)
    tread = apex.material("tyre_bridge_tread", (0.05, 0.05, 0.055), roughness=0.92)
    fascia = apex.material("tyre_bridge_fascia", (0.85, 0.65, 0.02), roughness=0.6)
    brand = apex.image_material(
        "tyre_bridge_brand", os.path.join(apex.PROPS_ROOT, "board", "brands", "piretti.png"), roughness=0.7)
    concrete = apex.material("bridge_concrete", (0.55, 0.55, 0.52), roughness=0.9)

    b = apex.Builder("tyre_bridge")
    cz, r_out = circle()
    r_in = r_out - BAND
    hx = HALF_DEPTH

    def at(angle, r, x):
        # angle 0 at the crown; positive toward +Y.
        return (x, r * math.sin(angle), cz + r * math.cos(angle))

    def inner(angle):
        # The solid reaches down to the deck's underside where that is below
        # the inner circle, so the sidewall above the deck is closed.
        return min(r_in, (DECK_Z0 - cz) / math.cos(angle))

    foot = math.acos((FOOT_Z - cz) / r_out)
    kink = math.acos((DECK_Z0 - cz) / r_in)
    angles = sorted({-foot + 2 * foot * i / SEGMENTS for i in range(SEGMENTS + 1)} | {-kink, kink})

    # Walk the arc: tread on top, the inner face under it, and on each side
    # the sidewall with its rim. Quads wind outward on their own: the tread
    # faces away from the centre, the inner face toward it.
    for a0, a1 in zip(angles, angles[1:]):
        b.sheet(tread, at(a0, r_out, -hx), at(a0, r_out, hx), at(a1, r_out, hx), at(a1, r_out, -hx))
        b.sheet(sidewall, at(a0, inner(a0), hx), at(a0, inner(a0), -hx),
                at(a1, inner(a1), -hx), at(a1, inner(a1), hx))
        for x in (-hx, hx):
            q_side = [at(a0, inner(a0), x), at(a0, r_out - RIM, x), at(a1, r_out - RIM, x), at(a1, inner(a1), x)]
            q_rim = [at(a0, r_out - RIM, x), at(a0, r_out, x), at(a1, r_out, x), at(a1, r_out - RIM, x)]
            if x > 0:
                q_side, q_rim = q_side[::-1], q_rim[::-1]
            b.sheet(sidewall, *q_side)
            b.sheet(rim, *q_rim)
    # Close the feet underground.
    for a in (angles[0], angles[-1]):
        cap = [at(a, inner(a), -hx), at(a, inner(a), hx), at(a, r_out, hx), at(a, r_out, -hx)]
        b.sheet(sidewall, *(cap if a > 0 else cap[::-1]))

    # Concrete pads round the feet where they meet the ground.
    for s in (-1, 1):
        y_in = math.sqrt(r_in ** 2 - cz ** 2)
        y0, y1 = sorted((s * (y_in - 0.6), s * (HALF_SPAN + 0.6)))
        b.box(concrete, (-hx - 0.6, y0, -0.6), (hx + 0.6, y1, 0.3))

    # The deck's fascia across the opening, standing a little proud of both
    # sidewalls and running into the arc's legs. Its underside hangs a
    # little below the opening's top, which would z-fight at the same height.
    deck_y = math.sqrt(r_in ** 2 - (DECK_Z0 - cz) ** 2) + 0.5
    b.box(fascia, (-hx - FASCIA_PROUD, -deck_y, DECK_Z0 - FASCIA_DROP), (hx + FASCIA_PROUD, deck_y, DECK_Z1))

    # The body is closed: weld it and wind it outward before the panels,
    # which are single quads wound by hand.
    apex.bmesh.ops.remove_doubles(b.bm, verts=b.bm.verts[:], dist=1e-4)
    apex.bmesh.ops.recalc_face_normals(b.bm, faces=b.bm.faces[:])

    # The brand panel: the tallest 3:1 that fits between the fascia and the
    # rim at its upper corners.
    z0 = DECK_Z1 + PANEL_GAP
    h = 0.0
    while True:
        nh = h + 0.05
        y = 1.5 * nh
        if y >= r_out or cz + math.sqrt(r_out ** 2 - y ** 2) - RIM - PANEL_MARGIN < z0 + nh:
            break
        h = nh
    half_w, z1 = 1.5 * h, z0 + h
    # Seen from -X (the approach), the viewer's right is -Y; seen from +X it
    # is +Y. Either way u runs left to right and v bottom to top.
    xa = hx + PANEL_PROUD
    b.quad_uv(brand, [(-xa, half_w, z0), (-xa, -half_w, z0), (-xa, -half_w, z1), (-xa, half_w, z1)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])
    b.quad_uv(brand, [(xa, -half_w, z0), (xa, half_w, z0), (xa, half_w, z1), (xa, -half_w, z1)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])

    ob = b.finish(planar_uv=True, recalc=False)
    return ob, (2 * hx + 2 * FASCIA_PROUD, 2 * (HALF_SPAN + 0.6), TOP), (2 * half_w, h)


if __name__ == "__main__":
    ob, size, panel = build()
    print("STATS", apex.stats(), "SIZE", size, "PANEL", panel)
    apex.preview_ground()
    glb, nbytes = apex.export_one("bridge", "tyre_bridge")
    print("GLB", glb, nbytes)
    apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", "tyre_bridge.png"),
                 look_from=(-70, -22, 6), look_at=(0, 0, 6))
    apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", "tyre_bridge_b.png"),
                 look_from=(-80, 0, 2), look_at=(0, 0, 6))
    # The preview objects stay out of the GLB (export_one selects the asset).
    apex.save_blend("bridge", "tyre_bridge")

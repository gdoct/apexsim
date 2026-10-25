r"""bridge/start_gantry: the start/finish gantry every circuit stands over its
line (the race director's start lights hang on it).

    blender -b --python tools/generators/props/build_start_gantry.py    (from apexsim-content/)

A dark box truss across a 15 m road on two truss legs (the builder scales
local Y to the road, `ApexProps::BridgeSpanScale`), a branded fascia on both
faces of the beam (`board_brand`, the kit's invented brands), and a compact
lamp housing hung under the middle of the beam, facing -X toward the grid.
The housing's upper lamp row is where the client lays the five
`ApexStartLight` lenses: x -0.37 m, y every 0.80 m, z 5.45 m, lenses of
0.22 m (`ApexProps::GantryLamp*`); the lower row is dark decoration.

Conventions (docs/props.md): +X along the track, metres, pivot on the road
centre at the ground; a bridge is centred on its span (across Y).
"""
import os
exec(open(os.path.join(os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"),
                       "tools", "generators", "props", "marina_common.py")).read())

SPAN_Y = 8.6          # legs' centre lines, either side of the road centre
BEAM_Z = 6.5          # beam centre
BEAM = 0.9            # beam and leg truss section
LAMP_X, LAMP_PITCH, LAMP_Z, LAMP_R = -0.37, 0.80, 5.45, 0.22


def mats():
    M, A = tex.kit_material, apex.material
    return dict(
        truss=M("bridge_dark", (0.12, 0.12, 0.13)),
        conc=M("pit_concrete", (0.6, 0.6, 0.58)),
        housing=A("start_housing", (0.03, 0.03, 0.035), roughness=0.45),
        visor=A("mb_black", (0.03, 0.03, 0.035), roughness=0.5),
        lens=A("start_lens_dark", (0.06, 0.02, 0.02), roughness=0.2),
        fascia=A("start_fascia", (0.05, 0.05, 0.06), roughness=0.5),
        brand=apex.image_material("board_brand", os.path.join(PR, "board", "brands", "apexsim.png")),
    )


def start_gantry():
    m = mats()
    b = B("start_gantry")
    top = BEAM_Z + BEAM / 2
    for y in (-SPAN_Y, SPAN_Y):
        b.box(m["conc"], (-0.8, y - 0.8, 0.0), (0.8, y + 0.8, 0.5))
        b.truss(m["truss"], (0.0, y, 0.5), (0.0, y, top), BEAM, BEAM, pitch=1.0, r_chord=0.07, r_diag=0.035)
    b.truss(m["truss"], (0.0, -SPAN_Y - BEAM / 2, BEAM_Z), (0.0, SPAN_Y + BEAM / 2, BEAM_Z), BEAM, BEAM,
            pitch=1.1, r_chord=0.08, r_diag=0.04)

    # The fascia: a dark board on each face of the beam, the brand on it
    # either side of the housing, so it reads from the grid and from the
    # cars arriving at the line.
    z0, z1 = BEAM_Z - BEAM / 2 + 0.05, BEAM_Z + BEAM / 2 - 0.05
    for face in (-1.0, 1.0):
        x = face * (BEAM / 2 + 0.06)
        b.box(m["fascia"], (min(x, x + face * 0.06), -SPAN_Y + 0.6, z0), (max(x, x + face * 0.06), SPAN_Y - 0.6, z1))
        xf = x + face * 0.07
        for y0, y1 in ((-SPAN_Y + 0.8, -2.6), (2.6, SPAN_Y - 0.8)):
            pts = [(xf, y0, z0 + 0.06), (xf, y1, z0 + 0.06), (xf, y1, z1 - 0.06), (xf, y0, z1 - 0.06)]
            if face > 0:
                pts = [pts[1], pts[0], pts[3], pts[2]]
            # Read from in front of each face: u runs to the viewer's right.
            b.quad_uv(m["brand"], pts, [(2, 0), (0, 0), (0, 1), (2, 1)])

    # The lamp housing under the beam: small, dark, two rows of five.
    hw = 2 * LAMP_PITCH + 0.45
    hz0, hz1 = LAMP_Z - 0.75, LAMP_Z + 0.32
    b.box(m["housing"], (-0.30, -hw, hz0), (0.25, hw, hz1))
    for y in (-1.2, 1.2):
        b.bar(m["truss"], (0.0, y, hz1), (0.0, y, BEAM_Z - BEAM / 2), 0.05, segs=6)
    for row, z in enumerate((LAMP_Z, LAMP_Z - 0.48)):
        for i in range(5):
            y = (i - 2) * LAMP_PITCH
            # A visor over each lamp; the lower row's lenses are the
            # housing's own (the client lights the upper row).
            b.box(m["visor"], (-0.42, y - 0.27, z + LAMP_R + 0.02), (-0.30, y + 0.27, z + LAMP_R + 0.06))
            if row == 1:
                b.cylinder(m["lens"], (-0.33, y, z), LAMP_R * 0.9, 0.03, segs=16, axis='X')
    return b.finish()


clear_scene()
start_gantry()
print("START_GANTRY", export_tree("bridge", "start_gantry"), tri_count("start_gantry"), bounds("start_gantry"))

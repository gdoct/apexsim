"""The rally cars' shape data, shared by build_rally.py (Blender) and the
livery painter (rally_skins.py, plain Python): no bpy here.

Group B, 1984-85, each car as it was and under a sound-alike name
(docs/content/naming.md):

    oudi     Oudi Sport Kwattro S1    front-engined five-cylinder turbo, 4WD,
                                      a short boxy coupe with box flares, a
                                      shovel splitter and a tall boot wing
    pugeot   Pugeot 205 T16           a small hatchback's shell over a
                                      mid-mounted turbo four, 4WD; quarter
                                      intakes behind the doors, a roof wing
    lancha   Lancha 037 Rally         a low mid-engined coupe, supercharged,
                                      RWD; a long nose, a clamshell deck with
                                      louvres and a ducktail

Frame: nose on -Y, ground z = 0, metres, left-hand drive (driver on +X).

Section control points (`keys`), floor centre outwards and up to the roof
centre, x as a share of the car's half-width (scaled to `half_w`):
    0 floor centre      3 fender peak (HARD)   6 upper surface
    1 floor edge        4 shoulder (HARD)      7 roof shoulder
    2 sill (HARD)       5 belt line            8 roof centre
`roof` is the roofline in profile (y, z), laid over points 6-8.

`skin_box` (y_nose, y_tail, half_width, height) is the frame the skin is
projected in (skin_layout.py): fixed here, so the painter and the build
agree whatever the mesh adds. `belt_z` and `sill_z` are where the painter
puts a flank's bands; the build does not read them.
"""

VARIANTS = {
    "oudi": dict(
        folder="oudi-kwattro-s1", stem="oudi_kwattro_s1",
        half_w=0.93, axles=(-1.112, 1.112), tyres=(0.315, 0.315), track=1.49,
        floor=0.120, eye=(0.33, 0.12, 1.07),
        number="1",
        # a short boxy coupe: a long bonnet over the engine ahead of the
        # front axle, an upright screen, a short roof and a steep back
        # light onto a high boot carrying the E2's wing
        screen_y=(-0.62, -0.06), glass_y=(-0.56, 0.70), rear_glass_y=(0.66, 1.26),
        roof=[(-0.62, 0.92), (-0.36, 1.12), (-0.06, 1.315), (0.30, 1.34), (0.62, 1.31),
              (0.95, 1.16), (1.26, 1.02), (1.60, 1.00), (2.06, 0.97)],
        ends=dict(nose_top=0.66, nose_w=0.86, tail_top=0.96, tail_bot=0.30, tail_w=0.95),
        hips=(0.03, 0.03), belt_z=0.88, sill_z=0.30,
        shutlines=[(-1.95, "upper"), (-0.66, "upper"), (-0.50, "side"), (0.50, "side"), (1.30, "upper")],
        lamps="rect", lamp_x=(0.42, 0.80), lamp_h=0.13, grille="slats",
        splitter="shovel", wing="boot", wing_z=1.22, wing_y=1.80, wing_hw=0.82, wing_chord=0.36,
        tail="bar", exhaust="side", scoop=(0.0, 0.40, 0.07),
        vents=["bonnet"], side=[("recess", dict(y0=(0.62, 0.70), y1=(0.92, 0.94), j0=2.4, j1=3.5,
                                                depth=0.040, rim=0.020))],
        skin_box=(-2.12, 2.07, 0.93, 1.36),
        keys=[
            (-2.12, [(0, .21), (.60, .21), (.80, .30), (.86, .48), (.80, .62), (.60, .655), (.40, .66), (.18, .665), (0, .67)]),
            (-1.80, [(0, .13), (.85, .13), (.96, .30), (.99, .58), (.94, .76), (.70, .80), (.45, .81), (.20, .815), (0, .82)]),
            (-1.112, [(0, .12), (.90, .12), (1.0, .28), (1.0, .70), (.96, .81), (.72, .85), (.48, .865), (.22, .87), (0, .875)]),
            (-0.62, [(0, .12), (.89, .12), (.925, .30), (.93, .70), (.915, .85), (.82, .89), (.56, .905), (.26, .915), (0, .92)]),
            (-0.20, [(0, .12), (.89, .12), (.925, .30), (.93, .70), (.915, .86), (.84, .90), (.72, 1.12), (.46, 1.29), (0, 1.33)]),
            (0.30, [(0, .12), (.89, .12), (.925, .30), (.935, .70), (.92, .865), (.845, .905), (.72, 1.13), (.46, 1.30), (0, 1.34)]),
            (0.70, [(0, .12), (.90, .12), (.94, .30), (.95, .71), (.93, .87), (.84, .91), (.71, 1.10), (.45, 1.25), (0, 1.28)]),
            (1.112, [(0, .12), (.90, .12), (1.0, .30), (1.0, .72), (.96, .86), (.82, .93), (.66, 1.00), (.42, 1.05), (0, 1.07)]),
            (1.60, [(0, .16), (.88, .16), (.97, .32), (.985, .70), (.94, .87), (.80, .95), (.56, .98), (.28, .995), (0, 1.0)]),
            (2.06, [(0, .30), (.84, .30), (.92, .40), (.93, .66), (.89, .86), (.74, .93), (.52, .955), (.26, .965), (0, .97)]),
        ]),
    "pugeot": dict(
        folder="pugeot-205-t16", stem="pugeot_205_t16",
        half_w=0.85, axles=(-1.27, 1.27), tyres=(0.310, 0.310), track=1.43,
        floor=0.120, eye=(0.31, -0.02, 1.07),
        number="7",
        # a small hatchback stretched over a mid engine: a short sloping
        # bonnet, a tall cabin whose roof runs nearly to the tail, an
        # upright hatch carrying the E2's wing, big quarter intakes
        screen_y=(-0.84, -0.26), glass_y=(-0.74, 0.46), rear_glass_y=(1.60, 1.90),
        roof=[(-0.84, 0.93), (-0.56, 1.15), (-0.26, 1.33), (0.30, 1.355), (1.00, 1.35),
              (1.52, 1.33), (1.66, 1.25), (1.80, 1.12), (1.93, 0.99)],
        ends=dict(nose_top=0.66, nose_w=0.88, tail_top=0.98, tail_bot=0.30, tail_w=0.94),
        hips=(0.03, 0.04), belt_z=0.90, sill_z=0.30,
        shutlines=[(-1.66, "upper"), (-0.70, "side"), (0.42, "side"), (1.58, "upper")],
        lamps="rect", lamp_x=(0.40, 0.76), lamp_h=0.11, grille="slats",
        splitter="lip", wing="roof", wing_z=1.38, wing_y=1.70, wing_hw=0.78, wing_chord=0.32,
        tail="blocks", exhaust="centre", scoop=(0.0, 0.10, 0.05),
        vents=[], side=[("recess", dict(y0=(0.50, 0.66), y1=(0.92, 0.96), j0=2.3, j1=4.4,
                                        depth=0.060, rim=0.030))],
        skin_box=(-1.90, 1.94, 0.85, 1.38),
        keys=[
            (-1.89, [(0, .22), (.62, .22), (.82, .30), (.88, .45), (.84, .60), (.66, .64), (.44, .65), (.20, .655), (0, .66)]),
            (-1.60, [(0, .13), (.86, .13), (.96, .28), (.99, .55), (.95, .72), (.74, .76), (.50, .77), (.24, .78), (0, .78)]),
            (-1.27, [(0, .12), (.90, .12), (1.0, .28), (1.0, .68), (.96, .80), (.80, .84), (.55, .86), (.26, .87), (0, .87)]),
            (-0.84, [(0, .12), (.88, .12), (.93, .30), (.94, .70), (.925, .86), (.84, .90), (.60, .92), (.30, .93), (0, .93)]),
            (-0.40, [(0, .12), (.88, .12), (.93, .30), (.94, .70), (.925, .87), (.85, .91), (.75, 1.14), (.50, 1.31), (0, 1.35)]),
            (0.30, [(0, .12), (.88, .12), (.93, .30), (.95, .70), (.93, .88), (.86, .92), (.76, 1.16), (.52, 1.33), (0, 1.355)]),
            (0.80, [(0, .12), (.90, .12), (.96, .30), (.98, .70), (.95, .89), (.87, .94), (.77, 1.16), (.52, 1.33), (0, 1.355)]),
            (1.27, [(0, .12), (.90, .12), (1.0, .30), (1.0, .72), (.96, .90), (.88, .95), (.77, 1.15), (.52, 1.31), (0, 1.33)]),
            (1.62, [(0, .16), (.88, .16), (.97, .32), (.98, .70), (.94, .90), (.86, .95), (.75, 1.12), (.50, 1.27), (0, 1.30)]),
            (1.935, [(0, .30), (.84, .30), (.92, .40), (.93, .68), (.89, .88), (.80, .92), (.68, .96), (.45, .97), (0, .98)]),
        ]),
    "lancha": dict(
        folder="lancha-037-rally", stem="lancha_037_rally",
        half_w=0.925, axles=(-1.22, 1.22), tyres=(0.305, 0.315), track=1.50,
        floor=0.110, eye=(0.32, 0.02, 0.99),
        number="4",
        # low and mid-engined: a long falling nose, a raked screen, a short
        # roof, a small upright back light and a long clamshell deck with
        # louvres over the engine to a ducktail
        screen_y=(-0.58, -0.08), glass_y=(-0.48, 0.42), rear_glass_y=(0.46, 0.64),
        roof=[(-0.58, 0.87), (-0.34, 1.05), (-0.08, 1.215), (0.22, 1.245), (0.44, 1.22),
              (0.62, 1.05), (0.80, 0.99), (1.50, 0.975), (1.945, 0.965)],
        ends=dict(nose_top=0.56, nose_w=0.88, tail_top=0.96, tail_bot=0.30, tail_w=0.95),
        hips=(0.03, 0.05), belt_z=0.82, sill_z=0.28,
        shutlines=[(-1.80, "upper"), (-0.62, "upper"), (-0.44, "side"), (0.44, "side"), (0.72, "upper")],
        lamps="round", lamp_x=(0.44, 0.80), lamp_h=0.10, grille="mesh",
        splitter="lip", wing="ducktail", wing_z=1.01, wing_y=1.86, wing_hw=0.86, wing_chord=0.20,
        tail="round", exhaust="twin", scoop=None,
        vents=["deck"], side=[("recess", dict(y0=(0.52, 0.62), y1=(0.86, 0.88), j0=2.3, j1=3.5,
                                              depth=0.050, rim=0.025))],
        skin_box=(-1.98, 1.95, 0.925, 1.26),
        keys=[
            (-1.97, [(0, .20), (.60, .20), (.80, .28), (.86, .42), (.82, .52), (.64, .55), (.42, .555), (.20, .558), (0, .56)]),
            (-1.60, [(0, .12), (.86, .12), (.96, .28), (.99, .52), (.94, .66), (.72, .70), (.48, .71), (.22, .715), (0, .72)]),
            (-1.22, [(0, .11), (.90, .11), (1.0, .27), (1.0, .62), (.96, .74), (.76, .78), (.50, .79), (.24, .795), (0, .80)]),
            (-0.58, [(0, .11), (.88, .11), (.93, .28), (.94, .64), (.92, .80), (.82, .84), (.58, .86), (.28, .865), (0, .87)]),
            (-0.20, [(0, .11), (.88, .11), (.93, .28), (.94, .64), (.92, .81), (.80, .85), (.66, 1.05), (.42, 1.20), (0, 1.23)]),
            (0.30, [(0, .11), (.88, .11), (.93, .28), (.95, .64), (.93, .82), (.80, .86), (.66, 1.06), (.42, 1.21), (0, 1.245)]),
            (0.70, [(0, .11), (.90, .11), (.96, .28), (.98, .66), (.95, .84), (.80, .90), (.62, .975), (.36, 1.0), (0, 1.01)]),
            (1.22, [(0, .11), (.90, .11), (1.0, .28), (1.0, .68), (.96, .86), (.80, .92), (.58, .955), (.30, .97), (0, .975)]),
            (1.60, [(0, .15), (.88, .15), (.97, .30), (.98, .66), (.94, .85), (.80, .92), (.58, .95), (.30, .96), (0, .97)]),
            (1.945, [(0, .30), (.84, .30), (.92, .40), (.93, .64), (.89, .84), (.76, .92), (.55, .95), (.28, .96), (0, .965)]),
        ]),
}

BY_FOLDER = {v["folder"]: k for k, v in VARIANTS.items()}

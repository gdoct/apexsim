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
`pod` is the auxiliary lamp pod: `mount` "front" (on a bar ahead of the
grille, `dz` from the nose top) or "bonnet" (standing on the bonnet, `dy`
behind the nose, its centres `lift` over the bonnet there), lamp radius `r`, lamp centres at +-`xs`.

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
        # four big round lamps across the front at the headlamps' height,
        # standing on a bar in front of the grille
        pod=dict(mount="front", dz=-0.09, r=0.088, xs=(0.17, 0.46)),
        # the short-wheelbase box: a long flat bonnet over the engine ahead
        # of the front axle to an upright face, an upright screen, a short
        # flat roof, a steep back light onto a high flat boot and a square
        # tail; flat sides creased at the belt, big square flares, the
        # quarter glass ending upright ahead of a wide C-pillar, and the
        # E2's wing standing at roof height over the boot
        # the quarter glass behind the B-pillar runs back to a rear edge raked
        # parallel to the back light (`glass_top`: where it meets the roof)
        screen_y=(-0.60, -0.05), glass_y=(-0.52, 0.98), rear_glass_y=(0.60, 1.03),
        rear_wrap=False, glass_top=0.62,
        # the S1's greenhouse is straight lines: the screen, a long flat roof,
        # the back light straight down to the boot (`roof_step`: sections
        # close enough that the loft keeps them straight)
        roof=[(-0.62, 0.885), (-0.08, 1.335), (0.60, 1.33), (1.18, 1.01), (1.60, 1.00), (2.06, 0.98)],
        roof_step=0.06,
        ends=dict(nose_top=0.62, nose_w=0.92, tail_top=0.98, tail_bot=0.30, tail_w=0.95),
        tips=(0.025, 0.93, 0.90, 0.025, 0.94, 0.92),
        hips=(0.02, 0.03), belt_z=0.85, sill_z=0.30,
        shutlines=[(-1.98, "upper"), (-0.64, "upper"), (-0.50, "side"), (0.50, "side"), (1.10, "upper")],
        lamps="rect", lamp_x=(0.44, 0.82), lamp_h=0.12, grille="slats",
        splitter="shovel", wing="boot", wing_z=1.30, wing_y=1.70, wing_hw=0.82, wing_chord=0.36,
        tail="bar", exhaust="side", scoop=(0.0, 0.32, 0.06),
        # the E2's big intake ahead of the rear wheel, from low on the
        # flank nearly to the belt, its leading edge raked back
        vents=["bonnet"], side=[("recess", dict(y0=(0.53, 0.57), y1=(0.71, 0.82), j0=2.6, j1=4.6,
                                                depth=0.065, rim=0.018))],
        skin_box=(-2.12, 2.07, 0.93, 1.36),
        keys=[
            # the E2's nose: the wings and bonnet curve down to the lamps
            (-2.12, [(0, .20), (.62, .20), (.84, .24), (.88, .40), (.86, .56), (.76, .60), (.52, .615), (.24, .62), (0, .62)]),
            (-1.95, [(0, .15), (.82, .15), (.93, .24), (.96, .44), (.95, .64), (.86, .69), (.58, .705), (.27, .71), (0, .715)]),
            (-1.60, [(0, .13), (.86, .13), (.97, .26), (.99, .55), (.97, .74), (.90, .775), (.61, .79), (.29, .795), (0, .80)]),
            (-1.112, [(0, .12), (.90, .12), (1.0, .28), (1.0, .68), (.98, .81), (.92, .84), (.62, .855), (.30, .86), (0, .865)]),
            (-0.60, [(0, .12), (.89, .12), (.93, .30), (.935, .70), (.93, .83), (.90, .855), (.64, .875), (.30, .88), (0, .885)]),
            (-0.20, [(0, .12), (.89, .12), (.93, .30), (.935, .70), (.93, .83), (.90, .855), (.83, 1.10), (.70, 1.30), (0, 1.33)]),
            (0.30, [(0, .12), (.89, .12), (.93, .30), (.94, .70), (.935, .83), (.905, .855), (.84, 1.10), (.71, 1.31), (0, 1.34)]),
            (0.70, [(0, .12), (.90, .12), (.95, .30), (.96, .70), (.95, .835), (.91, .86), (.84, 1.08), (.70, 1.24), (0, 1.27)]),
            (1.112, [(0, .12), (.90, .12), (1.0, .30), (1.0, .72), (.98, .86), (.93, .90), (.84, .96), (.62, .99), (0, 1.0)]),
            (1.60, [(0, .16), (.88, .16), (.98, .32), (.99, .70), (.97, .86), (.92, .90), (.80, .96), (.56, .99), (0, 1.0)]),
            (2.06, [(0, .30), (.84, .30), (.92, .38), (.93, .66), (.92, .86), (.88, .92), (.74, .96), (.50, .975), (0, .98)]),
        ]),
    "pugeot": dict(
        folder="pugeot-205-t16", stem="pugeot_205_t16",
        half_w=0.85, axles=(-1.27, 1.27), tyres=(0.310, 0.310), track=1.43,
        floor=0.120, eye=(0.31, -0.02, 1.07),
        number="7",
        # a pod of four lamps standing on the front of the bonnet
        pod=dict(mount="bonnet", dy=-0.02, lift=0.025, r=0.076, xs=(0.12, 0.37)),
        # the small hatchback over a mid engine: a short, nearly flat bonnet
        # ending in an upright face, a low belt under tall glass, a flat
        # roof that ends well short of the tail, the hatch glass falling at
        # ~45 degrees onto an upright tailgate; flat sides with a crease at
        # the belt, square flares, quarter intakes behind the doors, and the
        # E2's wing on the back of the roof
        screen_y=(-0.85, -0.30), glass_y=(-0.80, 1.05), rear_glass_y=(1.22, 1.70),
        rear_wrap=False,
        roof=[(-0.85, 0.88), (-0.30, 1.335), (0.30, 1.355), (1.00, 1.345), (1.18, 1.32),
              (1.45, 1.16), (1.72, 0.99), (1.935, 0.98)],
        ends=dict(nose_top=0.69, nose_w=0.92, tail_top=0.98, tail_bot=0.30, tail_w=0.95),
        tips=(0.025, 0.93, 0.90, 0.025, 0.94, 0.92),
        hips=(0.03, 0.04), belt_z=0.86, sill_z=0.30,
        shutlines=[(-1.70, "upper"), (-0.76, "side"), (0.42, "side"), (1.72, "upper")],
        lamps="rect", lamp_x=(0.42, 0.82), lamp_h=0.12, grille="slats",
        splitter="lip", wing="roof", wing_z=1.43, wing_y=1.08, wing_hw=0.80, wing_chord=0.36,
        tail="blocks", exhaust="centre", scoop=(0.0, 0.10, 0.05),
        vents=[], side=[("recess", dict(y0=(0.52, 0.64), y1=(0.92, 0.95), j0=2.3, j1=4.2,
                                        depth=0.055, rim=0.030))],
        skin_box=(-1.90, 1.94, 0.85, 1.38),
        keys=[
            (-1.89, [(0, .22), (.62, .22), (.86, .26), (.90, .45), (.90, .64), (.80, .675), (.55, .685), (.25, .69), (0, .69)]),
            (-1.80, [(0, .16), (.82, .16), (.93, .25), (.95, .48), (.94, .70), (.86, .725), (.60, .735), (.28, .74), (0, .745)]),
            (-1.27, [(0, .12), (.90, .12), (1.0, .28), (1.0, .62), (.97, .78), (.90, .80), (.62, .82), (.30, .83), (0, .835)]),
            (-0.85, [(0, .12), (.88, .12), (.94, .30), (.95, .62), (.94, .82), (.90, .85), (.65, .87), (.32, .875), (0, .88)]),
            (-0.40, [(0, .12), (.88, .12), (.94, .30), (.95, .62), (.94, .83), (.90, .86), (.82, 1.10), (.68, 1.30), (0, 1.34)]),
            (0.30, [(0, .12), (.88, .12), (.94, .30), (.95, .62), (.945, .84), (.905, .865), (.83, 1.11), (.70, 1.32), (0, 1.355)]),
            (0.80, [(0, .12), (.90, .12), (.96, .30), (.98, .62), (.96, .85), (.91, .87), (.83, 1.11), (.70, 1.32), (0, 1.35)]),
            (1.27, [(0, .12), (.90, .12), (1.0, .30), (1.0, .64), (.97, .86), (.92, .88), (.84, 1.08), (.70, 1.26), (0, 1.30)]),
            (1.62, [(0, .16), (.88, .16), (.97, .32), (.97, .64), (.95, .86), (.90, .90), (.82, .98), (.62, 1.02), (0, 1.04)]),
            (1.935, [(0, .30), (.84, .30), (.92, .38), (.93, .64), (.92, .86), (.86, .92), (.72, .96), (.45, .975), (0, .98)]),
        ]),
    "lancha": dict(
        folder="lancha-037-rally", stem="lancha_037_rally",
        half_w=0.925, axles=(-1.22, 1.22), tyres=(0.305, 0.315), track=1.50,
        floor=0.110, eye=(0.32, 0.02, 0.99),
        number="4",
        # four lamps on a bar along the nose, ahead of the bonnet's edge
        pod=dict(mount="bonnet", dy=-0.03, lift=0.035, r=0.080, xs=(0.13, 0.41)),
        # low and mid-engined: a long, low, sharp nose, a raked screen, a
        # short roof, a small back light and a long clamshell deck with
        # louvres that falls only gently from the roof to a ducktail
        screen_y=(-0.58, -0.08), glass_y=(-0.48, 0.42), rear_glass_y=(0.46, 0.64),
        roof=[(-0.58, 0.85), (-0.34, 1.05), (-0.08, 1.215), (0.22, 1.245), (0.44, 1.22),
              (0.70, 1.14), (1.20, 1.07), (1.70, 1.02), (1.945, 1.00)],
        ends=dict(nose_top=0.52, nose_w=0.86, tail_top=1.00, tail_bot=0.30, tail_w=0.95),
        hips=(0.03, 0.05), belt_z=0.82, sill_z=0.28,
        shutlines=[(-1.80, "upper"), (-0.62, "upper"), (-0.44, "side"), (0.44, "side"), (0.72, "upper")],
        lamps="round", lamp_x=(0.44, 0.80), lamp_h=0.10, grille="mesh",
        splitter="lip", wing="ducktail", wing_z=1.01, wing_y=1.86, wing_hw=0.86, wing_chord=0.20,
        tail="round", exhaust="twin", scoop=None,
        vents=["deck"], side=[("recess", dict(y0=(0.52, 0.62), y1=(0.86, 0.88), j0=2.3, j1=3.5,
                                              depth=0.050, rim=0.025))],
        skin_box=(-1.98, 1.95, 0.925, 1.26),
        keys=[
            (-1.97, [(0, .20), (.60, .20), (.80, .26), (.86, .38), (.82, .48), (.64, .505), (.42, .51), (.20, .515), (0, .52)]),
            (-1.60, [(0, .12), (.86, .12), (.96, .27), (.99, .48), (.95, .63), (.74, .67), (.48, .68), (.22, .685), (0, .69)]),
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

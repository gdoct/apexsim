"""Build limbotiti-caravan-gt3 (GT3 kit) in Blender: cars/default/limbotiti-caravan-gt3/limbotiti_caravan.glb.

    blender -b --factory-startup --python tools/generators/cars/limbotiti_caravan.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\limbotiti_caravan.py").read())

The data is this car's; the stages are kits/gt3.py's. Replace one by
calling a function of your own in its place (docs/cars.md).
"""
import importlib.util, os, sys

# The content root (apexsim-content): three folders above this script
# (tools/generators/<group>/), else $APEXSIM_CONTENT (a script exec'd in Blender
# has no __file__), else the checkout on E:.
_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))
for _n, _p in (("apex", os.path.join(_ROOT, "tools", "generators", "props", "apex_props.py")),
               ("carlib", os.path.join(_ROOT, "tools", "generators", "cars", "carlib.py"))):
    _s = importlib.util.spec_from_file_location(_n, _p)
    _m = importlib.util.module_from_spec(_s)
    sys.modules[_n] = _m
    _s.loader.exec_module(_m)
import carlib                                              # noqa: E402

kit = carlib.load("kits/gt3")

CAR = dict(
    folder="limbotiti-caravan-gt3", stem="limbotiti_caravan",
    paint=(0.95, 0.66, 0.02), accent=(0.04, 0.04, 0.045), caliper=(0.05, 0.05, 0.05),
    paint_metallic=0.55, number="63",
    rear_number_y=None, cam_x=-0.28,
    axles=(-1.375, 1.375),
    # mid-engined wedge: the driver on the middle of the wheelbase, a
    # low roof over him and a long deck behind (the imported Huracan)
    # the glass where the real car's is (measured off a photo of it): the
    # screen and side glass from the A-pillar's foot, the side window
    # tapering to a point over the intake
    glass_y=(-1.12, 0.80), screen_y=(-1.12, -0.47), rear_glass_y=(0.80, 0.86),
    eye=(0.36, 0.05, 0.92),
    wing="swan", wing_z=1.02, wing_y=1.92, wing_hw=0.88, ducktail=False,
    # a broad plane swept to a V on swan necks, low and hung out past the
    # tail, the brake lights as two upright bars on its endplates
    wing_plan=("swept", 0.08), wing_chord=0.36, wing_aoa=10.0, endplate="swoop", wing_led="endplate",
    sponsors=dict(sun=12, quarter=None, fender=15, ep=13),     # the intake fills the quarter
    ends=dict(nose_top=0.45, tail_top=0.90, tail_bot=0.22),
    valance_z=(0.20, 0.52),            # the tail open from the diffuser to the lamps
    lights="ybar", grille=False, exhaust="hexquad", mirror="stalk",
    # the lamps on the nose's corners, just under the bonnet's leading edge
    lamp_x=(0.38, 0.72), lamp_h=0.085, lamp_z=(0.36, 0.43), raked_nose=True, face="arrow", tail="y",
    shutlines=[(-1.84, "upper"), (-0.94, "side"), (0.44, "side"), (0.86, "upper"), (1.84, "upper")],
    # no deck louvres: the kit's flat blades stand off a cover crowned to a
    # spine like this one
    vents=[], scoop=(0.0, 0.62, 0.075),        # the snorkel at the roof's end
    # the wedge: the intake behind the door, raked back and standing from the
    # sill to the top of the flank, a knife slot behind the front wheel
    # leaning the other way, and the crease between them
    # (j 2 is the sill at 15 cm, j 3 the top of the flank at 58 cm)
    side=[
        ("recess", dict(y0=(0.38, 0.58), y1=(0.80, 0.90), j0=2.05, j1=3.00,
                        depth=0.070, rim=0.025, bow=-0.04)),
        ("recess", dict(y0=(-0.94, -0.86), y1=(-0.84, -0.78), j0=2.35, j1=2.95,
                        depth=0.035, rim=0.016, blades=2, blade_r=0.005)),
        # the door's crease, climbing from the foot of the front arch into the
        # intake's top corner
        ("swage", dict(y0=-0.90, y1=0.50, j_a=2.30, j_b=2.95, depth=0.016,
                       width=0.30, fade=0.20)),
    ],
    decal_j=(2.00, 2.90),
    # Sections measured off a low-poly Huracan STO (CC BY, not in the repo):
    # x and z at 1.5x the model, its axles on ours, the sill below the hubs
    # squashed to an 85 mm floor; the sill and flank top at fixed heights,
    # the shoulder, belt, glass top and roof edge at fixed fractions of the
    # half-width along the top, the arch stations bridged, every point
    # smoothed along the car. Finished: `body_loft` below takes them as they
    # are, without the class's shaping (kit.shape_keys).
    keys=[
        (-2.15, [(0, .085), (.689, .085), (.749, .150), (.710, .410), (.658, .494), (.567, .515), (.415, .544), (.233, .573), (0, .570)]),
        (-2.05, [(0, .085), (.722, .085), (.782, .150), (.754, .441), (.693, .519), (.597, .537), (.435, .564), (.243, .592), (0, .601)]),
        (-1.95, [(0, .085), (.762, .085), (.822, .150), (.810, .483), (.746, .563), (.642, .578), (.466, .602), (.259, .626), (0, .643)]),
        (-1.85, [(0, .085), (.799, .085), (.859, .150), (.857, .526), (.791, .621), (.681, .632), (.494, .653), (.273, .672), (0, .686)]),
        (-1.75, [(0, .085), (.821, .085), (.881, .150), (.883, .562), (.818, .672), (.703, .682), (.510, .704), (.281, .717), (0, .729)]),
        (-1.65, [(0, .085), (.832, .085), (.892, .150), (.894, .578), (.831, .701), (.715, .717), (.518, .742), (.286, .754), (0, .764)]),
        (-1.55, [(0, .085), (.837, .085), (.897, .150), (.903, .580), (.839, .716), (.722, .739), (.523, .768), (.289, .783), (0, .790)]),
        (-1.45, [(0, .085), (.842, .085), (.902, .150), (.910, .580), (.847, .727), (.728, .759), (.528, .792), (.291, .811), (0, .817)]),
        (-1.35, [(0, .085), (.846, .085), (.906, .150), (.918, .580), (.854, .734), (.734, .779), (.532, .820), (.294, .841), (0, .846)]),
        (-1.25, [(0, .085), (.851, .085), (.911, .150), (.926, .580), (.861, .734), (.741, .799), (.537, .852), (.297, .871), (0, .875)]),
        (-1.15, [(0, .085), (.856, .085), (.916, .150), (.934, .580), (.868, .732), (.748, .810), (.543, .885), (.310, .902), (0, .905)]),
        (-1.05, [(0, .085), (.860, .085), (.920, .150), (.941, .580), (.875, .728), (.759, .808), (.552, .921), (.346, .936), (0, .940)]),
        (-0.95, [(0, .085), (.862, .085), (.922, .150), (.946, .580), (.879, .724), (.769, .804), (.561, .957), (.390, .973), (0, .978)]),
        (-0.85, [(0, .085), (.863, .085), (.923, .150), (.947, .580), (.880, .720), (.774, .805), (.566, .988), (.423, 1.004), (0, 1.012)]),
        (-0.75, [(0, .085), (.861, .085), (.921, .150), (.943, .580), (.876, .722), (.773, .813), (.565, 1.016), (.433, 1.033), (0, 1.043)]),
        (-0.65, [(0, .085), (.858, .085), (.918, .150), (.937, .580), (.872, .729), (.769, .826), (.562, 1.043), (.431, 1.063), (0, 1.073)]),
        (-0.55, [(0, .085), (.857, .085), (.917, .150), (.933, .580), (.869, .735), (.766, .834), (.560, 1.061), (.430, 1.090), (0, 1.100)]),
        (-0.45, [(0, .085), (.857, .085), (.917, .150), (.933, .580), (.868, .739), (.765, .836), (.560, 1.065), (.429, 1.105), (0, 1.117)]),
        (-0.35, [(0, .085), (.859, .085), (.919, .150), (.935, .580), (.870, .746), (.767, .834), (.561, 1.064), (.430, 1.112), (0, 1.123)]),
        (-0.25, [(0, .085), (.862, .085), (.922, .150), (.938, .580), (.873, .751), (.769, .830), (.563, 1.060), (.432, 1.116), (0, 1.129)]),
        (-0.15, [(0, .085), (.865, .085), (.925, .150), (.941, .580), (.875, .754), (.772, .827), (.565, 1.057), (.433, 1.121), (0, 1.134)]),
        (-0.05, [(0, .085), (.868, .085), (.928, .150), (.944, .580), (.878, .755), (.774, .826), (.566, 1.055), (.434, 1.123), (0, 1.137)]),
        (0.05, [(0, .085), (.870, .085), (.930, .150), (.946, .580), (.879, .759), (.775, .828), (.567, 1.056), (.435, 1.122), (0, 1.137)]),
        (0.15, [(0, .085), (.871, .085), (.931, .150), (.946, .580), (.880, .764), (.776, .834), (.568, 1.061), (.435, 1.117), (0, 1.133)]),
        (0.25, [(0, .085), (.872, .085), (.932, .150), (.947, .580), (.881, .771), (.777, .843), (.568, 1.065), (.436, 1.111), (0, 1.127)]),
        (0.35, [(0, .085), (.873, .085), (.933, .150), (.948, .580), (.881, .777), (.777, .852), (.568, 1.070), (.436, 1.105), (0, 1.122)]),
        (0.45, [(0, .085), (.874, .085), (.934, .150), (.948, .580), (.881, .785), (.777, .862), (.569, 1.072), (.435, 1.098), (0, 1.117)]),
        (0.55, [(0, .085), (.874, .085), (.934, .150), (.947, .580), (.881, .795), (.777, .876), (.570, 1.067), (.429, 1.087), (0, 1.114)]),
        (0.65, [(0, .085), (.875, .085), (.935, .150), (.945, .580), (.881, .804), (.777, .897), (.576, 1.046), (.410, 1.065), (0, 1.114)]),
        (0.75, [(0, .085), (.875, .085), (.935, .150), (.941, .580), (.880, .806), (.777, .913), (.584, 1.012), (.386, 1.041), (0, 1.106)]),
        (0.85, [(0, .085), (.874, .085), (.934, .150), (.938, .580), (.879, .806), (.776, .917), (.591, .975), (.362, 1.016), (0, 1.087)]),
        (0.95, [(0, .085), (.872, .085), (.932, .150), (.935, .580), (.878, .805), (.775, .911), (.596, .940), (.343, .989), (0, 1.061)]),
        (1.05, [(0, .085), (.870, .085), (.930, .150), (.931, .580), (.875, .800), (.773, .894), (.596, .916), (.336, .966), (0, 1.031)]),
        (1.15, [(0, .085), (.868, .085), (.928, .150), (.927, .580), (.872, .791), (.770, .876), (.594, .904), (.334, .954), (0, .997)]),
        (1.25, [(0, .085), (.866, .085), (.926, .150), (.924, .580), (.868, .787), (.767, .860), (.591, .897), (.333, .939), (0, .959)]),
        (1.35, [(0, .085), (.864, .085), (.924, .150), (.920, .580), (.865, .785), (.764, .846), (.589, .888), (.331, .919), (0, .928)]),
        (1.45, [(0, .085), (.862, .085), (.922, .150), (.917, .580), (.862, .785), (.761, .839), (.587, .876), (.330, .907), (0, .913)]),
        (1.55, [(0, .085), (.858, .085), (.918, .150), (.913, .580), (.857, .787), (.757, .842), (.584, .869), (.328, .901), (0, .907)]),
        (1.65, [(0, .085), (.840, .085), (.900, .150), (.906, .580), (.849, .799), (.749, .848), (.578, .866), (.325, .898), (0, .914)]),
        (1.75, [(0, .085), (.785, .085), (.845, .150), (.886, .580), (.828, .812), (.731, .850), (.563, .866), (.317, .892), (0, .913)]),
        (1.85, [(0, .085), (.687, .085), (.747, .150), (.843, .580), (.794, .818), (.701, .847), (.540, .866), (.304, .885), (0, .893)]),
        (1.95, [(0, .085), (.580, .085), (.640, .150), (.795, .580), (.766, .824), (.676, .844), (.521, .865), (.293, .881), (0, .879)]),
    ])



def body_loft():
    """The kit's stage on the fitted keys: widened to the class's 2.0 m over
    the arches, the fenders lifted 3 cm over the hubs for the arch lip, both
    ends rounded off in two tip stations, and lofted finer than the drawn
    cars (240 stations) so the glass edges stay clean. Creased only at the
    sill (j 2) and the shoulder (j 4): the fitted points lie on smooth
    curves, and creasing all four drawn creases stepped the flank."""
    keys = CAR["keys"]
    keys = carlib.scale_width(keys, 1.0 / max(x for (_, pts) in keys for (x, _) in pts))
    keys = carlib.fender_bump(keys, CAR["axles"], amount=0.03, width=0.55, j0=2.2, j1=4.3)
    # the front overhang to the real car's: the stations ahead of the arch
    # drawn out half as long again, so the nose runs out low and pointed
    keys = [(y if y >= -1.80 else -1.80 + (y + 1.80) * 1.5, pts) for (y, pts) in keys]
    # and the bonnet run down to the real car's leading edge (0.47 m): ahead of
    # y -1.9 every point drops by up to 10 cm, in proportion to its height,
    # so the floor and the chin stay where they are
    def drop(y, pts):
        d = 0.10 * carlib._smoothstep((-1.90 - y) / 0.45)
        zb, zt = min(z for (_, z) in pts), max(z for (_, z) in pts)
        return [(x, z - d * (z - zb) / (zt - zb)) for (x, z) in pts]
    keys = [(y, drop(y, pts)) for (y, pts) in keys]
    k0, k1 = keys[0][0], keys[-1][0]
    # the model's own bumper face is too crude to copy: the nose is the first
    # fitted section drawn forward, its top dropping and the chin running on
    # low, so the face rakes forward from the bonnet's leading edge to the
    # splitter as the real car's does (a tip shrunk about its middle made a
    # bulb); the tail is the last section drawn back
    src = keys[0][1]
    zb = min(z for (_, z) in src)
    for (dy, xs, zs) in ((0.06, 0.95, 0.78), (0.12, 0.86, 0.42)):
        keys = [(k0 - dy, [(x * xs, zb + (z - zb) * zs) for (x, z) in src])] + keys
    for (y, xs, zs) in ((k1 + 0.03, 0.94, 0.92), (k1 + 0.06, 0.80, 0.74)):
        keys = carlib.tip_station(keys, k1, y, x_scale=xs, z_shrink=zs)
    # 14 samples per control span round the section (the kit's 6 drew the
    # intake's raked edges and the door crease as stairs)
    kit.SAMP = 14
    kit.body_loft(keys, faces=(k0 - 0.06, k1), ny=240, hard=(2, 4))


kit.start(CAR)
kit.materials()
body_loft()
kit.arches()
kit.apertures()
kit.begin_parts()
kit.floor()
kit.wing()
kit.lights()
kit.tail_lights()
kit.vent_blades()
kit.fittings()
kit.hardware()
kit.numbers()
kit.cockpit()
kit.decals()
kit.driver()
kit.export()

"""Build murcetes-amd-gt3 (GT3 kit) in Blender: cars/default/murcetes-amd-gt3/murcetes_amd_gt3.glb.

    blender -b --factory-startup --python tools/generators/cars/murcetes_amd_gt3.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\murcetes_amd_gt3.py").read())

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
    folder="murcetes-amd-gt3", stem="murcetes_amd_gt3", logo="murcetes_logo.png",
    paint=(0.56, 0.57, 0.60), accent=(0.0, 0.62, 0.60), caliper=(0.85, 0.10, 0.05),
    paint_metallic=0.90, number="88", roof_factor=1.0, hips=(0.035, 0.050),
    rear_number_y=(1.88, 2.14), cam_x=0.0,
    axles=(-1.375, 1.375),
    # the long-bonnet GT, cab rearward as the real thing is: the screen
    # base 0.45 m ahead of the middle of the wheelbase, the roof peaking
    # 0.9 m behind it and running down in one fastback to the deck
    glass_y=(-0.45, 1.05), screen_y=(-0.45, 0.30), rear_glass_y=(1.05, 1.80),
    eye=(0.36, 0.60, 0.97),
    logo_y=(-0.30, 0.75), bonnet_drop=None,
    wing="pylon", wing_z=1.16, wing_y=1.95, wing_hw=0.86, ducktail=False,
    # a broad spoon-shaped plane on wide-set pylons, rounded endplates,
    # the brake strip only across the middle third
    wing_plan=("spoon", 0.045), wing_chord=0.38, wing_aoa=7.0, endplate="round", wing_led="centre",
    sponsors=dict(sun=10, quarter=3, fender=15, ep=8),
    pylon_x=0.66,
    ends=dict(nose_top=0.60, nose_w=0.88, tail_top=0.86, tail_bot=0.32, tail_w=0.93),
    lights="slant", grille=True, exhaust="side", mirror="pod",
    lamp_x=(0.50, 0.84), lamp_h=0.150, face="shield", tail="wrap",
    shutlines=[(-1.86, "upper"), (-0.52, "upper"), (-0.40, "side"), (0.78, "side"), (1.84, "upper")],
    vents=["bonnet_louvres"], scoop=None,
    # the long-bonnet GT: three gills behind the front wheel raked back
    # like a shark's, a brake exit ahead of the rear wheel leaning the
    # other way, and a shoulder line that runs back and up onto the hips
    side=[("recess", dict(y0=(-0.955 + 0.075 * k, -0.875 + 0.075 * k),
                          y1=(-0.915 + 0.075 * k, -0.835 + 0.075 * k), j0=2.30, j1=3.45,
                          depth=0.034, rim=0.012)) for k in range(3)] + [
        ("recess", dict(y0=(0.84, 0.78), y1=(0.975, 0.93), j0=2.10, j1=3.25,
                        depth=0.045, rim=0.020, blades=3, blade_r=0.006)),
        ("swage", dict(y0=-0.75, y1=1.00, j_a=3.55, j_b=3.92, depth=0.008,
                       width=0.30, fade=0.30)),
    ],
    logo_j=(2.05, 3.05),
    # Sections measured off the imported AMG GT3 (pass 5): 2.0 m over the
    # arches, a flank that stands near-vertical to a belt at ~0.95, fender
    # crowns at 0.80 a hand inboard of the flank, the bonnet in a valley
    # between them, a 1.25 m roof peaking behind the B-pillar.
    keys=[
        (-2.30, [(0, .215), (.58, .215), (.74, .33), (.80, .52), (.66, .58), (.46, .60), (.28, .61), (.12, .62), (0, .62)]),
        (-1.90, [(0, .095), (.86, .095), (.94, .34), (.96, .62), (.84, .70), (.62, .68), (.42, .66), (.18, .66), (0, .66)]),
        (-1.375, [(0, .075), (.90, .075), (.975, .38), (.99, .72), (.88, .80), (.66, .73), (.44, .705), (.20, .70), (0, .70)]),
        (-0.80, [(0, .075), (.90, .075), (.975, .38), (.985, .72), (.885, .85), (.70, .81), (.47, .80), (.22, .80), (0, .80)]),
        (-0.45, [(0, .075), (.90, .075), (.975, .38), (.985, .73), (.89, .84), (.72, .88), (.50, .80), (.24, .83), (0, .84)]),
        (-0.10, [(0, .075), (.90, .075), (.975, .38), (.985, .74), (.90, .86), (.74, .92), (.56, .98), (.30, 1.04), (0, 1.06)]),
        (0.40, [(0, .075), (.92, .075), (.99, .40), (1.00, .76), (.92, .90), (.76, .96), (.60, 1.12), (.33, 1.19), (0, 1.21)]),
        (0.95, [(0, .075), (.94, .075), (1.00, .42), (1.01, .78), (.93, .92), (.77, .98), (.60, 1.14), (.33, 1.22), (0, 1.25)]),
        (1.375, [(0, .075), (.94, .075), (1.00, .42), (1.015, .78), (.93, .91), (.75, .96), (.56, 1.08), (.30, 1.15), (0, 1.17)]),
        (1.90, [(0, .145), (.90, .145), (.975, .42), (.985, .76), (.90, .86), (.70, .90), (.48, .96), (.24, 1.00), (0, 1.02)]),
        (2.35, [(0, .33), (.80, .33), (.88, .49), (.90, .70), (.78, .80), (.58, .85), (.38, .87), (.16, .88), (0, .88)]),
    ])

kit.start(CAR)
kit.materials()
kit.body_loft()
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
kit.wordmark()
kit.driver()
kit.export()

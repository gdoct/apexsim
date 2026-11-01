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
    paint_metallic=0.55, number="63", roof_factor=1.0, hips=(0.060, 0.060),
    rear_number_y=None, cam_x=-0.28,
    axles=(-1.375, 1.375),
    # mid-engined wedge: the driver on the middle of the wheelbase, a
    # low roof over him and a long deck behind (the imported Huracan)
    glass_y=(-0.95, 0.55), screen_y=(-1.02, -0.47), rear_glass_y=(0.62, 0.80),
    eye=(0.36, 0.05, 0.92),
    wing="pylon", wing_z=1.14, wing_y=1.80, wing_hw=0.88, ducktail=False,
    # a short, steep plane swept to a V, faceted endplates carrying the
    # brake lights as two upright bars, on narrow pylons
    wing_plan=("swept", 0.10), wing_chord=0.30, wing_aoa=12.0, endplate="facet", wing_led="endplate",
    sponsors=dict(sun=12, quarter=9, fender=15, ep=13),
    pylon_x=0.40,
    ends=dict(nose_top=0.45, nose_w=1.0, tail_top=0.91, tail_bot=0.22, tail_w=1.0),
    lights="ybar", grille=False, exhaust="hexquad", mirror="stalk",
    lamp_x=(0.46, 0.86), lamp_h=0.085, face="arrow", tail="y",
    shutlines=[(-1.84, "upper"), (-0.94, "side"), (0.56, "side"), (0.86, "upper"), (1.84, "upper")],
    vents=["deck_louvres"], scoop=(0.0, 0.26, 0.13),
    # the wedge: a hexagonal intake behind the door raked hard back, a
    # knife slot behind the front wheel leaning the other way, and a
    # crease that climbs from the front arch into the intake's top corner
    side=[
        ("recess", dict(y0=(0.40, 0.80), y1=(0.98, 1.005), j0=1.95, j1=3.65,
                        depth=0.065, rim=0.050, bow=-0.04, blades=3, blade_r=0.008)),
        ("recess", dict(y0=(-0.95, -0.84), y1=(-0.85, -0.76), j0=2.25, j1=3.40,
                        depth=0.040, rim=0.018, blades=2, blade_r=0.005)),
        ("swage", dict(y0=-0.98, y1=0.74, j_a=3.00, j_b=3.62, depth=0.013,
                       width=0.34, fade=0.20)),
    ],
    decal_j=(2.00, 2.90),
    keys=[
        (-2.25, [(0, .075), (.440, .075), (.500, .110), (.530, .260), (.440, .360), (.330, .405), (.220, .430), (.110, .443), (0, .450)]),
        (-2.05, [(0, .075), (.690, .075), (.750, .120), (.755, .400), (.700, .460), (.600, .490), (.450, .515), (.250, .545), (0, .570)]),
        (-1.8, [(0, .075), (.840, .075), (.900, .120), (.912, .450), (.870, .585), (.750, .634), (.550, .652), (.300, .672), (0, .686)]),
        (-1.375, [(0, .075), (.870, .075), (.930, .120), (.935, .620), (.890, .700), (.760, .762), (.550, .800), (.300, .828), (0, .836)]),
        (-1.0, [(0, .075), (.870, .075), (.930, .120), (.960, .600), (.900, .700), (.780, .790), (.600, .930), (.350, .955), (0, .958)]),
        (-0.65, [(0, .075), (.860, .075), (.920, .120), (.940, .600), (.885, .700), (.770, .820), (.550, 1.030), (.420, 1.070), (0, 1.080)]),
        (-0.3, [(0, .075), (.865, .075), (.925, .120), (.945, .580), (.890, .700), (.780, .820), (.560, 1.075), (.440, 1.125), (0, 1.136)]),
        (0.0, [(0, .075), (.870, .075), (.930, .120), (.952, .580), (.900, .700), (.790, .820), (.560, 1.085), (.440, 1.138), (0, 1.151)]),
        (0.3, [(0, .075), (.875, .075), (.935, .120), (.955, .600), (.920, .740), (.790, .860), (.570, 1.070), (.450, 1.120), (0, 1.132)]),
        (0.6, [(0, .075), (.875, .075), (.935, .120), (.953, .620), (.905, .800), (.760, .930), (.650, 1.010), (.480, 1.090), (0, 1.118)]),
        (0.85, [(0, .075), (.875, .075), (.935, .120), (.935, .650), (.905, .760), (.840, .850), (.700, .955), (.400, 1.020), (0, 1.070)]),
        (1.1, [(0, .075), (.870, .075), (.930, .120), (.930, .650), (.900, .770), (.820, .860), (.620, .900), (.350, .955), (0, .990)]),
        (1.375, [(0, .075), (.865, .075), (.925, .125), (.920, .660), (.880, .770), (.760, .840), (.580, .878), (.320, .910), (0, .930)]),
        (1.65, [(0, .075), (.860, .075), (.920, .140), (.935, .640), (.880, .760), (.760, .830), (.580, .865), (.320, .890), (0, .905)]),
        (1.85, [(0, .075), (.740, .075), (.800, .170), (.850, .550), (.800, .740), (.740, .830), (.560, .860), (.300, .875), (0, .880)]),
        (1.98, [(0, .075), (.340, .075), (.400, .240), (.520, .500), (.620, .740), (.600, .860), (.470, .900), (.260, .910), (0, .910)]),
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
kit.decals()
kit.driver()
kit.export()

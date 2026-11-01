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
    folder="limbotiti-caravan-gt3", stem="limbotiti_caravan", logo="limbotiti_logo.png",
    paint=(0.95, 0.66, 0.02), accent=(0.04, 0.04, 0.045), caliper=(0.05, 0.05, 0.05),
    paint_metallic=0.55, number="63", roof_factor=1.0, hips=(0.050, 0.080),
    rear_number_y=None, cam_x=-0.28, bonnet_drop=(0.035, 0.09),
    axles=(-1.375, 1.375),
    # mid-engined wedge: the driver on the middle of the wheelbase, a
    # low roof over him and a flat deck behind (the imported Huracan)
    glass_y=(-0.98, 0.50), screen_y=(-1.00, -0.40), rear_glass_y=(0.30, 0.74),
    eye=(0.36, 0.05, 0.92),
    roof=[(-1.00, 0.72), (-0.65, 0.95), (-0.30, 1.12), (0.0, 1.17), (0.30, 1.15),
          (0.85, 1.02), (1.375, 0.88), (1.85, 0.86)],
    belt_lift=((3, 0.03), (4, 0.04), (5, 0.04)), belt_y=(-1.1, 1.7),
    wing="pylon", wing_z=1.14, wing_y=1.80, wing_hw=0.88, ducktail=False,
    # a short, steep plane swept to a V, faceted endplates carrying the
    # brake lights as two upright bars, on narrow pylons
    wing_plan=("swept", 0.10), wing_chord=0.30, wing_aoa=12.0, endplate="facet", wing_led="endplate",
    sponsors=dict(sun=12, quarter=9, fender=15, ep=13),
    pylon_x=0.40,
    ends=dict(nose_top=0.52, nose_w=0.92, tail_top=0.84, tail_bot=0.30, tail_w=0.95),
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
    logo_j=(2.00, 2.90), logo_y=(-0.66, 0.26),
    keys=[
        (-2.25, [(0, .155), (.70, .155), (.855, .255), (.915, .415), (.755, .485), (.52, .515), (.31, .535), (.14, .545), (0, .55)]),
        (-1.85, [(0, .095), (.905, .095), (.975, .315), (1.00, .555), (.845, .665), (.63, .685), (.43, .66), (.20, .65), (0, .65)]),
        (-1.375, [(0, .075), (.925, .075), (1.00, .355), (1.015, .635), (.865, .755), (.655, .765), (.445, .705), (.20, .68), (0, .67)]),
        (-1.00, [(0, .075), (.925, .075), (1.00, .355), (1.005, .655), (.885, .765), (.765, .765), (.60, .725), (.30, .705), (0, .70)]),
        (-0.65, [(0, .075), (.925, .075), (1.00, .355), (1.005, .655), (.885, .785), (.77, .845), (.61, .895), (.31, .935), (0, .95)]),
        (-0.30, [(0, .075), (.925, .075), (1.00, .355), (1.005, .675), (.895, .805), (.785, .865), (.625, 1.095), (.32, 1.145), (0, 1.16)]),
        (0.30, [(0, .075), (.945, .075), (1.02, .375), (1.025, .695), (.925, .825), (.805, .885), (.625, 1.075), (.32, 1.125), (0, 1.14)]),
        (0.85, [(0, .075), (.965, .075), (1.04, .395), (1.045, .715), (.925, .835), (.765, .885), (.545, .905), (.26, .925), (0, .93)]),
        (1.375, [(0, .075), (.965, .075), (1.04, .395), (1.055, .735), (.925, .835), (.745, .865), (.505, .875), (.24, .875), (0, .88)]),
        (1.85, [(0, .135), (.945, .135), (1.02, .395), (1.025, .715), (.905, .815), (.705, .845), (.465, .855), (.22, .855), (0, .86)]),
        (2.25, [(0, .315), (.845, .315), (.925, .495), (.945, .675), (.805, .775), (.605, .815), (.385, .835), (.16, .835), (0, .84)]),
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

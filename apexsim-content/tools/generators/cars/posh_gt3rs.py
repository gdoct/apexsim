"""Build posh-gt3rs (GT3 kit) in Blender: cars/default/posh-gt3rs/posh_gt3rs.glb.

    blender -b --factory-startup --python tools/generators/cars/posh_gt3rs.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\posh_gt3rs.py").read())

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
    folder="posh-gt3rs", stem="posh_gt3rs", logo="posh_logo.png",
    paint=(0.02, 0.10, 0.42), accent=(0.86, 0.87, 0.90), caliper=(0.95, 0.75, 0.05),
    paint_metallic=0.78, number="91", roof_factor=1.0, hips=(0.050, 0.075),
    rear_number_y=(1.30, 1.62), cam_x=0.0, bonnet_drop=(0.075, 0.13),
    axles=(-1.375, 1.375),
    # rear-engined: the cabin far forward, the screen base 0.85 m ahead of
    # the middle, a roof peaking just behind it, a long fastback to a
    # short deck (measured against the imported 911 GT3 R)
    glass_y=(-0.85, 0.95), screen_y=(-0.85, -0.25), rear_glass_y=(0.55, 1.05),
    eye=(0.36, 0.33, 0.97),
    roof=[(-0.85, 0.80), (-0.45, 1.02), (0.0, 1.20), (0.35, 1.245), (0.80, 1.20),
          (1.20, 1.07), (1.60, 0.95), (1.90, 0.90)],
    belt_lift=((3, 0.04), (4, 0.05), (5, 0.05)), belt_y=(-1.0, 1.7),
    wing="swan", wing_z=1.19, wing_y=1.86, wing_hw=0.86, ducktail=True,
    # an arched plane on swan necks over the ducktail, endplates that
    # sweep up to a tall trailing corner, the brake strip along the edge
    wing_plan=("arch", 0.030), wing_chord=0.345, wing_aoa=9.0, endplate="swoop", wing_led="trail",
    sponsors=dict(sun=0, quarter=6, fender=14, ep=2),
    ends=dict(nose_top=0.58, nose_w=0.90, tail_top=0.84, tail_bot=0.30, tail_w=0.94),
    lights="round", grille=False, exhaust="centre", mirror="pod",
    lamp_x=(0.52, 0.82), lamp_h=0.125, face="tri", tail="bar",
    shutlines=[(-1.82, "upper"), (-0.94, "side"), (0.58, "side"), (1.24, "upper"), (1.84, "upper")],
    vents=["naca_front"], scoop=None,
    # a rear-engined coupe breathes ahead of the rear wheel: one big
    # quarter intake whose leading edge sweeps back as it climbs, louvres
    # let into the top of the front fenders, and a waist line that dips
    # over the door and lifts onto the hips
    side=[
        ("recess", dict(y0=(0.58, 0.80), y1=(0.955, 0.975), j0=2.2, j1=3.55,
                        depth=0.055, rim=0.030, bow=-0.02, blades=3)),
        ("recess", dict(y0=(-1.28, -1.20), y1=(-0.99, -0.95), j0=3.35, j1=4.15,
                        depth=0.030, rim=0.016, blades=5, blade_r=0.005)),
        ("swage", dict(y0=-0.98, y1=0.62, j_a=3.55, j_mid=3.30, j_b=3.62, depth=0.007,
                       width=0.30, fade=0.30)),
    ],
    logo_j=(2.05, 3.05), logo_y=(-0.62, 0.46),
    keys=[
        (-2.25, [(0, .195), (.55, .195), (.72, .31), (.79, .50), (.66, .60), (.46, .655), (.30, .68), (.14, .695), (0, .70)]),
        (-1.90, [(0, .095), (.86, .095), (.955, .35), (.985, .625), (.845, .735), (.60, .755), (.40, .74), (.18, .74), (0, .74)]),
        (-1.375, [(0, .075), (.90, .075), (.995, .40), (1.015, .685), (.875, .795), (.63, .80), (.42, .78), (.18, .78), (0, .78)]),
        (-0.95, [(0, .075), (.90, .075), (.985, .40), (.99, .665), (.855, .775), (.67, .795), (.46, .82), (.20, .82), (0, .82)]),
        (-0.60, [(0, .075), (.90, .075), (.985, .40), (.99, .665), (.86, .765), (.71, .885), (.52, 1.035), (.30, 1.095), (0, 1.115)]),
        (-0.20, [(0, .075), (.90, .075), (.985, .40), (.99, .665), (.865, .765), (.725, .905), (.56, 1.135), (.32, 1.265), (0, 1.30)]),
        (0.40, [(0, .075), (.925, .075), (1.005, .40), (1.015, .685), (.885, .79), (.745, .915), (.58, 1.10), (.32, 1.235), (0, 1.275)]),
        (1.00, [(0, .075), (.945, .075), (1.025, .42), (1.045, .725), (.905, .835), (.745, .895), (.52, 1.00), (.28, 1.095), (0, 1.13)]),
        (1.375, [(0, .075), (.945, .075), (1.025, .42), (1.05, .745), (.905, .855), (.72, .895), (.48, .955), (.24, 1.015), (0, 1.04)]),
        (1.90, [(0, .135), (.90, .135), (.985, .42), (1.005, .725), (.865, .835), (.66, .895), (.44, .935), (.20, .975), (0, 1.00)]),
        (2.30, [(0, .295), (.78, .295), (.865, .48), (.885, .695), (.765, .815), (.58, .875), (.38, .915), (.16, .945), (0, .95)]),
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

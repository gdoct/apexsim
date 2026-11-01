"""Build panini-zomba-hypercar (Hypercar kit) in Blender: cars/default/panini-zomba-hypercar/panini_zomba.glb.

    blender -b --factory-startup --python tools/generators/cars/panini_zomba.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\panini_zomba.py").read())

The data is this car's; the stages are kits/hypercar.py's. Replace one by
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

kit = carlib.load("kits/hypercar")

CAR = dict(folder="panini-zomba-hypercar", stem="panini_zomba", logo="panini_logo.png",
    paint=(0.020, 0.045, 0.110), accent=(0.78, 0.79, 0.82), caliper=(0.85, 0.86, 0.88),
    paint_metallic=0.85, number="9", bonnet_drop=(0.015, 0.035),
    lights="round", drl="points", tail="rings", exhaust="quad", grille="twin",
    nose_w=0.94, fender=1.02, roof=1.00, canopy_shift=0.04, tail_h=1.00, wing_z=0.02,
    mirror="stalk", scoop=(0.22, 0.60, 0.13), seat=(0.36, 0.17, 0.06), rain_z=(0.420, 0.550),
    # upright and square: a raised nose, shallow valleys, a wide
    # canopy, fenders that stand like boxes over the wheels
    nose_z=0.060, valley=0.065, canopy_w=1.07, fender_r=1.00, face="twin", side="upright",
    # a straight plane on square endplates, brake strip along the flap
    wing_plan=("straight", 0.0), endplate="square", wing_led="trail",
    sponsors=dict(door=7, rear=2, nose=6, wing=0, ep=14))

kit.start(CAR)
kit.materials()
kit.body_loft()
kit.arches()
kit.apertures()
kit.begin_parts()
kit.floor()
kit.wing()
kit.headlamps()
kit.mouths()
kit.horseshoe()
kit.c_line()
kit.vent_blades()
kit.exhausts()
kit.fittings()
kit.hardware()
kit.numbers()
kit.cockpit()
kit.decals()
kit.driver()
kit.export()

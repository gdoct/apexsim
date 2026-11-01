"""Build bugotti-chiffon-hypercar (Hypercar kit) in Blender: cars/default/bugotti-chiffon-hypercar/bugotti_chiffon.glb.

    blender -b --factory-startup --python tools/generators/cars/bugotti_chiffon.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\bugotti_chiffon.py").read())

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

CAR = dict(folder="bugotti-chiffon-hypercar", stem="bugotti_chiffon", logo="bugotti_logo.png",
    paint=(0.015, 0.085, 0.42), accent=(0.004, 0.006, 0.018), accent_metallic=0.25, caliper=(0.05, 0.25, 0.80),
    paint_metallic=0.80, number="16", bonnet_drop=(0.015, 0.035),
    lights="quad", drl="under", tail="fullbar", exhaust="twin", grille="horseshoe",
    nose_w=1.00, fender=1.00, roof=1.02, canopy_shift=0.00, tail_h=1.02, wing_z=0.03,
    mirror="pod", scoop=None, seat=(0.04, 0.10, 0.30), cline=True, two_tone=True, rain_z=(0.285, 0.430),
    # round and full: bulbous fenders, a filled-in valley, the
    # C-shaped intake the polished line wraps
    nose_z=0.010, valley=0.020, canopy_w=1.00, fender_r=0.98, fender_x=0.025,
    face="horseshoe", side="c",
    # an arched plane, endplates with a rounded top rolling
    # over it, brake light across the middle
    wing_plan=("arch", 0.040), endplate="roll", wing_led="centre",
    sponsors=dict(door=10, rear=8, nose=14, wing=6, ep=2))

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

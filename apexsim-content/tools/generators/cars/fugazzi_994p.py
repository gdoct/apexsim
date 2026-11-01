"""Build fugazzi-994p-hypercar (Hypercar kit) in Blender: cars/default/fugazzi-994p-hypercar/fugazzi_994p.glb.

    blender -b --factory-startup --python tools/generators/cars/fugazzi_994p.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\fugazzi_994p.py").read())

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

CAR = dict(folder="fugazzi-994p-hypercar", stem="fugazzi_994p", logo="fugazzi_hyper_logo.png",
    paint=(0.60, 0.015, 0.025), accent=(0.96, 0.78, 0.04), caliper=(0.96, 0.80, 0.05),
    paint_metallic=0.60, number="50", bonnet_drop=(0.015, 0.035),
    lights="slit", drl="blade", tail="double", exhaust="twin", grille="twin",
    nose_w=0.88, fender=1.05, roof=0.96, canopy_shift=0.10, tail_h=0.96, wing_z=0.00,
    mirror="stalk", scoop=None, seat=(0.16, 0.04, 0.04),
    # low and pointed: the nose drooped, the valleys cut deep
    # either side of a narrow canopy, rear fenders swept up
    nose_z=-0.030, valley=-0.055, canopy_w=0.95, fender_r=1.07, face="boomerang",
    side="slash",
    # a spoon-shaped plane between endplates that rise to a
    # swept-back horn, the brake light up their trailing edges
    wing_plan=("spoon", 0.055), endplate="horn", wing_led="endplate",
    sponsors=dict(door=9, rear=4, nose=15, wing=8, ep=14))

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

"""Build fugazzi-lmp2 (LMP2 kit) in Blender: cars/default/fugazzi-lmp2/fugazzi_lmp2.glb.

    blender -b --factory-startup --python tools/generators/cars/fugazzi_lmp2.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\fugazzi_lmp2.py").read())

The data is this car's; the stages are kits/lmp2.py's. Replace one by
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

kit = carlib.load("kits/lmp2")

CAR = dict(folder="fugazzi-lmp2", stem="fugazzi_lmp2", logo="fugazzi_logo.png",
    paint=(0.62, 0.02, 0.03), accent=(0.95, 0.78, 0.05), caliper=(0.95, 0.80, 0.05),
    paint_metallic=0.65, number="51", bonnet_drop=(0.02, 0.05), drl="blade", tail="rings",
    nose_w=0.90, fender=1.05, roof=1.00, canopy_shift=0.12, tail_h=1.04,
    side_w=0.99, wing_z=0.02, fin=True, lights="tri", mirror="stalk",
    scoop=(0.26, 0.68, 0.095), seat=(0.16, 0.05, 0.05),
    # sharp: drooped nose, deep valleys, one boomerang mouth,
    # the long raked slash of the marque's hypercar
    nose_z=-0.025, valley=-0.045, face="boomerang", side="slash",
    lamp_x=(0.22, 0.66), lamp_h=0.070,
    # a spoon-shaped plane hung under two pylons, raked
    # endplates with the brake lights running up their backs
    wing_plan=("spoon", 0.050), endplate="raked", mount="pylon", wing_led="endplate",
    sponsors=dict(fender=14, wing=8, ep=4))

kit.start(CAR)
kit.materials()
kit.body_loft()
kit.arches()
kit.apertures()
kit.begin_parts()
kit.floor()
kit.wing()
kit.lights()
kit.vent_blades()
kit.fittings()
kit.drl_blades()
kit.hardware()
kit.numbers()
kit.cockpit()
kit.wordmark()
kit.driver()
kit.export()

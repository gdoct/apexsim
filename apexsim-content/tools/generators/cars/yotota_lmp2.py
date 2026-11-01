"""Build yotota-lmp2 (LMP2 kit) in Blender: cars/default/yotota-lmp2/yotota_lmp2.glb.

    blender -b --factory-startup --python tools/generators/cars/yotota_lmp2.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\yotota_lmp2.py").read())

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

CAR = dict(folder="yotota-lmp2", stem="yotota_lmp2", logo="yotota_logo.png",
    paint=(0.90, 0.90, 0.88), accent=(0.80, 0.04, 0.04), caliper=(0.85, 0.10, 0.05),
    paint_metallic=0.45, number="7", bonnet_drop=(0.02, 0.05), drl="T", tail="double",
    nose_w=1.00, fender=1.00, roof=1.00, canopy_shift=0.00, tail_h=1.00,
    side_w=1.00, wing_z=0.00, fin=True, lights="tri", mirror="pod",
    scoop=(0.30, 0.62, 0.09), seat=(0.10, 0.10, 0.32),
    # the plain one: square twin mouths, an upright intake
    nose_z=0.0, valley=0.0, face="twin", side="upright",
    # square endplates, a straight two-element wing on swan necks
    wing_plan=("straight", 0.0), endplate="square", mount="swan", wing_led="trail",
    sponsors=dict(fender=15, wing=2, ep=12))

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

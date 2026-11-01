"""Build jeanetti-lmp2 (LMP2 kit) in Blender: cars/default/jeanetti-lmp2/jeanetti_lmp2.glb.

    blender -b --factory-startup --python tools/generators/cars/jeanetti_lmp2.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\jeanetti_lmp2.py").read())

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

CAR = dict(folder="jeanetti-lmp2", stem="jeanetti_lmp2", logo="jeanetti_logo.png",
    paint=(0.02, 0.20, 0.10), accent=(0.95, 0.82, 0.18), caliper=(0.20, 0.20, 0.22),
    paint_metallic=0.60, number="38", bonnet_drop=(0.02, 0.05), drl="claws", tail="claws",
    nose_w=1.00, fender=0.98, roof=1.03, canopy_shift=0.05, tail_h=1.00,
    side_w=1.02, wing_z=0.05, fin=False, lights="bar", mirror="pod",
    scoop=(0.32, 0.52, 0.10), seat=(0.08, 0.10, 0.06),
    # clawed: two tall mouths leaning in at the top, three
    # gills behind the front wheel, an intake leaning forward
    nose_z=0.012, valley=0.010, face="claw", side="gills",
    lamp_x=(0.30, 0.66), lamp_h=0.090,
    # a V-swept plane, louvred endplates, one central swan
    # neck behind the fin, brake light across the middle
    wing_plan=("swept", 0.12), endplate="louvred", mount="centre", wing_led="centre",
    sponsors=dict(fender=14, wing=4, ep=10))

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

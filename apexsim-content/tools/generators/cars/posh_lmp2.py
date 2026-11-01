"""Build posh-lmp2 (LMP2 kit) in Blender: cars/default/posh-lmp2/posh_lmp2.glb.

    blender -b --factory-startup --python tools/generators/cars/posh_lmp2.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\posh_lmp2.py").read())

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

CAR = dict(folder="posh-lmp2", stem="posh_lmp2", logo="posh_logo.png",
    paint=(0.50, 0.51, 0.54), accent=(0.04, 0.04, 0.045), caliper=(0.95, 0.75, 0.05),
    paint_metallic=0.90, number="22", bonnet_drop=(0.02, 0.05), drl="points", tail="bar",
    nose_w=1.06, fender=1.03, roof=0.97, canopy_shift=-0.10, tail_h=0.94,
    side_w=1.00, wing_z=-0.04, fin=True, lights="round", mirror="pod",
    scoop=(0.34, 0.56, 0.085), seat=(0.10, 0.10, 0.12),
    # smooth: filled-in valleys, a centre mouth between corner
    # intakes, an intake that sweeps back under a waist line
    nose_z=0.020, valley=0.035, face="tri", side="sweep",
    lamp_x=(0.24, 0.56), lamp_h=0.130,
    # an arched plane, endplates swept up to a tall trailing
    # corner, swan necks set wide
    wing_plan=("arch", 0.035), endplate="swoop", mount="swan_wide", wing_led="trail",
    sponsors=dict(fender=14, wing=0, ep=8))

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

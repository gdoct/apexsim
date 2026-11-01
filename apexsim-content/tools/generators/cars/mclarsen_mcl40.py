"""Build mclarsen-mcl40 (F1 kit) in Blender: cars/default/mclarsen-mcl40/mclarsen_mcl40.glb.

    blender -b --factory-startup --python tools/generators/cars/mclarsen_mcl40.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\mclarsen_mcl40.py").read())

The data is this car's; the stages are kits/f1.py's. Replace one by
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

kit = carlib.load("kits/f1")

CAR = dict(folder="mclarsen-mcl40", stem="mclarsen_mcl40", logo="mclarsen_logo.png",
    paint=(0.95, 0.32, 0.015), accent=(0.012, 0.016, 0.035), paint_metallic=0.30,
    number="4", nose_z=0.02, pod_w=1.04, pod_h=1.00, cover_h=1.00, coke=1.04,
    inlet="wide", airbox="oval", tcam=(0.02, 0.02, 0.02), halo="paint",
    # the spoon: a broad flat nose over a low two-element
    # wing; high pod shoulders ramping back; curled endplates
    nose_len=0.0, nose_w=1.35, undercut=0.07, pod_slope=-0.03,
    fw="low", rw="curl", beam=1, fin=False, rw_plan=("arch", 0.025),
    livery=[(-1.3, 2.5, 0.0, 2.9),               # dark lower half
            (0.45, 2.4, 6.6, 9.0),               # dark spine on the engine cover
            (-2.2, -1.2, 6.8, 9.0)],             # and down the nose
    sponsors=dict(pod=0, cover=11, nose=15, fw_ep=3, rw_ep=10, flap=6))

kit.start(CAR)
kit.materials()
kit.body_loft()
kit.apertures()
kit.begin_parts()
kit.floor()
kit.front_wing()
kit.rear_wing()
kit.halo()
kit.cockpit()
kit.mirrors()
kit.intakes()
kit.suspension()
kit.decals()
kit.top_decals()
kit.driver()
kit.export()

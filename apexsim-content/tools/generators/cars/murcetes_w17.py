"""Build murcetes-amd-w17 (F1 kit) in Blender: cars/default/murcetes-amd-w17/murcetes_w17.glb.

    blender -b --factory-startup --python tools/generators/cars/murcetes_w17.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\murcetes_w17.py").read())

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

CAR = dict(folder="murcetes-amd-w17", stem="murcetes_w17", logo="murcetes_f1_logo.png",
    paint=(0.60, 0.61, 0.64), accent=(0.015, 0.015, 0.018), paint_metallic=0.90,
    number="63", nose_z=-0.03, pod_w=0.86, pod_h=0.94, cover_h=0.97, coke=0.92,
    inlet="slot", airbox="tri", tcam=(0.02, 0.02, 0.02), halo="carbon",
    # needle nose; slim pods cut deep underneath; a shark
    # fin to the wing; square endplates, double beam wing
    nose_len=0.08, nose_w=0.78, undercut=0.10, pod_slope=0.02,
    fw="classic", rw="square", beam=2, fin=True, rw_plan=("straight", 0.0),
    livery=[(-1.3, 2.5, 0.0, 3.25),              # black below the flank crease
            (-3.1, -2.3, 0.0, 9.0)],
    sponsors=dict(pod=6, cover=7, nose=15, fw_ep=10, rw_ep=6, flap=2))

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

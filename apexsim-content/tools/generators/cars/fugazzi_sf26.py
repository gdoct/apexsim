"""Build fugazzi-sf26 (F1 kit) in Blender: cars/default/fugazzi-sf26/fugazzi_sf26.glb.

    blender -b --factory-startup --python tools/generators/cars/fugazzi_sf26.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\fugazzi_sf26.py").read())

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

CAR = dict(folder="fugazzi-sf26", stem="fugazzi_sf26", logo="fugazzi_f1_logo.png",
    paint=(0.50, 0.006, 0.010), accent=(0.93, 0.93, 0.92), paint_metallic=0.30,
    number="16", nose_z=0.00, pod_w=1.00, pod_h=1.03, cover_h=1.00, coke=1.00,
    inlet="tall", airbox="oval", tcam=(0.95, 0.80, 0.05), halo="paint",
    # short, wide nose; pods that wash steeply down to the
    # floor; swept wing endplates; no fin
    nose_len=-0.10, nose_w=1.20, undercut=0.05, pod_slope=0.10,
    fw="swept", rw="swept", beam=1, fin=False, rw_plan=("spoon", 0.030),
    livery=[(-0.70, 1.40, 4.10, 4.40),            # white band along the pod shoulder
            (-3.1, -2.45, 0.0, 9.0)],             # white nose tip
    sponsors=dict(pod=8, cover=4, nose=14, fw_ep=2, rw_ep=13, flap=0))

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

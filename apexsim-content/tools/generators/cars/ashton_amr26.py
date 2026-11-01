"""Build ashton-marvin-amr26 (F1 kit) in Blender: cars/default/ashton-marvin-amr26/ashton_amr26.glb.

    blender -b --factory-startup --python tools/generators/cars/ashton_amr26.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\ashton_amr26.py").read())

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

CAR = dict(folder="ashton-marvin-amr26", stem="ashton_amr26", logo="ashton_logo.png",
    paint=(0.005, 0.15, 0.10), accent=(0.62, 0.95, 0.08), paint_metallic=0.65,
    number="14", nose_z=0.01, pod_w=0.95, pod_h=1.06, cover_h=1.03, coke=0.97,
    inlet="high", airbox="tri", tcam=(0.62, 0.95, 0.08), halo="paint",
    # long pointed nose; pods that fall away early; tall swept
    # front endplates, curled rear ones, a fin
    nose_len=0.05, nose_w=0.92, undercut=0.03, pod_slope=0.06,
    fw="swept", rw="curl", beam=2, fin=True, rw_plan=("swept", 0.07),
    livery=[(-0.60, 1.60, 4.55, 4.80),             # lime pinstripe along the pod shoulder
            (-3.1, -2.55, 0.0, 9.0)],              # lime nose tip
    sponsors=dict(pod=4, cover=8, nose=14, fw_ep=6, rw_ep=4, flap=10))

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

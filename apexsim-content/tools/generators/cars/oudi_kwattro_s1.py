"""Build oudi-kwattro-s1 (Rally kit) in Blender: cars/default/oudi-kwattro-s1/oudi_kwattro_s1.glb.

    blender -b --factory-startup --python tools/generators/cars/oudi_kwattro_s1.py
    exec(open(r"E:\\apexsim\\apexsim-content\\tools\\generators\\cars\\oudi_kwattro_s1.py").read())

The data is this car's; the stages are kits/rally.py's. Replace one by
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

kit = carlib.load("kits/rally")

CAR = carlib.load("rally_cars").VARIANTS["oudi"]   # shared with the livery painter

kit.start(CAR)
kit.materials()
kit.body_loft()
kit.arches()
kit.apertures()
kit.begin_parts()
kit.face()
kit.lamp_pod()
kit.splitter()
kit.rear_aero()
kit.tail_lights()
kit.mud_flaps()
kit.pillars()
kit.intake_floors()
kit.fittings()
kit.cockpit()
kit.driver()
kit.export()

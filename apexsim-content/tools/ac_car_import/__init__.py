"""Import an Assetto Corsa car as an ApexSim car (docs/ac-import.md).

The package behind `tools/ac_car_import.py`. It shares the kn5 reader,
the INI parser and the texture decoder with the track importer
(`tools/ac_import/`) and writes nothing outside `cars/custom/`.
"""

TOOL_VERSION = "ac_car_import 1"

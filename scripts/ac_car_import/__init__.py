"""Import an Assetto Corsa car as an ApexSim car (docs/content/ac-import.md).

The package behind `scripts/ac_car_import.py`. It shares the kn5 reader,
the INI parser and the texture decoder with the track importer
(`scripts/ac_import/`) and writes nothing outside `content/cars/custom/`.
"""

TOOL_VERSION = "ac_car_import 1"

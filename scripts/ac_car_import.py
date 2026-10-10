#!/usr/bin/env python3
"""Import an Assetto Corsa car as an ApexSim car (docs/content/ac-import.md).

    python scripts/ac_car_import.py "E:\\SteamLibrary\\steamapps\\common\\assettocorsa\\content\\cars\\ks_porsche_911_gt3_r_2016"
    python scripts/ac_car_import.py <folder> --list          # skins, compounds, parts
    python scripts/ac_car_import.py --all <ac>/content/cars  # a whole collection

Writes content/cars/custom/<Stem>/: car.toml, the body, wheel and steering
wheel GLBs, the skins as texture liveries, and <Stem>.import.json. Then
restart the server, and restart the game or run `apexsim.car.Rescan`.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ac_car_import.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())

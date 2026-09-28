#!/usr/bin/env python3
"""Import an Assetto Corsa track as an ApexSim track (docs/AC_TRACK_IMPORT.md).

    python scripts/ac_import.py "E:\\SteamLibrary\\steamapps\\common\\assettocorsa\\content\\tracks\\ks_zandvoort"
    python scripts/ac_import.py <folder> --list             # the layouts
    python scripts/ac_import.py <folder> --layout layout_gp  # one of them
    python scripts/ac_import.py --all <ac>/content/tracks    # a whole collection

Writes the server's files (YAML, .ats, the four sidecars, the report) into
content/tracks/custom/ and the client's export (manifest, mesh blob,
textures, preview) into build/tracks/. Then restart the server, and restart
the game or run `apexsim.track.Rescan`. Run from the repo root.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ac_import.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())

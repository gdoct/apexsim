// Importing a car or track from Assetto Corsa: the importers are Python
// (numpy, Pillow, PyYAML, msgpack). The launcher looks for a Python 3.11+,
// keeps the packages in a venv of its own under %LOCALAPPDATA% (made on first
// use, redone when importer-requirements.txt changes) and runs the import in
// a console so the player sees the setup and the report.

#pragma once

#include "env.h"
#include "text.h"

// Asks for an AC car or track folder (or a whole content\cars or content\tracks)
// and starts its import in a console window. wantCar picks the folder prompt.
// Returns the import's process handle, for the caller to wait on and close, or
// null when nothing was started (cancelled, no Python, no importer); the player
// has then been told why.
HANDLE StartAcImport(HWND owner, const Env& env, bool wantCar);

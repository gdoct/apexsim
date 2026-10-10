# Custom cars

Put your own cars here: ones you have imported or built yourself.
Everything in this folder except this README is gitignored, and a release
build does not ship it unless you pass `-IncludeCustomCars` to
`build_release.ps1` or `build_game_standalone.ps1`. An import may be derived
from another game's content that you may use but must not redistribute.

To import a car from your own Assetto Corsa install, use the launcher's
Manage content > Import from Assetto Corsa, or run
`python scripts/ac_car_import.py <path to the AC car folder>` from the repo
root, which writes the car's folder here (docs/content/ac-import.md).

A custom car is laid out exactly like one in `../default/`: a folder holding
its `car.toml` and every file that names (the body GLB as `model`, the
driver, the DRS flap, its own road or steering wheels, skins and livery
logos). A class wheel named in `[wheels]` comes from the shared
`content/wheels/` (`Game/Wheels` in an installed game). See
`docs/content/cars.md` and `MODDING.md`.

In an installed game the same folder goes in `Game/Cars/custom/<folder>`,
with a copy of its `car.toml` in `Server/content/cars/custom/<folder>`.

Two rules:

- **The folder name must be unique across `default/` and `custom/`.** The
  game keys a car's meshes by the folder name alone.
- **The `id` must be unique too.** If a custom car reuses a shipped car's
  id, the server and the game keep the shipped one and log a warning.

After adding or changing a car, restart the server and the game (an editor
or Development build can run `apexsim.car.Rescan` instead).

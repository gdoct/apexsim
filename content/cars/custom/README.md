# Custom cars

Put your own cars here: ones you have imported or built yourself.
Everything in this folder except this README is gitignored, and a release
build does not ship it unless you pass `-IncludeCustomCars` to
`build_release.ps1` or `build_game_standalone.ps1`. An import may be derived
from another game's content that you may use but must not redistribute.

A custom car is laid out exactly like one in `../default/`: a folder holding
its `car.toml` and the files that names (the body GLB as `model`, the DRS
flap GLB, the livery logos). The class wheel it names in `[wheels]` comes
from the shared `content/wheels/`. See `docs/CAR_MODELS.md` and `MODDING.md`.

Two rules:

- **The folder name must be unique across `default/` and `custom/`.** The
  game keys a car's meshes and the editor import (`/Game/Cars/<folder>`) by
  the folder name alone.
- **The `id` must be unique too.** If a custom car reuses a shipped car's
  id, the server and the game keep the shipped one and log a warning.

After adding or changing a car, restart the server, and restart the game or
run `apexsim.car.Rescan`.

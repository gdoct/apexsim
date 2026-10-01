# Custom HUD components

Put your own HUD components here. Each one is a folder holding a
`component.json`; the game reads this folder after `../default/`.

To move, resize, add or remove panels you do not need to write anything: use
Settings > Gameplay > HUD layout in the game. It saves your arrangement here
as `layout.json`; delete that file to go back to the shipped layout.

- **Change a shipped panel:** copy its folder from `../default/` to here and
  edit the copy. A folder here with the same name replaces the shipped one.
- **Hide a shipped panel:** make a folder here with its name, holding a
  `component.json` that says `{ "enabled": false }`.
- **Add a panel:** make a folder with a new name.

In a race, open the console and run `apexsim.hud.Reload` to see a change
without restarting, and `apexsim.hud.Data` to list every value a component
can show (speed, lap times, tyres, the standings, ...) with what it holds
right now. A component that fails to load is named on screen with the
reason.

A small one, a big speed read-out above the middle of the bottom edge:

```jsonc
// custom/big_speed/component.json
{
  "name": "Big speed",
  "region": "bottom",
  "margin": [0, 0, 0, 170],
  "root": {
    "type": "panel", "padding": [18, 6], "background": "=alpha('surface', 0.8)",
    "children": [
      { "type": "text", "font": "display", "size": 64,
        "text": "{fmt(car.speed)}",
        "color": "=car.rpm_fraction > 0.95 ? 'error' : 'text'" }
    ]
  }
}
```

The full format, every element and function, and every data point are in
`docs/HUD_MODDING.md` in the source repository.

In the repository, everything in this folder except this README is
gitignored, and the build scripts never ship it.

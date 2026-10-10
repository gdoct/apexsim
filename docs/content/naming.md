# Names on screen

Real circuit, corner, series and car names are mostly registered trademarks
(the operators', sponsors', manufacturers', famous drivers'). The source data
may keep them; the product never shows them. The rule:
**a real name lives in `name`, what the player sees lives beside it.** This
covers everything ApexSim ships, and every screen, export, replay, guide,
marketing page and promo caption made from it.

## Tracks

- A track YAML's `name` is the real one (`Circuit Zandvoort`), source data
  only. `display_name` is a sound-alike that still says which circuit it is
  (`Zandervoort`, `Spa-Frankenchamps`, `Red Pull Ring`).
- `metadata.description` says what it is modelled on by place, never by the
  operator's name ("Modelled on the circuit in the dunes at Zandvoort,
  Netherlands."). The track picker shows it
  (`FApexTrackCatalogRow::Description`).
- The server sends `display_name` as the track's name: `TrackConfig::name` is
  `TrackFileFormat::display_name`, falling back to `name` when it is missing
  or blank (`server/src/track_loader.rs`). The lobby, sessions, replays and
  spectator streams all carry it.
- `ats-export` writes it into the export manifest as `track_display_name`,
  which the runtime catalog row prefers over `track_name`
  (`ApexTrackContentSubsystem`); `scripts/build_track_catalog.py` writes it
  into the `DT_TrackCatalog` fallback rows.
- Both `TrackFile` types (server and track editor) keep `display_name`
  through rewrites.
- Stems stay the place names (`Zandvoort`), and `-ApexTrack=` takes the stem.
- The table of every shipped circuit, real name and shown name, is in
  [content/tracks/default/README.md](../../content/tracks/default/README.md).
  A new track needs a row there and both names in its YAML.
- Tools that write YAMLs take a display name: `convert_track
  --display-name`, `scripts/ac_import.py --display-name`.

## Corners

- Each corner in a `<Stem>.layout.json` dossier has `name` (OSM's, which
  `apexsim-replay find --corner` / `pose --corner` and `drs_zones.py` look
  up) and `display_name`, written by `scripts/osm_layout.py` from
  `CORNER_DISPLAY` (`with_display_names`).
- A `CORNER_DISPLAY` entry maps a real name to a sound-alike (a sponsor or a
  person: `Würth Kurve` -> `Wurst-Kurve`, `S do Senna` -> `S do Sonho`) or to
  `None` for a way that is not a corner (the circuit's own name, a
  connector). **A name not in the table is shown as it is**, which is right
  for a place (Eau Rouge) and wrong for anything else.
- `ats-dress` lays a `corner_sign` only from `display_name`
  (`dress::corner_signs`); a corner with none gets no board. The track guide
  and the hotlap-watch corner list name corners from `display_name` too
  ([track-guide.md](track-guide.md)).
- `python scripts/osm_layout.py --all --names-only` re-applies the table to
  the checked-in dossiers without the OSM cache. After a refetch that finds a
  new corner name, or for a new circuit, review its corners against the table.

## Classes and categories

Car classes and track categories stay `F1` / `WEC` / `DTM` / `IndyCar` in
`car.toml` and the YAML, because logic keys on them (`F1` picks the cockpit
style, DRS, the engine sound). Every screen shows them through
`ApexCatalog::DisplayClass` (`Catalog/ApexCatalogRows.h`): F1 -> Formula,
WEC -> Endurance, DTM / GT3 -> GT3, IndyCar -> Independent, anything else as
it is. Test: `ApexSim.Content.DisplayClass`. The marketing site mirrors it in
`scripts/site/facts.py` (`display_class`).

## Cars, brands and props

- Shipped cars carry parody names and brands in `car.toml` itself (`name`,
  `brand`: `Posh GT3 RS`, `Fugazzi SF-26`, `Murcetes-AMD W17`); there is no
  separate real-name field.
- Hoarding and sponsor brands are invented (`content/props/board/brands/`,
  `scripts/content/props/gen_brands.py`). Landmark props are original
  designs with neutral asset keys, and trademarked mascots are not modelled.
- Promo captions (`scripts/promo/shots.yml`) and the marketing page use the
  display names.

## Player content is exempt

The rule covers what ApexSim ships. An Assetto Corsa import is the player's
own content on their own machine: an imported car keeps AC's real `name` and
`brand`, an imported track its own name and sponsor textures
([ac-import.md](ac-import.md)). Anything rendered for publication (showcases,
site shots, promo clips) is therefore dealt from `content/cars/default` only,
so an import in a real team's colours never reaches it.

## Traps

- Missing `display_name` falls back to the real `name` on screen. A new YAML,
  dossier corner or circuit without a `CORNER_DISPLAY` entry shows the
  trademark silently.
- Never key logic on a display name or show a class key raw: logic uses
  `name`, stems, `track_id` and class keys; screens use the display forms.

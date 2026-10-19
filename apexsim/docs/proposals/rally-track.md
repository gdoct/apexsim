# Feature: Fantasy rally stages

## Goal

Add three fictional point-to-point rally stages that reward car control on
changing surfaces and feel distinct through both terrain and roadside scenery.
Each stage alternates enclosed natural sections with occasional public-road
or village sections. Transitions should be readable at driving speed and must
not make scenery look like a drivable route when it is not.

Stages are run individually: one car occupies the stage at a time. Players
race the clock and leaderboard, and may watch another driver's run. This is a
stage format, not a requirement to add rally-specific collision or vehicle
physics to the client; the server remains authoritative for simulation.

## Stage requirements

- Build one dirt, one gravel, and one snow stage. Give each a distinct route
	profile, setting, and visual identity; do not make three reskins of the same
	layout.
- Each route must include a mix of faster sections, linked corners, tight
	corners, elevation changes, and braking zones. Avoid placing a hazard just
	past a blind crest or immediately beyond a visually misleading road edge
	unless the approach gives the driver a fair warning.
- Use the existing track format and pipeline. Author road grip and off-road
	ground through track surfaces and sidecars, and place scenery through `.ats`
	props. Reuse the common pipeline with per-stage data/configuration; only add
	surface-specific processing where the current formats cannot represent a
	required behavior.
- Treat puddles, mud, ruts, loose gravel, ice, and snow as road/ground
	appearance or simulation behavior, not as decorative prop substitutes.
	Decorative rocks, logs, banks, and similar objects must not silently change
	server collision or track-limit behavior.
- Keep props clear of the usable road and sightlines. Use barriers, fences,
	or terrain to communicate dangerous edges. A scenic shortcut must not be
	mistaken for the route.
- Set stage names and descriptions as fictional display content, following
	the track naming rules. Keep route length, event format, target lap time, and
	specific surface physics as design decisions to settle when the layouts are
	planned; this proposal does not prescribe unsupported values.

## Stage identities

### Dirt

Wind through open farmland and narrow dirt lanes, with occasional muddy or
wet sections. Use field boundaries, drainage, farm access, and a small number
of village/public-road transitions to create changes in width and visibility.
Roadside houses and spectators belong only in appropriate settled sections.

### Gravel

Wind through forest and rough upland terrain. Vary the road between compacted
gravel and loose edges, with ruts, shallow drainage, and scattered rocks used
as visual cues. Include a few open clearings so the forest route has changes
in exposure and sight distance.

### Snow

Wind through mountainous forest and open snowy terrain. Distinguish packed
road, icy patches, and deep snow at the road edge through authored surface
and terrain treatment. Use snowbanks to define the route, with exposed
sections, tree-lined sections, and occasional settlements or service areas.

## Required props

Use the existing `apexsim-content/props` kit wherever it fits. The list below is a
content requirement, not a claim that every asset already exists. Record any
missing asset as new kit work, with its kind, footprint, pivot, material
slots, and intended placement before building it. See
[`apexsim-content/docs/props.md`](../../../apexsim-content/docs/props.md) for authoring rules and the
current catalogue. We have Blender integration to create these ourselves.

### Required on every stage

- **Route and distance cues:** rally direction/chevron boards, distance
	markers, stage start and finish markers, and marshal posts or flags at
	controlled/spectator areas. Use existing signs and boards where possible;
	add a rally-specific sign asset only if the current kit cannot express it.
- **Road-edge safety:** modular guardrails or low walls on exposed road
	edges, mesh or catch fencing where spectators are close, and suitable
	barrier ends/transitions. Do not use a barrier continuously where the
	landscape should remain open.
- **Vegetation:** trees, bushes, and ground-cover/scatter appropriate to the
	region, with enough variation to avoid repeated, visibly identical runs.
- **Natural set dressing:** rocks and occasional fallen timber placed outside
	the road and clear recovery area. Keep these visual unless server-side
	collision is explicitly designed and tested.
- **Human presence:** small roadside spectator groups, simple viewing
	enclosures, and marshal positions at selected safe viewing points. Avoid
	crowd props directly on the road edge without a barrier or terrain buffer.
- **Settlement and service dressing:** a restrained set of houses or farm
	buildings, fences, utility poles, lamps, and service tents/vehicles where
	the route passes a village, farm, or stage facility. Do not add urban props
	to every stage just to fill space.

### Dirt-stage props

- Field-edge fences, gates, hedges, crop/field dressing, and drainage
	culverts or ditches.
- Farm buildings, sheds, barns, and occasional parked farm/service vehicles.
- Muddy verges, puddles, and water-filled ruts as terrain or surface
	treatment; these are not standalone prop requirements.
- Low walls and crowd-control fencing for any village-road spectator section.

### Gravel-stage props

- Dense conifer and mixed-forest tree sets, bushes, scrub, and forest-floor
	scatter.
- Rock clusters, exposed cut-bank/retaining-wall dressing, and drainage
	ditches or culverts.
- Occasional forestry access gates, timber stacks, and service vehicles,
	kept outside the driving line.
- Gravel shoulders and ruts as terrain or surface treatment, not meshes
	scattered across the road.

### Snow-stage props

- Snowbanks and snow-covered edge barriers that clearly separate road from
	deep snow. The snow treatment should work with the existing snow material
	system where possible; a dedicated bank mesh/material is new kit work if
	the existing terrain cannot represent the shape.
- Snow-covered conifers and bare winter trees, plus open-field boundary
	fences and mountain cut-bank/retaining-wall dressing.
- Ice and packed/deep snow as surface or terrain treatment, not loose prop
	meshes on the driving surface.
- Sparse village/service dressing suitable for winter, including covered
	signs or lights only where visibility and authored assets support it.

## Acceptance criteria

- All three stages load through the existing track pipeline and can be driven
	from start to finish without scenery blocking the road or confusing the
	route.
- Stage runs support one active car, timed results/leaderboards, and viewing
	another run using existing session and spectator capabilities where
	available. Any missing event/session capability is identified separately
	rather than hidden in track content work.
- Surface changes are represented by the track data consumed by the server
	and client; decorative dressing alone does not claim to change grip.
- Required prop families are represented on each stage or explicitly marked
	as unavailable/new kit work. Props have sensible scale, placement, and
	visibility and do not intrude into the driving line.
- Re-running the applicable dress, groom, and export steps produces valid
	track outputs without requiring a separate one-off pipeline per surface.
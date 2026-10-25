//! Dress a `.ats` scene from the circuit's real-world layout dossier.
//!
//! The shipped scenes were dressed by a procedural enrichment pass that
//! knew nothing about the actual venue: grandstands landed wherever there
//! was room, the pit lane took whichever side of the road the old data
//! guessed, and no circuit had its landmarks. This pass replaces all of
//! that with what is really there, as recorded in
//! `tracks/default/<Stem>.layout.json` (see [`crate::layout`]).
//!
//! What it owns — and therefore deletes and re-lays from scratch on every
//! run — is every prop of the kinds it places: grandstands, buildings,
//! attractions, bridges over the road, lights, vehicles and sky props,
//! plus the pit lane when the dossier has one. Barriers, tire walls,
//! distance boards and trees stay [`crate::groom`]'s, so the two passes
//! never fight: dress first (it decides where the stands and the pit lane
//! are), groom second (it fills in around them).
//!
//! Dressing is a pure function of the dossier and the track — no RNG, no
//! wall clock, no map-order dependence — so it is idempotent and a re-run
//! writes byte-identical output.

use crate::ats::{AtsScene, Decal, PitLane, Prop, PropKind, Side, Surface, SurfaceKind};
use crate::dem::DemFile;
use crate::layout::{Crossing, Landmark, Layout, Stand, Structure};
use crate::props;
use crate::strip_layout::surface_height;
use crate::terrain::TerrainHeightfield;
use crate::track_data::TrackFile;
use crate::track_path::{curvature_at, offset_point, CenterlinePath, PathSample};

/// Bay pitch of the stand families, matching `ApexProps::BayPitchM`. A
/// stand is laid as runs of at most [`STAND_RUN_M`] so one built round a
/// bend follows its real outline instead of cutting the corner.
const BAY_PITCH_M: f32 = 10.0;
const STAND_RUN_M: f32 = 40.0;
/// A stand shorter than this is a commentary box or a photo platform, not
/// a grandstand.
const MIN_STAND_M: f32 = 12.0;
/// Beyond this depth a stand is laid from the tall `_large` family.
const LARGE_STAND_DEPTH_M: f32 = 13.0;
/// A stand this deep is roofed even when OSM does not say so: nothing that
/// deep is built open.
const ASSUMED_ROOF_DEPTH_M: f32 = 25.0;

/// Buildings closer than this to the pit lane are pit garages, which the
/// Unreal bake generates from the lane itself.
const PIT_BUILDING_RANGE_M: f32 = 30.0;
/// How far from the pit lane a real building is still the paddock, and is
/// laid as a media centre or hospitality block.
const PADDOCK_RANGE_M: f32 = 100.0;
/// A one-storey building at least this big is a hall, not an office block.
const HALL_AREA_M2: f32 = 1500.0;
/// Footprint below which a building is a shed, not scenery.
const MIN_BUILDING_AREA_M2: f32 = 250.0;
/// A building's front stands at least this far past the road edge. The
/// dossier's footprint is the bounding box of the real outline, and the
/// box round an L-shaped or diagonal building can reach onto a road the
/// building itself keeps clear of.
const BUILDING_ROAD_CLEAR_M: f32 = 3.0;
/// Probes per side of a building unit's footprint grid (9 x 9 points, about
/// 5 m apart on the largest block: no road fits between them).
const FOOTPRINT_PROBES: usize = 8;
/// How far a building row is stepped back from its mapped spot looking
/// for room for its whole footprint before it is left out.
const BUILDING_MAX_PUSH_M: f32 = 25.0;
/// How far a building row whose every block stands on a paved run-off is
/// stepped back off it before it is left out. A corner apron reaches
/// further than the asphalt clearance does (Spa's at Eau Rouge).
const BUILDING_RUNOFF_MAX_STEP_M: f32 = 40.0;
/// Least room between the road edge and any part of a stand bay.
const STAND_ROAD_CLEAR_M: f32 = 3.0;
/// How far past its own span a stand's bays may find their nearest road
/// before the check calls them laid against another leg.
const STAND_LEG_SLACK_M: f32 = 150.0;
/// Room a stand keeps past a paved run-off: the server stops cars on
/// stands, and one on the apron is a wall where a car runs wide.
const STAND_RUNOFF_CLEAR_M: f32 = 2.0;
/// Least room between the road edge and a general-admission bank, which
/// stands behind the barrier line, not at the edge of the verge.
const BANK_ROAD_CLEAR_M: f32 = 10.0;
/// An old banked oval's modules, and how far they keep off the circuit's
/// road (their 12.7 m width centred on the mapped line).
const BANKING_MODULE_M: f32 = 10.0;
const BANKING_ROAD_CLEAR_M: f32 = 15.0;
/// A general-admission area's crowd: a knot of spectators about every
/// `GA_PITCH_M` along the front, rows `GA_ROW_M` apart, no deeper than
/// `GA_MAX_DEPTH_M`.
const GA_PITCH_M: f32 = 9.0;
const GA_ROW_M: f32 = 4.5;
const GA_MAX_DEPTH_M: f32 = 14.0;
/// A building with a footprint at least this square and this many levels
/// is a tower.
const TOWER_LEVELS: u32 = 4;

/// Pit boxes are one garage module apart (`ApexTrackAssetBuilder`'s 6 m
/// pitch); the count is what the dossier's lane has room for.
const PIT_BOX_PITCH_M: f32 = 6.0;
const PIT_BOXES: std::ops::RangeInclusive<u32> = 8..=40;
const PIT_WIDTH_M: f32 = 12.0;
const PIT_SPEED_LIMIT_KMH: f32 = 80.0;
/// Boxes a lane gets when it has room: every car of a full grid (the
/// create screen's 20) its own garage, and spares.
const PIT_GRID_BOXES: u32 = 24;
/// A mapped lane shorter than this along the course is not laid, m.
const PIT_MIN_LANE_M: f32 = 150.0;
/// Node spacing of the laid lane, m.
const PIT_LANE_STEP_M: f32 = 4.0;
/// The laid lane bends no tighter than this, m, and the passes that round
/// it off are at most this many.
const PIT_LANE_MIN_RADIUS_M: f32 = 30.0;
const PIT_LANE_ROUND_PASSES: usize = 400;
/// How far along the lane its mapped offset is smoothed, m (Gaussian
/// sigma): a GPS trace's wander, not the lane's shape.
const PIT_LANE_SMOOTH_M: f32 = 12.0;
/// Steeper than this, rise over run, a stretch of lane off the road is
/// eased into a ramp ([`ease_steep_grades`]), in at most this many passes.
const PIT_LANE_MAX_GRADE: f32 = 0.08;
const PIT_LANE_GRADE_PASSES: usize = 4000;
/// Least room between the road edge and the lane's edge through the
/// lane's middle: enough to stand the pit wall in
/// (`ue_export`'s `PIT_TAPER_WALL_MIN_M`).
const PIT_APRON_M: f32 = 3.0;
/// The entry and exit tapers: a fifth of the lane, within these, m.
const PIT_TAPER_MIN_M: f32 = 40.0;
const PIT_TAPER_MAX_M: f32 = 90.0;
/// At its ends the lane's middle sits this far inside the road edge, so
/// the two surfaces overlap rather than meet at a seam, m.
const PIT_MERGE_OVERLAP_M: f32 = 1.5;

#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct DressReport {
    /// Props of the dressed kinds removed before re-laying.
    pub removed: usize,
    pub stands: usize,
    pub stand_props: usize,
    pub buildings: usize,
    pub bridges: usize,
    pub landmarks: usize,
    /// Car parks, camps, the village, marshal posts, corner boards and
    /// any floodlights this pass had to add.
    pub surroundings: usize,
    pub pit_lane: bool,
    /// Tarmac run-off bands given the circuit's painted stripes.
    pub painted_runoff: usize,
    /// Kerb markers at the curbs' ends and sausage kerbs behind the inside
    /// curbs of tight bends (`surroundings::lay_kerb_pieces`).
    pub kerb_pieces: usize,
    /// Road graffiti laid from the dossier as `.ats` decals.
    pub graffiti: usize,
    /// Dossier entries that could not be placed, with the reason.
    pub skipped: Vec<String>,
    /// Entries laid where they look wrong, for a person to read (R17).
    pub checks: Vec<String>,
}

impl DressReport {
    pub fn placed(&self) -> usize {
        self.stand_props
            + self.buildings
            + self.bridges
            + self.landmarks
            + self.surroundings
            + self.kerb_pieces
    }
}

/// True for the prop kinds this pass owns. Grooming consults it too: in a
/// dressed scene these props stand where the circuit's own survey says,
/// so they are re-seated but never pushed or squared up.
pub fn dressed_kind(kind: PropKind) -> bool {
    matches!(
        kind,
        PropKind::Grandstand
            | PropKind::Building
            | PropKind::Attraction
            | PropKind::Bridge
            | PropKind::Light
            | PropKind::Sky
            | PropKind::Vehicle
    )
}

/// The `misc` and `sign` assets [`lay_landmark`] can produce. Neither kind
/// is owned by this pass — bollards, kerb markers and flag poles are placed
/// by hand — but a landmark laid from the dossier is, or every re-dress
/// would stack another statue on the same spot and grooming would treat
/// the surveyed position as a guess it may push.
const LANDMARK_ASSETS: [(PropKind, &str); 4] = [
    (PropKind::Misc, "bull_statue"),
    (PropKind::Misc, "cauldron"),
    (PropKind::Misc, "toll_gate_lane"),
    (PropKind::Sign, "hillside_letters"),
];

fn is_landmark_asset(prop: &Prop) -> bool {
    LANDMARK_ASSETS
        .iter()
        .any(|(kind, asset)| prop.kind == *kind && prop.asset == *asset)
}

/// True for a prop this pass owns: every prop of a [`dressed_kind`], plus
/// the `misc` landmarks. Use this, not `dressed_kind`, when there is a
/// prop in hand.
/// Props that belong to the dressing pass whatever the circumstances:
/// everything it lays from the surroundings layers, and the landmarks.
///
/// [`dressed_prop`] is the wider test, and part of it — every
/// `grandstand` or `building` — is only true of a scene that was actually
/// dressed, because an undressed scene's buildings were placed by hand
/// and grooming may legitimately push them. These assets are different:
/// nothing but the dressing pass ever lays a `village_house_a` or a
/// `forest_impostor`, so they are the pass's own even when a scene is
/// groomed without its dossier. Treating them as ordinary furniture there
/// pushed two of them back and forth across a folded section at Austin on
/// every groom, which `grooming_every_real_track_twice_changes_nothing`
/// caught.
pub fn always_dress_owned(prop: &Prop) -> bool {
    surroundings::owns(prop) || is_landmark_asset(prop)
}

pub fn dressed_prop(prop: &Prop) -> bool {
    dressed_kind(prop.kind) || is_landmark_asset(prop) || surroundings::owns(prop)
}

/// Apply `layout` to `scene`. Returns `None` for a degenerate centerline.
pub fn dress_scene(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: &Layout,
) -> Option<DressReport> {
    dress_scene_with_dem(track, scene, layout, None)
}

/// [`dress_scene`] with the track's elevation model, which is what the
/// far-field woodland is planted from. Without one the slopes stay bare,
/// exactly as before.
pub fn dress_scene_with_dem(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: &Layout,
    dem: Option<&DemFile>,
) -> Option<DressReport> {
    let path = CenterlinePath::from_track(track)?;
    // Water and bridge spans first: the ground the props are seated on
    // sinks under the water, so it has to know about it.
    lay_water_and_bridges(scene, layout);
    // Seated on the ground the exporter will draw: see
    // `groom::seating_terrain`, which is shared so the two cannot drift.
    // The roads under the lap's own bridges are laid on that ground
    // first, then join it.
    let mut ids: Vec<u64> = scene.under_roads.drain(..).map(|r| r.id).collect();
    let terrain = crate::groom::seating_terrain(&path, scene, dem)?;
    ids.sort_unstable();
    ids.reverse();
    for (name, nodes, width_m) in lay_under_roads(&path, &terrain, layout) {
        let id = ids.pop().unwrap_or_else(|| scene.alloc_id());
        scene.under_roads.push(crate::ats::UnderRoad {
            id,
            name,
            nodes,
            width_m,
        });
    }
    let terrain = terrain.with_under_roads(&crate::groom::under_road_paths(scene));
    let before = scene.props.len();
    // The ids of the props this pass replaces come back to it, lowest
    // first, so re-dressing an unchanged circuit rewrites the same file
    // rather than walking `next_id` up on every run.
    let mut recycled: Vec<u64> = scene
        .props
        .iter()
        .filter(|p| dressed_prop(p))
        .map(|p| p.id)
        .collect();
    recycled.sort_unstable();
    recycled.reverse();
    scene.props.retain(|p| !dressed_prop(p));
    let mut report = DressReport {
        removed: before - scene.props.len(),
        ..Default::default()
    };

    if !crate::circuit_style::CircuitStyle::for_scene(scene).pit_lane {
        scene.pit_lane = None;
    } else if let Some(pit) = build_pit_lane(&path, layout) {
        scene.pit_lane = Some(pit);
        report.pit_lane = true;
    }
    report.painted_runoff = paint_runoff(&path, scene);
    report.graffiti = lay_graffiti(&path, scene, layout, &mut report.skipped);
    let lane = scene
        .pit_lane
        .as_ref()
        .and_then(|pit| CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0));
    // Nothing dressed stands in the pit lane or where its garages go: the
    // stands step back out of it, everything else laid in it is dropped
    // below.
    let zone = scene
        .pit_lane
        .as_ref()
        .zip(layout.pit_lane.as_ref())
        .and_then(|(pit, road)| {
            let row = crate::ue_export::garage_row_span(&path, pit, Some(&terrain));
            PitZone::new(pit, road.side, row)
        });

    let mut laid: Vec<Prop> = Vec::new();
    for stand in &layout.stands {
        let props = lay_stand(&path, &terrain, zone.as_ref(), &scene.surfaces, stand);
        if props.is_empty() {
            let why = if stand.length_m < MIN_STAND_M {
                "too small to be seating"
            } else {
                "over the garage row (the bake builds that terrace) or no room clear of the pit lane"
            };
            report
                .skipped
                .push(format!("stand {}: {why}", name_of(&stand.name)));
            continue;
        }
        if let Some(note) = stand_off_its_leg(&path, stand, &props) {
            report.checks.push(note);
        }
        report.stands += 1;
        report.stand_props += props.len();
        laid.extend(stamped(props, format!("stand {}", name_of(&stand.name))));
    }
    let city = crate::circuit_style::CircuitStyle::for_scene(scene).city_buildings;
    for structure in &layout.structures {
        match lay_structure(
            &path,
            &terrain,
            lane.as_ref(),
            &scene.surfaces,
            structure,
            city,
        ) {
            Ok(props) => {
                report.buildings += props.len();
                laid.extend(stamped(
                    props,
                    format!("building {}", name_of(&structure.name)),
                ));
            }
            Err(reason) => report
                .skipped
                .push(format!("building {}: {reason}", name_of(&structure.name))),
        }
    }
    for crossing in &layout.crossings {
        for mut prop in lay_crossing(&path, crossing) {
            report.bridges += 1;
            prop.source = Some(format!("bridge {}", name_of(&crossing.name)));
            laid.push(prop);
        }
    }
    if crate::circuit_style::CircuitStyle::for_scene(scene).city_buildings {
        laid.extend(stamped(
            lay_street_lamps(&path, &terrain, &scene.bridges),
            "street lamps".to_string(),
        ));
    }
    for landmark in &layout.landmarks {
        match lay_landmark(&path, &terrain, landmark) {
            Ok(Some(mut prop)) => {
                report.landmarks += 1;
                let stamp = if landmark.pit_roof {
                    PIT_ROOF_STAMP
                } else {
                    "landmark"
                };
                prop.source = Some(format!("{stamp} {}", name_of(&landmark.name)));
                laid.push(prop);
            }
            Ok(None) => {}
            Err(reason) => report
                .skipped
                .push(format!("landmark {}: {reason}", name_of(&landmark.name))),
        }
    }

    for (i, line) in layout.banking.iter().enumerate() {
        let props = lay_banking(&path, &terrain, line);
        report.landmarks += usize::from(!props.is_empty());
        laid.extend(stamped(
            props,
            format!(
                "banking {}",
                line.name.clone().unwrap_or_else(|| i.to_string())
            ),
        ));
    }

    // Anything the circuit has of its own is respected: a venue with its
    // own lighting masts in the dossier does not get a generated set.
    let has_lighting = layout.landmarks.iter().any(|l| l.kind == "floodlight");
    let bare = track
        .metadata
        .as_ref()
        .and_then(|m| m.environment_type.as_deref())
        .is_some_and(|env| {
            let env = env.trim();
            env.eq_ignore_ascii_case("desert") || env.eq_ignore_ascii_case("dune")
        });
    let style = crate::circuit_style::CircuitStyle::for_scene(scene);
    let extras = surroundings::lay(
        &path,
        &terrain,
        layout,
        dem,
        !has_lighting && style.floodlights,
        bare,
        style.fan_dressing,
        !style.city_buildings,
    );
    report.surroundings = extras.len();
    // Stamped per sub-layer and area by `surroundings::lay`.
    laid.extend(extras);
    // The kit's pieces on the track's own edge: a marker post at each
    // end of a curb, and sausage kerbs behind the inside curb of a tight
    // bend (a `kerb` wall in the sim: a jolt, never a stop).
    let kerb_pieces = surroundings::lay_kerb_pieces(&path, &terrain, &scene.curbs);
    report.kerb_pieces = kerb_pieces.len();
    laid.extend(stamped(kerb_pieces, "kerb pieces".to_string()));

    // Nothing dressed stands in the water. A bridge, a sky prop and a
    // landmark that is meant to stand over it (the double helix) keep
    // their place.
    if terrain.has_water() {
        drop_laid(&mut laid, &mut report.skipped, "in the water", |p| {
            crate::groom::stands_in_water(&terrain, p)
        });
    }
    // Nor on a road passing under the lap.
    drop_laid(
        &mut laid,
        &mut report.skipped,
        "on a road under the lap",
        |p| crate::groom::stands_on_under_road(&terrain, p),
    );

    // On a city circuit a tree, lamp, sign or planter is not inside a block
    // that was laid after the survey put it there (the buildings are pushed
    // clear of the road; a lamp row is not).
    let city_blocks: Vec<crate::groom::Slab> = if city {
        laid.iter()
            .filter(|p| matches!(p.kind, PropKind::Building | PropKind::Grandstand))
            .map(|p| crate::groom::Slab::of(&path, p))
            .collect()
    } else {
        Vec::new()
    };
    let in_a_block = |p: &Prop| {
        matches!(
            p.kind,
            PropKind::Tree | PropKind::Light | PropKind::Sign | PropKind::Misc | PropKind::Board
        ) && city_blocks.iter().any(|s| s.gap(p.x, p.y) < 1.0)
    };
    if city {
        drop_laid(&mut laid, &mut report.skipped, "inside a building", |p| {
            in_a_block(p)
        });
    }

    if let Some(zone) = &zone {
        // A screen over the garage row stands on the pit building's roof,
        // as the real ones do (R20), rather than being dropped with what
        // stands in the garages' way.
        // The generated garage row is shorter than most real pit
        // buildings, so the roof's screens are spread evenly along it in
        // their order rather than kept at stations past its ends.
        let roof = pit_roof_height(scene);
        let mut on_garages: Vec<(f32, usize)> = laid
            .iter()
            .enumerate()
            .filter(|(_, p)| on_the_pit_roof(p))
            .map(|(i, p)| (zone.lane_station(p.x, p.y), i))
            .collect();
        on_garages.sort_by(|a, b| a.0.total_cmp(&b.0));
        let spots = zone.roof_spots(on_garages.len());
        for (&(_, i), &(x, y)) in on_garages.iter().zip(&spots) {
            let (sample, lat) = nearest_cross_section(&path, x, y);
            let p = &mut laid[i];
            (p.x, p.y) = (x, y);
            p.z = seat_z(&terrain, &sample, lat, x, y) + roof;
        }
        let on_roof = |p: &Prop| on_the_pit_roof(p) && zone.on_the_garage_roof(p.x, p.y);
        drop_laid(
            &mut laid,
            &mut report.skipped,
            "in the pit lane or where its garages stand",
            |p| {
                !matches!(p.kind, PropKind::Bridge | PropKind::Sky)
                    && !on_roof(p)
                    && zone.blocks(&path, p)
            },
        );
    }

    // The authored furniture of a circuit's own straights. It is laid
    // after the pit-zone test on purpose: its lamps and marquees stand
    // behind the garages, which that test keeps clear.
    for mut prop in lay_furniture(&path, &terrain, &layout.furniture) {
        if in_a_block(&prop) {
            continue;
        }
        report.landmarks += 1;
        prop.source = Some(format!("furniture {}", prop.asset));
        laid.push(prop);
    }

    for mut prop in laid {
        prop.id = recycled.pop().unwrap_or_else(|| {
            let id = scene.next_id;
            scene.next_id += 1;
            id
        });
        scene.props.push(prop);
    }
    Some(report)
}

fn name_of(name: &Option<String>) -> String {
    name.clone().unwrap_or_else(|| "(unnamed)".to_string())
}

/// The props with `source` stamped on each (`Prop::source`).
fn stamped(mut props: Vec<Prop>, source: String) -> Vec<Prop> {
    for prop in &mut props {
        prop.source = Some(source.clone());
    }
    props
}

/// Drop the props `gone` picks out of `laid`, noting per source how many
/// went and `why` (`DressReport::skipped`), so `--verbose` and
/// `--explain` say which dossier entry lost them.
fn drop_laid(
    laid: &mut Vec<Prop>,
    skipped: &mut Vec<String>,
    why: &str,
    gone: impl Fn(&Prop) -> bool,
) {
    let mut lost: std::collections::BTreeMap<String, usize> = std::collections::BTreeMap::new();
    laid.retain(|p| {
        if gone(p) {
            *lost
                .entry(
                    p.source
                        .clone()
                        .unwrap_or_else(|| "(unstamped)".to_string()),
                )
                .or_default() += 1;
            false
        } else {
            true
        }
    });
    for (source, n) in lost {
        skipped.push(format!("{source}: {n} prop(s) {why}"));
    }
}

// ---- Road graffiti -----------------------------------------------------------

/// The decal image set this pass owns: every `graffiti/*` decal in a
/// dressed scene came from the dossier and is replaced on each run.
const GRAFFITI_SETS: [&str; 2] = ["graffiti/", "marina/"];

fn is_dressed_decal(image: &str) -> bool {
    GRAFFITI_SETS.iter().any(|set| image.starts_with(set))
}

/// Replace the scene's graffiti decals with the dossier's, reusing their
/// ids lowest first (so a re-run writes the same file). Entries that do
/// not fit on the road are reported and left out. Returns how many were
/// laid.
fn lay_graffiti(
    path: &CenterlinePath,
    scene: &mut AtsScene,
    layout: &Layout,
    skipped: &mut Vec<String>,
) -> usize {
    let mut recycled: Vec<u64> = scene
        .decals
        .iter()
        .filter(|d| is_dressed_decal(&d.image))
        .map(|d| d.id)
        .collect();
    recycled.sort_unstable();
    recycled.reverse();
    scene.decals.retain(|d| !is_dressed_decal(&d.image));

    let total = path.total_length_m();
    let mut laid = 0;
    for g in &layout.graffiti {
        let label = g.name.as_deref().unwrap_or(&g.image);
        let mut decal = Decal {
            id: 0,
            image: g.image.clone(),
            start_m: g.station_m.rem_euclid(total),
            length_m: g.length_m,
            lat_m: g.lat_m,
            width_m: g.width_m,
            reversed: g.reversed,
        };
        if !is_dressed_decal(&g.image) || decal.image_parts().is_none() {
            skipped.push(format!(
                "graffiti {label}: image {:?} is not graffiti/<name>",
                g.image
            ));
            continue;
        }
        if !(g.length_m > 0.0 && g.width_m > 0.0 && g.length_m < total) {
            skipped.push(format!("graffiti {label}: no size"));
            continue;
        }
        // Paint stays on the tarmac: the picture is trimmed to the road
        // at its narrowest over the span, and dropped if nothing is left.
        let steps = (g.length_m / 2.0).ceil().max(1.0) as usize;
        let (mut left, mut right) = (f32::INFINITY, f32::INFINITY);
        for i in 0..=steps {
            let s = path.sample_at(decal.start_m + g.length_m * i as f32 / steps as f32);
            left = left.min(s.width_left_m);
            right = right.min(s.width_right_m);
        }
        let hi = (g.lat_m + g.width_m / 2.0).min(left - 0.2);
        let lo = (g.lat_m - g.width_m / 2.0).max(-(right - 0.2));
        if hi - lo < 1.0 {
            skipped.push(format!("graffiti {label}: off the road"));
            continue;
        }
        decal.lat_m = (hi + lo) / 2.0;
        decal.width_m = hi - lo;
        decal.id = recycled.pop().unwrap_or_else(|| {
            let id = scene.next_id;
            scene.next_id += 1;
            id
        });
        scene.decals.push(decal);
        laid += 1;
    }
    laid
}

// ---- Painted run-off ------------------------------------------------------

/// The circuits whose tarmac run-off is painted in stripes, and the style
/// (`ue_export::runoff_paint_colours`): Spa's red and yellow through Eau
/// Rouge and Raidillon and on round the outside of its corners, Yas
/// Marina's blue and white, Bahrain's blue and red. Keyed by the scene's
/// source file stem.
const RUNOFF_PAINT: [(&str, &str); 3] = [
    ("SpaFrankenchamps", "red_yellow"),
    ("YasMarinara", "blue_white"),
    ("Bahrainless", "blue_red"),
];

/// Tightest radius over a band's span under which it is a corner's run-off
/// and gets the paint; a straight's stays bare.
const PAINTED_RUNOFF_RADIUS_M: f32 = 180.0;

/// Give every corner's tarmac run-off the circuit's stripes, and take them
/// off a straight's. Returns how many bands carry paint.
fn paint_runoff(path: &CenterlinePath, scene: &mut AtsScene) -> usize {
    let stem = scene
        .source_track
        .rsplit_once('.')
        .map_or(scene.source_track.as_str(), |(stem, _)| stem);
    let style = RUNOFF_PAINT
        .iter()
        .find(|(track, _)| *track == stem)
        .map(|(_, style)| *style);
    let total = path.total_length_m();
    let mut painted = 0;
    for surface in &mut scene.surfaces {
        let tarmac = matches!(
            surface.kind,
            crate::ats::SurfaceKind::AsphaltRunoff | crate::ats::SurfaceKind::Concrete
        );
        let mut paint = None;
        if let (true, Some(style)) = (tarmac, style) {
            let span = if surface.end_m >= surface.start_m {
                surface.end_m - surface.start_m
            } else {
                surface.end_m + total - surface.start_m
            };
            let steps = (span / 10.0).ceil().max(1.0) as usize;
            let tightest = (0..=steps)
                .map(|i| curvature_at(path, surface.start_m + span * i as f32 / steps as f32).abs())
                .fold(0.0f32, f32::max);
            if tightest > 1.0 / PAINTED_RUNOFF_RADIUS_M {
                paint = Some(style.to_string());
            }
        }
        if paint.is_some() {
            painted += 1;
        }
        surface.paint = paint;
    }
    painted
}

// ---- Pit lane -------------------------------------------------------------

/// The real pit lane, laid as a road a car can drive: the dossier's
/// polyline, smoothed, then moved where it must be — held at least a pit
/// wall's apron clear of its own leg of the course, kept from folding on
/// the inside of a bend, and blended onto the road edge at both ends so it
/// always leaves and rejoins the track — and rounded wherever it still
/// bends tighter than a car can take at the limit. Each correction is a
/// displacement across the course from the mapped point, so where the
/// lane needs none it stays exactly where it was mapped. Every node takes
/// the height of the road edge beside it. Marked [`PitLane::authored`] so
/// grooming leaves it alone instead of replacing it with a generated
/// ribbon.
///
/// The polyline used to be taken as mapped. A traced lane wanders: at
/// Austin its box row bent 57° in 13 m, at Zandvoort it kinked into its
/// own garages, Shanghai's started 62 m off the road in a field and
/// Melbourne's 8 m short of it, and on seven circuits it ran too close to
/// the road for more than a handful of garages.
/// An old banked oval's line laid as `building/banked_oval_10m` modules,
/// one every 10 m along it, each turned so its high side faces the
/// outside of the curve, the ones within [`BANKING_ROAD_CLEAR_M`] of the
/// circuit's road left out (where the oval meets the lap, or the span
/// over it stands).
fn lay_banking(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    line: &crate::layout::Line,
) -> Vec<Prop> {
    let pts = &line.line;
    // Which way the line turns overall: left turns put the centre on the
    // left, so the outside, the module's +Y, is on the right.
    let turn: f32 = pts
        .windows(3)
        .map(|w| {
            let (a, b) = (
                (w[1][0] - w[0][0], w[1][1] - w[0][1]),
                (w[2][0] - w[1][0], w[2][1] - w[1][1]),
            );
            a.0 * b.1 - a.1 * b.0
        })
        .sum();
    let flip = if turn > 0.0 {
        std::f32::consts::PI
    } else {
        0.0
    };
    let mut out = Vec::new();
    let mut carry = BANKING_MODULE_M / 2.0;
    for seg in pts.windows(2) {
        let (a, b) = (seg[0], seg[1]);
        let len = (b[0] - a[0]).hypot(b[1] - a[1]);
        let heading = (b[1] - a[1]).atan2(b[0] - a[0]);
        let mut d = carry;
        while d < len {
            let (x, y) = (
                a[0] + (b[0] - a[0]) * d / len,
                a[1] + (b[1] - a[1]) * d / len,
            );
            d += BANKING_MODULE_M;
            let (sample, lat) = nearest_cross_section(path, x, y);
            let side = if lat >= 0.0 { Side::Left } else { Side::Right };
            if lat.abs() - side_half_width(&sample, side) < BANKING_ROAD_CLEAR_M {
                continue;
            }
            out.push(Prop {
                id: 0,
                kind: PropKind::Building,
                asset: "banked_oval_10m".to_string(),
                x,
                y,
                z: seat_z(terrain, &sample, lat, x, y),
                yaw_rad: heading + flip,
                scale: 1.0,
                text: None,
                length_m: None,
                source: None,
            });
        }
        carry = d - len;
    }
    out
}

/// The source stamp of a landmark the dossier puts on the pit building's
/// roof (`Landmark::pit_roof`).
const PIT_ROOF_STAMP: &str = "pit roof";

/// The stamp the surroundings pass gives the floodlight masts it puts
/// round a circuit that maps no lighting (`surroundings::floodlights`).
pub const FLOODLIGHT_STAMP: &str = "surroundings: floodlights";

/// A mast the dressing pass generated rather than the circuit's own: the
/// export marks it night-only, so by day the circuit looks as it is.
pub fn generated_floodlight(prop: &Prop) -> bool {
    prop.asset == "floodlight_tower" && prop.source.as_deref() == Some(FLOODLIGHT_STAMP)
}

/// A prop the dressing pass stood on the pit building's roof, which
/// grooming leaves where it is.
pub fn on_the_pit_roof(prop: &Prop) -> bool {
    prop.source
        .as_deref()
        .is_some_and(|s| s.starts_with(PIT_ROOF_STAMP))
}

/// How high the pit building's roof stands over the ground: the generated
/// garages, or the storey the export stacks on them
/// (`CircuitStyle::pit_storey_for`), or a circuit's own pit building.
fn pit_roof_height(scene: &AtsScene) -> f32 {
    let (kind, asset) = if !crate::circuit_style::CircuitStyle::for_scene(scene).pit_garages {
        (PropKind::Building, "pit_building_roofdeck")
    } else {
        (
            PropKind::Pit,
            crate::circuit_style::CircuitStyle::pit_storey_for(scene).unwrap_or("garage_6m"),
        )
    };
    props::resolve(kind, asset).map_or(10.0, |a| a.height_m)
}

fn build_pit_lane(path: &CenterlinePath, layout: &Layout) -> Option<PitLane> {
    let road = layout.pit_lane.as_ref()?;
    if road.nodes.len() < 3 {
        return None;
    }
    let sign = match road.side {
        Side::Left => 1.0f32,
        Side::Right => -1.0,
    };

    // The mapped lane every PIT_LANE_STEP_M along itself, smoothed (a GPS
    // trace's wander is not the lane's shape); its ends stay put.
    let mapped = resample_polyline(&road.nodes, PIT_LANE_STEP_M);
    let length = (mapped.len() - 1) as f32 * PIT_LANE_STEP_M;
    if length < PIT_MIN_LANE_M {
        return None;
    }
    let mapped = smooth_points(&mapped, PIT_LANE_SMOOTH_M / PIT_LANE_STEP_M);

    // Each point against its own leg of the course: looked for near where
    // the lane's progress says it should be, so a lane running beside one
    // leg of a folded circuit is not read against another.
    // Over the exit taper each point is read against the road nearest it:
    // that is the road it merges into, and after a lane that ran far from
    // the course (Silverstone's, a hundred metres out) the search near the
    // lane's progress has lost its leg, and the exit was pulled across
    // the infield to the wrong one.
    let taper = (length * 0.2).clamp(PIT_TAPER_MIN_M, PIT_TAPER_MAX_M);
    let exit_from = mapped
        .len()
        .saturating_sub((taper / PIT_LANE_STEP_M) as usize + 1);
    // Where the lane's own progress puts each point along the course: from
    // the station beside its entry to the one beside its exit. A lane that
    // runs far from the course loses its leg in the window (Silverstone's,
    // cutting a hundred metres inside Club, stayed matched to Vale and was
    // seated at Vale's height all along the Hamilton straight, three metres
    // under it); a point the window puts further off than PIT_LEG_LOST_M is
    // looked for again around there, and the nearer of the two kept.
    let total = path.total_length_m();
    let entry_s = nearest_cross_section(path, mapped[0][0], mapped[0][1])
        .0
        .station_m;
    let last = mapped[mapped.len() - 1];
    let exit_s = nearest_cross_section(path, last[0], last[1]).0.station_m;
    let span = if path.is_closed() {
        (exit_s - entry_s).rem_euclid(total)
    } else {
        exit_s - entry_s
    };
    let off = |p: &[f32; 2], (sample, _): &(PathSample, f32)| {
        (p[0] - sample.pos.0).hypot(p[1] - sample.pos.1)
    };
    let mut station: Option<f32> = None;
    let frame: Vec<(PathSample, f32)> = mapped
        .iter()
        .enumerate()
        .map(|(i, p)| {
            let found = match station {
                _ if i >= exit_from => nearest_cross_section(path, p[0], p[1]),
                Some(s) => {
                    let near = nearest_cross_section_near(
                        path,
                        p[0],
                        p[1],
                        s + PIT_LANE_STEP_M,
                        PIT_LEG_WINDOW_M,
                    );
                    if off(p, &near) <= PIT_LEG_LOST_M {
                        near
                    } else {
                        let along = entry_s + span * i as f32 / (mapped.len() - 1) as f32;
                        let along = if path.is_closed() {
                            along.rem_euclid(total)
                        } else {
                            along
                        };
                        let again =
                            nearest_cross_section_near(path, p[0], p[1], along, PIT_LEG_WINDOW_M);
                        if off(p, &again) < off(p, &near) {
                            again
                        } else {
                            near
                        }
                    }
                }
                None => nearest_cross_section(path, p[0], p[1]),
            };
            station = Some(found.0.station_m);
            found
        })
        .collect();

    // How far across its leg each point should be, positive on the lane's
    // side. Through the middle: pushed out to a pit wall's apron off the
    // road where it is mapped closer, but on the inside of a bend never
    // pushed so far in that it leaves the lane less than
    // PIT_LANE_MIN_RADIUS_M (an offset of a bend is tighter than the bend,
    // and past its radius folds back on itself); a point mapped further out
    // stays where it is. At the ends: from just inside the road edge out to
    // that over a taper.
    let half = PIT_WIDTH_M / 2.0;
    let ease = |t: f32| {
        let t = t.clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    };
    let shift: Vec<f32> = frame
        .iter()
        .enumerate()
        .map(|(i, (sample, lat))| {
            let road_half = side_half_width(sample, road.side);
            let across = lat * sign;
            let tightest = (-4..=4)
                .map(|k| curvature_at(path, sample.station_m + k as f32 * 5.0))
                .filter(|k| k * sign > 0.0)
                .fold(0.0f32, |a, k| a.max(k.abs()));
            let cap = if tightest < 1e-4 {
                f32::MAX
            } else {
                (1.0 / tightest - PIT_LANE_MIN_RADIUS_M).max(road_half - PIT_MERGE_OVERLAP_M)
            };
            // A point read against the wrong side of its leg (another leg
            // came nearer) is left where it was mapped.
            let middle = if across < -road_half {
                across
            } else {
                across.max((road_half + PIT_APRON_M + half).min(cap))
            };
            let u = i as f32 * PIT_LANE_STEP_M;
            let w = ease(u / taper) * ease((length - u) / taper);
            let want = (1.0 - w) * (road_half - PIT_MERGE_OVERLAP_M) + w * middle;
            want - across
        })
        .collect();
    // Smoothed along the lane, the two ends kept exactly on the road edge.
    let mut smoothed = smooth_values(&shift, PIT_LANE_SMOOTH_M / PIT_LANE_STEP_M);
    let last = shift.len() - 1;
    smoothed[0] = shift[0];
    smoothed[last] = shift[last];
    let shift = smoothed;
    let nodes: Vec<[f32; 3]> = mapped
        .iter()
        .zip(&frame)
        .zip(&shift)
        .map(|((p, (sample, _)), d)| {
            let (sin, cos) = sample.heading_rad.sin_cos();
            let edge = offset_point(sample, sign * side_half_width(sample, road.side));
            [p[0] - sin * sign * d, p[1] + cos * sign * d, edge.2]
        })
        .collect();
    // Anything still tighter than a car takes at the limit is rounded off
    // on its inside (the ends stay where they join the road).
    let nodes = round_tight_bends(nodes, PIT_LANE_MIN_RADIUS_M);
    // Each node at the height of the road beside where it ended up — the
    // edge, or the road itself on a taper over it — not beside where it was
    // mapped: moved, a node on a slope was a metre off its verge.
    let seated: Vec<([f32; 3], bool)> = nodes
        .iter()
        .zip(&frame)
        .map(|(n, (mapped_at, _))| {
            let (sample, lat) =
                nearest_cross_section_near(path, n[0], n[1], mapped_at.station_m, PIT_LEG_WINDOW_M);
            let on_road = lat.clamp(-sample.width_right_m, sample.width_left_m);
            (
                [n[0], n[1], offset_point(&sample, on_road).2],
                on_road == lat,
            )
        })
        .collect();
    let pinned: Vec<bool> = seated.iter().map(|s| s.1).collect();
    let nodes = ease_steep_grades(seated.into_iter().map(|s| s.0).collect(), &pinned);

    // The garages line the stretch between the tapers.
    let clear = (length - 2.0 * taper).max(0.0);
    let room = (clear / PIT_BOX_PITCH_M) as u32;
    // Every car its own garage: a full grid's worth where the lane has
    // room.
    let box_count = PIT_GRID_BOXES
        .min(room)
        .clamp(*PIT_BOXES.start(), *PIT_BOXES.end());
    Some(PitLane {
        nodes,
        width_m: PIT_WIDTH_M,
        box_count,
        speed_limit_kmh: PIT_SPEED_LIMIT_KMH,
        authored: true,
        podium_m: road.podium_m,
    })
}

/// `nodes` with every stretch steeper than [`PIT_LANE_MAX_GRADE`] eased
/// out into a ramp, the ends and the `pinned` nodes (those over the road,
/// which must stay on it) kept. Off the road the nearest road can change
/// legs from one node to the next: Spa's exit, cutting past La Source from
/// the pit straight's side to the downhill's banked low edge, stepped down
/// 3.6 m in 2 m, and the ground with it. Only a node beside a too-steep
/// step moves, to between its neighbours, so a lane on a steady slope stays
/// at its road's height.
fn ease_steep_grades(mut nodes: Vec<[f32; 3]>, pinned: &[bool]) -> Vec<[f32; 3]> {
    let n = nodes.len();
    if n < 3 {
        return nodes;
    }
    let run = |a: &[f32; 3], b: &[f32; 3]| (b[0] - a[0]).hypot(b[1] - a[1]).max(1e-3);
    for _ in 0..PIT_LANE_GRADE_PASSES {
        let mut moved = false;
        for i in 1..n - 1 {
            if pinned[i] {
                continue;
            }
            let (a, b) = (run(&nodes[i - 1], &nodes[i]), run(&nodes[i], &nodes[i + 1]));
            let (z0, z, z1) = (nodes[i - 1][2], nodes[i][2], nodes[i + 1][2]);
            let steepest = ((z - z0) / a).abs().max(((z1 - z) / b).abs());
            let between = (z0 * b + z1 * a) / (a + b);
            if steepest > PIT_LANE_MAX_GRADE && (between - z).abs() > 1e-3 {
                nodes[i][2] = between;
                moved = true;
            }
        }
        if !moved {
            break;
        }
    }
    nodes
}

/// A polyline resampled every `step` metres along itself, both ends kept.
fn resample_polyline(points: &[[f32; 2]], step: f32) -> Vec<[f32; 2]> {
    let total: f32 = points
        .windows(2)
        .map(|w| (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]))
        .sum();
    let n = (total / step).round().max(1.0) as usize;
    (0..=n)
        .map(|i| {
            let (x, y) = point_along(points, total * i as f32 / n as f32);
            [x, y]
        })
        .collect()
}

/// Gaussian smoothing of a sequence, `sigma` in samples, the ends held.
fn smooth_values(values: &[f32], sigma: f32) -> Vec<f32> {
    let reach = (3.0 * sigma).ceil() as isize;
    (0..values.len())
        .map(|i| {
            let (mut sum, mut weight) = (0.0f32, 0.0f32);
            for k in -reach..=reach {
                let j = (i as isize + k).clamp(0, values.len() as isize - 1) as usize;
                let w = (-0.5 * (k as f32 / sigma).powi(2)).exp();
                sum += values[j] * w;
                weight += w;
            }
            sum / weight
        })
        .collect()
}

/// [`smooth_values`] on both coordinates, with the first and last point
/// pinned where they are.
fn smooth_points(points: &[[f32; 2]], sigma: f32) -> Vec<[f32; 2]> {
    let xs = smooth_values(&points.iter().map(|p| p[0]).collect::<Vec<_>>(), sigma);
    let ys = smooth_values(&points.iter().map(|p| p[1]).collect::<Vec<_>>(), sigma);
    let last = points.len() - 1;
    (0..points.len())
        .map(|i| {
            if i == 0 || i == last {
                points[i]
            } else {
                [xs[i], ys[i]]
            }
        })
        .collect()
}

/// Relax a polyline's nodes tighter than `min_radius_m` toward the middle
/// of their neighbours until none is (or the passes run out), first and
/// last node fixed.
fn round_tight_bends(mut nodes: Vec<[f32; 3]>, min_radius_m: f32) -> Vec<[f32; 3]> {
    let radius = |a: &[f32; 3], b: &[f32; 3], c: &[f32; 3]| {
        let ab = (b[0] - a[0]).hypot(b[1] - a[1]);
        let bc = (c[0] - b[0]).hypot(c[1] - b[1]);
        let ca = (a[0] - c[0]).hypot(a[1] - c[1]);
        let cross = ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])).abs();
        if cross < 1e-6 {
            f32::MAX
        } else {
            ab * bc * ca / (2.0 * cross)
        }
    };
    for _ in 0..PIT_LANE_ROUND_PASSES {
        let tight: Vec<usize> = (1..nodes.len().saturating_sub(1))
            .filter(|&i| radius(&nodes[i - 1], &nodes[i], &nodes[i + 1]) < min_radius_m)
            .collect();
        if tight.is_empty() {
            break;
        }
        // The neighbours of a tight node move too, a little, or a single
        // node is pulled flat and its neighbours become the cusp.
        let mut weight = vec![0.0f32; nodes.len()];
        for &i in &tight {
            weight[i] = 0.5;
            for j in [i - 1, i + 1] {
                if j > 0 && j + 1 < nodes.len() {
                    weight[j] = weight[j].max(0.25);
                }
            }
        }
        let before = nodes.clone();
        for i in 1..nodes.len() - 1 {
            if weight[i] == 0.0 {
                continue;
            }
            for k in 0..3 {
                let mid = (before[i - 1][k] + before[i + 1][k]) / 2.0;
                nodes[i][k] += (mid - before[i][k]) * weight[i];
            }
        }
    }
    nodes
}

// ---- The pit zone ---------------------------------------------------------

/// What a dressed prop must keep out of: the pit lane, the pit wall's
/// apron on its road side and the garages' depth behind it, which the
/// bake fills (`ue_export::bake_pit_complex`). A stand or a building only
/// used to be kept from the lane by its centre, so a long terrace of
/// media-centre blocks whose middle stood clear could still put a block
/// across the lane: Suzuka's and Catalunya's did, and a car driving in
/// met a wall.
struct PitZone {
    lane: CenterlinePath,
    half: f32,
    /// +1 when the garages are on the lane's left.
    garage_side: f32,
    /// The lane stations the garage row runs between
    /// ([`crate::ue_export::garage_row_span`]); `None` keeps the garages'
    /// depth clear the whole length of the lane.
    row: Option<(f32, f32)>,
}

/// A roof screen stands this far past the lane's edge: over the middle of
/// a garage (`pit/garage_6m` is 14.2 m deep).
const PIT_ROOF_SPOT_M: f32 = 7.0;

/// The zone reaches this far past the lane's road-side edge (the apron
/// and the pit wall)…
const PIT_ZONE_APRON_M: f32 = 2.5;
/// …and this far past its garage-side edge (a garage's depth and a
/// little).
const PIT_ZONE_GARAGES_M: f32 = 17.0;
/// The garages' depth is kept clear this far past either end of their
/// row too: the pit complex's end blocks and the lane's apron there.
const PIT_ZONE_ROW_MARGIN_M: f32 = 12.0;

impl PitZone {
    fn new(pit: &PitLane, side: Side, row: Option<(f32, f32)>) -> Option<Self> {
        let lane = CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0)?;
        Some(Self {
            lane,
            half: pit.width_m / 2.0,
            // The lane runs the course's way, so on the course's left the
            // garages are further left still.
            garage_side: match side {
                Side::Left => 1.0,
                Side::Right => -1.0,
            },
            row,
        })
    }

    fn contains(&self, x: f32, y: f32) -> bool {
        let (toward_garages, by_the_garages, beyond_an_end) = self.locate(x, y);
        if beyond_an_end {
            return false;
        }
        if toward_garages >= 0.0 && by_the_garages {
            toward_garages < self.half + PIT_ZONE_GARAGES_M
        } else {
            toward_garages.abs() < self.half + PIT_ZONE_APRON_M
        }
    }

    /// Whether `(x, y)` stands over the garage row: seating there is the
    /// terrace over the garages, which the bake builds as part of the pit
    /// complex from the lane.
    /// The lane station nearest `(x, y)`.
    fn lane_station(&self, x: f32, y: f32) -> f32 {
        self.lane
            .samples()
            .iter()
            .min_by(|a, b| {
                let d = |s: &PathSample| (s.pos.0 - x).hypot(s.pos.1 - y);
                d(a).total_cmp(&d(b))
            })
            .map_or(0.0, |s| s.station_m)
    }

    /// `n` spots spread evenly along the garage row, over the middle of
    /// the garages' depth: where screens stand on the pit building's roof.
    fn roof_spots(&self, n: usize) -> Vec<(f32, f32)> {
        let (from, to) = self.row.unwrap_or((0.0, self.lane.total_length_m()));
        (0..n)
            .map(|k| {
                let sample = self
                    .lane
                    .sample_at(from + (to - from) * (k as f32 + 0.5) / n as f32);
                let p = offset_point(&sample, self.garage_side * (self.half + PIT_ROOF_SPOT_M));
                (p.0, p.1)
            })
            .collect()
    }

    /// Whether `(x, y)` is on the garages' own footprint, past the lane's
    /// edge: where the pit building's roof is.
    fn on_the_garage_roof(&self, x: f32, y: f32) -> bool {
        let (toward_garages, by_the_garages, beyond_an_end) = self.locate(x, y);
        !beyond_an_end
            && by_the_garages
            && (self.half..self.half + PIT_ZONE_GARAGES_M).contains(&toward_garages)
    }

    fn over_the_garages(&self, x: f32, y: f32) -> bool {
        let (toward_garages, by_the_garages, beyond_an_end) = self.locate(x, y);
        !beyond_an_end
            && by_the_garages
            && (0.0..self.half + PIT_ZONE_GARAGES_M).contains(&toward_garages)
    }

    /// Where `(x, y)` is from the lane: how far toward the garages (negative
    /// toward the road), whether it is level with the garage row, and
    /// whether it is past either end of the lane.
    fn locate(&self, x: f32, y: f32) -> (f32, bool, bool) {
        let (mut best, mut lat, mut station) = (f32::MAX, 0.0f32, 0.0f32);
        let mut at_end = false;
        let samples = self.lane.samples();
        for (i, sample) in samples.iter().enumerate() {
            let (dx, dy) = (x - sample.pos.0, y - sample.pos.1);
            let d2 = dx * dx + dy * dy;
            if d2 < best {
                best = d2;
                let (sin, cos) = sample.heading_rad.sin_cos();
                lat = -sin * dx + cos * dy;
                station = sample.station_m;
                at_end = i == 0 || i == samples.len() - 1;
            }
        }
        // Beyond either end of the lane is not beside it.
        let beyond_an_end = at_end && best.sqrt() > lat.abs() + 1.0;
        let by_the_garages = self.row.is_none_or(|(from, to)| {
            (from - PIT_ZONE_ROW_MARGIN_M..=to + PIT_ZONE_ROW_MARGIN_M).contains(&station)
        });
        (lat * self.garage_side, by_the_garages, beyond_an_end)
    }

    /// Whether any part of `prop`'s footprint stands in the zone.
    fn blocks(&self, path: &CenterlinePath, prop: &Prop) -> bool {
        let kit = props::resolve(prop.kind, &prop.asset);
        let (length, depth) = match prop.kind {
            PropKind::Grandstand => (
                prop.length_m.unwrap_or(30.0) * prop.scale,
                kit.map_or(12.0, |a| a.depth_m),
            ),
            PropKind::Building => kit.map_or((15.0, 10.0), |a| {
                (a.length_m * prop.scale, a.depth_m * prop.scale)
            }),
            _ => {
                // A landmark wheel is 150 m across but only 37 m deep: its
                // circle of influence would reach garages it never touches.
                let r = if prop.asset.starts_with("landmark_") {
                    0.0
                } else {
                    dressed_footprint_radius_m(prop) * prop.scale
                };
                return (0..=8).any(|k| {
                    let (dx, dy) = if k == 8 {
                        (0.0, 0.0)
                    } else {
                        let a = k as f32 * std::f32::consts::FRAC_PI_4;
                        (a.cos() * r, a.sin() * r)
                    };
                    self.contains(prop.x + dx, prop.y + dy)
                });
            }
        };
        // A stand or a building stands on its front: its footprint runs
        // from the pivot away from the road.
        let (sin, cos) = prop.yaw_rad.sin_cos();
        let (near, _) = nearest_cross_section(path, prop.x, prop.y);
        let toward = -sin * (near.pos.0 - prop.x) + cos * (near.pos.1 - prop.y);
        let away = if toward >= 0.0 {
            (sin, -cos)
        } else {
            (-sin, cos)
        };
        (0..=FOOTPRINT_PROBES).any(|a| {
            let along = (a as f32 / FOOTPRINT_PROBES as f32 - 0.5) * length;
            (0..=FOOTPRINT_PROBES).any(|b| {
                let back = b as f32 / FOOTPRINT_PROBES as f32 * depth;
                self.contains(
                    prop.x + cos * along + away.0 * back,
                    prop.y + sin * along + away.1 * back,
                )
            })
        })
    }
}

// ---- Grandstands ----------------------------------------------------------

/// The bay family a stand is built from: `_large` once it is deep enough
/// to be a two-tier stand, roofed when the dossier says so (or when it is
/// too deep to be anything else, unless its seating map says it is open).
fn stand_family(stand: &Stand) -> &'static str {
    // The street circuit's tall stand: one family, roofed or not.
    if stand.family.as_deref() == Some("street_deck") {
        return "street_stand_deck_10m";
    }
    if stand.family.as_deref() == Some("street") {
        return if stand.covered {
            "street_stand_tier_10m_roof"
        } else {
            "street_stand_tier_10m"
        };
    }
    // The other families a dossier names (`MANUAL_STANDS`' or
    // `STAND_FAMILY`'s `family`): general-admission banks and temporary
    // stands (R2), the venue modules, and the stadium stand for a tall
    // single-roof stand the depth rule takes for a two-tier one (R13).
    match stand.family.as_deref() {
        Some("bank") => return "grass_bank_10m",
        Some("scaffold") => return "scaffold_10m",
        Some("banking_seats") => return "banking_seats",
        Some("tent") => return "tent_roof_10m",
        Some("palm") => return "palm_canopy_10m",
        Some("lotus") => return "lotus_canopy_10m",
        Some("stone") => return "stone_tribune_10m",
        Some("stone_centre") => return "stone_tribune_centre_10m",
        Some("stadium") => return "bay_10m_stadium_roof",
        _ => {}
    }
    let large = stand.depth_m >= LARGE_STAND_DEPTH_M;
    let roof = match stand.roof.as_deref() {
        Some("covered") => true,
        Some("open") => false,
        _ => stand.covered || stand.depth_m >= ASSUMED_ROOF_DEPTH_M,
    };
    match (large, roof) {
        (true, true) => "bay_10m_large_roof",
        (true, false) => "bay_10m_large",
        (false, true) => "bay_10m_roof",
        (false, false) => "bay_10m",
    }
}

/// Lay one stand as a chain of runs along the outline that faces the road.
/// Each run becomes one grandstand prop carrying its own `length_m`, so
/// the Unreal side lays `round(length / 10)` bays there; a stand built
/// round a bend comes out as a chain of runs that follows it.
/// A laid stand whose bays stand nearer another stretch of the course than
/// its own (a dossier side error puts a main stand across the straight
/// and against the next leg: Sepang's stood on the back straight). Its
/// own stretch is its station give or take half its length and
/// [`STAND_LEG_SLACK_M`].
fn stand_off_its_leg(path: &CenterlinePath, stand: &Stand, props: &[Prop]) -> Option<String> {
    let total = path.total_length_m();
    let along = |a: f32, b: f32| {
        let d = (a - b).abs();
        if path.is_closed() {
            d.min(total - d)
        } else {
            d
        }
    };
    let reach = stand.length_m / 2.0 + STAND_LEG_SLACK_M;
    let off: Vec<f32> = props
        .iter()
        .map(|p| nearest_cross_section(path, p.x, p.y).0.station_m)
        .filter(|&s| along(s, stand.station_m) > reach)
        .collect();
    if off.len() * 2 <= props.len() {
        return None;
    }
    Some(format!(
        "stand {}: laid nearer the road at {:.0} m than at its own {:.0} m (the far side of a leg?)",
        name_of(&stand.name),
        off[off.len() / 2],
        stand.station_m
    ))
}

fn lay_stand(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    zone: Option<&PitZone>,
    surfaces: &[Surface],
    stand: &Stand,
) -> Vec<Prop> {
    let run_len_of = |pts: &[[f32; 2]]| -> f32 {
        pts.windows(2)
            .map(|w| (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]))
            .sum()
    };
    let mut front = stand_front(stand);
    let mut total = run_len_of(&front);
    // A traced outline whose road-facing run came out much shorter than
    // the stand is one where the near face is an end wall (a small square
    // stand, or one the trace cut at a corner): fall back to the long
    // side of its box, which is the row of seats.
    if total < 0.6 * stand.length_m {
        front = box_front(stand);
        total = run_len_of(&front);
    }
    if total < MIN_STAND_M || front.len() < 2 {
        return Vec::new();
    }
    let asset = stand_family(stand);
    let bay_depth = props::resolve(PropKind::Grandstand, asset).map_or(10.0, |a| a.depth_m);
    let runs = (total / STAND_RUN_M).ceil().max(1.0);
    let run_len = total / runs;
    let mut out = Vec::new();
    let mut walked = 0.0f32;
    for i in 0..runs as usize {
        let from = i as f32 * run_len;
        let to = from + run_len;
        let a = point_along(&front, from);
        let b = point_along(&front, to);
        let len = (b.0 - a.0).hypot(b.1 - a.1);
        if len < BAY_PITCH_M * 0.5 {
            continue;
        }
        let (x, y) = ((a.0 + b.0) / 2.0, (a.1 + b.1) / 2.0);
        let yaw = (b.1 - a.1).atan2(b.0 - a.0);
        // The front is the real one, but the kit bay is as deep as it is:
        // a front traced a few metres off the asphalt put the back rows of
        // a deep bay — or the front of one on the far side of a bend —
        // over the road. Step the run back until the whole bay is clear,
        // of a paved run-off too (and a bank further, behind the barrier
        // line): the server stops cars on stands.
        let (sample, lat) = nearest_cross_section(path, x, y);
        let side = if lat >= 0.0 { Side::Left } else { Side::Right };
        let runoff = runoff_reach(surfaces, path.total_length_m(), sample.station_m, side);
        let mut clear = STAND_ROAD_CLEAR_M.max(if runoff > 0.0 {
            runoff + STAND_RUNOFF_CLEAR_M
        } else {
            0.0
        });
        if asset == "grass_bank_10m" {
            clear = clear.max(BANK_ROAD_CLEAR_M);
        }
        let Some((x, y)) = clear_footprint(path, x, y, yaw, len, bay_depth, clear) else {
            continue;
        };
        let run = |x: f32, y: f32| Prop {
            id: 0,
            kind: PropKind::Grandstand,
            asset: asset.to_string(),
            x,
            y,
            z: 0.0,
            yaw_rad: yaw,
            scale: 1.0,
            text: None,
            length_m: Some(round2(len)),
            source: None,
        };
        // Seating over the garage row is the pit complex's terrace, which
        // the bake builds; a run in the lane or its apron elsewhere (a
        // stand behind the pit entry or exit road) is stepped further back
        // from the road out of it.
        let Some((x, y)) = clear_of_pit_zone(path, zone, x, y, yaw, len, bay_depth, &run) else {
            continue;
        };
        if asset == "grass_bank_10m" {
            out.extend(standing_crowd(path, terrain, x, y, yaw, len, stand.depth_m));
            walked = to;
            continue;
        }
        let (sample, lat) = nearest_cross_section(path, x, y);
        out.push(Prop {
            z: seat_z(terrain, &sample, lat, x, y),
            ..run(x, y)
        });
        walked = to;
    }
    let _ = walked;
    out
}

/// A general-admission area is people standing about on the ground, not a
/// terrace: knots of spectators (`misc/crowd_group`) scattered over a run's
/// footprint, `len` along its front through `(x, y)` and `depth` back from
/// the road, at uneven spacing, thinning toward the back, each turned
/// roughly to the road. A raked bank of rows read as one more grandstand.
fn standing_crowd(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    x: f32,
    y: f32,
    yaw: f32,
    len: f32,
    depth: f32,
) -> Vec<Prop> {
    use surroundings::roll;
    let depth = depth.clamp(GA_ROW_M, GA_MAX_DEPTH_M);
    let (_, away) = footprint_road_gap(path, x, y, yaw, len, depth);
    let along_dir = yaw.sin_cos();
    let rows = (depth / GA_ROW_M).round().max(1.0) as usize;
    let per_row = (len / GA_PITCH_M).round().max(1.0) as usize;
    let mut out = Vec::new();
    for r in 0..rows {
        for i in 0..per_row {
            let salt = (r * 64 + i) as u32;
            // Fewer people further from the fence.
            if roll(x, y, 700 + salt) < r as f32 * 0.25 {
                continue;
            }
            let along =
                -len / 2.0 + (i as f32 + 0.2 + 0.6 * roll(x, y, 800 + salt)) * len / per_row as f32;
            let back = (r as f32 + 0.25 + 0.5 * roll(x, y, 900 + salt)) * GA_ROW_M;
            let px = x + along_dir.1 * along + away.0 * back;
            let py = y + along_dir.0 * along + away.1 * back;
            let (sample, lat) = nearest_cross_section(path, px, py);
            out.push(Prop {
                id: 0,
                kind: PropKind::Misc,
                asset: "crowd_group".to_string(),
                x: px,
                y: py,
                z: seat_z(terrain, &sample, lat, px, py),
                yaw_rad: sample.heading_rad + (roll(x, y, 1000 + salt) - 0.5) * 0.7,
                scale: 1.0,
                text: None,
                length_m: None,
                source: None,
            });
        }
    }
    out
}

/// Where a stand run at `(x, y)` stands clear of the pit `zone`: its own
/// spot when it already does, else stepped straight back from the road in
/// 1 m steps, up to [`BUILDING_MAX_PUSH_M`]. `None` over the garage row,
/// or when there is no such spot.
#[allow(clippy::too_many_arguments)]
fn clear_of_pit_zone(
    path: &CenterlinePath,
    zone: Option<&PitZone>,
    x: f32,
    y: f32,
    yaw: f32,
    length: f32,
    depth: f32,
    run: &dyn Fn(f32, f32) -> Prop,
) -> Option<(f32, f32)> {
    let Some(zone) = zone else {
        return Some((x, y));
    };
    if zone.over_the_garages(x, y) {
        return None;
    }
    let (_, away) = footprint_road_gap(path, x, y, yaw, length, depth);
    let (mut x, mut y) = (x, y);
    let mut pushed = 0.0f32;
    while zone.blocks(path, &run(x, y)) {
        pushed += 1.0;
        if pushed > BUILDING_MAX_PUSH_M {
            return None;
        }
        x += away.0;
        y += away.1;
    }
    Some((x, y))
}

/// Smallest distance from the road edge — any section of the course —
/// over a footprint whose front runs `length` along `yaw` through
/// `(x, y)` and which reaches `depth` back, away from the road (the way
/// the Unreal builder turns a face-road prop: away from the centerline
/// point nearest its pivot). Negative is on the asphalt.
fn footprint_road_gap(
    path: &CenterlinePath,
    x: f32,
    y: f32,
    yaw: f32,
    length: f32,
    depth: f32,
) -> (f32, (f32, f32)) {
    let (sin, cos) = yaw.sin_cos();
    let (near, _) = nearest_cross_section(path, x, y);
    let side = -sin * (near.pos.0 - x) + cos * (near.pos.1 - y);
    let away = if side >= 0.0 {
        (sin, -cos)
    } else {
        (-sin, cos)
    };
    let mut worst = f32::MAX;
    for a in 0..=FOOTPRINT_PROBES {
        let along = (a as f32 / FOOTPRINT_PROBES as f32 - 0.5) * length;
        for b in 0..=FOOTPRINT_PROBES {
            let back = b as f32 / FOOTPRINT_PROBES as f32 * depth;
            let px = x + cos * along + away.0 * back;
            let py = y + sin * along + away.1 * back;
            let (sample, lat) = nearest_cross_section(path, px, py);
            let side = if lat >= 0.0 { Side::Left } else { Side::Right };
            worst = worst.min(lat.abs() - side_half_width(&sample, side));
        }
    }
    (worst, away)
}

/// Where a footprint can stand at least `clear` metres from every part of
/// the road: its own spot when it already does, else stepped straight back
/// from the road in 1 m steps, up to [`BUILDING_MAX_PUSH_M`]. `None` when
/// there is no such spot (the far side of the step is road too).
fn clear_footprint(
    path: &CenterlinePath,
    x: f32,
    y: f32,
    yaw: f32,
    length: f32,
    depth: f32,
    clear: f32,
) -> Option<(f32, f32)> {
    let (mut x, mut y) = (x, y);
    let (_, away) = footprint_road_gap(path, x, y, yaw, length, depth);
    let mut pushed = 0.0f32;
    loop {
        let (gap, _) = footprint_road_gap(path, x, y, yaw, length, depth);
        if gap >= clear - 0.05 {
            return Some((x, y));
        }
        pushed += 1.0;
        if pushed > BUILDING_MAX_PUSH_M {
            return None;
        }
        x += away.0;
        y += away.1;
    }
}

/// The stand's road-facing outline: the dossier's `front` where it has
/// one, otherwise [`box_front`].
fn stand_front(stand: &Stand) -> Vec<[f32; 2]> {
    if stand.front.len() >= 2 {
        return stand.front.clone();
    }
    box_front(stand)
}

/// The long axis of the stand's box, through its centre.
fn box_front(stand: &Stand) -> Vec<[f32; 2]> {
    let (sin, cos) = stand.yaw_rad.sin_cos();
    let half = stand.length_m / 2.0;
    let (cx, cy) = (stand.centre[0], stand.centre[1]);
    vec![
        [cx - cos * half, cy - sin * half],
        [cx + cos * half, cy + sin * half],
    ]
}

/// Point at arc length `d` along a polyline (clamped to its ends).
fn point_along(pts: &[[f32; 2]], d: f32) -> (f32, f32) {
    let mut left = d.max(0.0);
    for w in pts.windows(2) {
        let seg = (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]);
        if seg <= f32::EPSILON {
            continue;
        }
        if left <= seg {
            let t = left / seg;
            return (
                w[0][0] + (w[1][0] - w[0][0]) * t,
                w[0][1] + (w[1][1] - w[0][1]) * t,
            );
        }
        left -= seg;
    }
    let last = pts[pts.len() - 1];
    (last[0], last[1])
}

// ---- Buildings ------------------------------------------------------------

/// The kit building a real footprint reads as. The kit has four, so this
/// is a choice between a tower, a media centre, a hospitality block and a
/// clubhouse — decided by how tall, how big and how square the real one is.
fn building_asset(structure: &Structure, paddock: bool) -> (&'static str, f32) {
    let square = structure.depth_m / structure.length_m.max(1.0);
    if structure.levels >= TOWER_LEVELS && square > 0.6 && structure.length_m < 30.0 {
        return ("control_tower", 13.2);
    }
    // The media centre and hospitality blocks have open, colonnaded ground
    // floors: beside the pit lane they are the paddock, anywhere else they
    // read as a row of garages. Away from it a big building is an office
    // block, or a hall when the map gives it one storey.
    if paddock {
        if structure.length_m >= 35.0 && structure.depth_m >= 12.0 {
            return ("media_centre", 40.6);
        }
        if structure.levels >= 2 || structure.area_m2 >= 700.0 {
            return ("hospitality_3f", 30.6);
        }
    } else if structure.area_m2 >= 700.0 || structure.levels >= 2 {
        if structure.levels == 1 && structure.area_m2 >= HALL_AREA_M2 {
            return ("venue_hall", 40.6);
        }
        return ("office_block", 40.1);
    }
    ("clubhouse", 25.6)
}

/// The `skyline_*` block a street circuit's neighbour reads as, and how
/// many of them make its row: by footprint, and for the mid-size ones by a
/// hash of where it stands, so a street of offices is not one tower copied.
fn city_building_asset(structure: &Structure) -> (&'static str, usize) {
    const MIX: [&str; 7] = [
        "skyline_slab_a",
        "skyline_slab_b",
        "skyline_step",
        "skyline_pyramid",
        "skyline_cylinder",
        "skyline_twin",
        "skyline_needle",
    ];
    let hash = ((structure.centre[0] * 7.0).floor() as i64)
        .wrapping_mul(31)
        .wrapping_add((structure.centre[1] * 7.0).floor() as i64)
        .unsigned_abs() as usize;
    // What the map says about its height beats a guess from the footprint.
    if (1..=8).contains(&structure.levels) || structure.area_m2 < 900.0 {
        ("skyline_lowrise", 1)
    } else if structure.levels >= 40 {
        ("skyline_needle", 1)
    } else if structure.area_m2 >= 3500.0 {
        let row = (structure.length_m / 40.0).round().clamp(1.0, 6.0) as usize;
        ("skyline_podium", row)
    } else {
        (MIX[hash % MIX.len()], 1)
    }
}

/// Lay a kit building that is centred on its footprint (`landmark_*`,
/// `building_*`, `skyline_*`) on a real footprint: one unit, or a row
/// along its long axis. Each unit is stepped back from the road until its
/// whole footprint is clear of it.
fn lay_centred_building(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    structure: &Structure,
    asset: &str,
    row: usize,
) -> Result<Vec<Prop>, String> {
    let kit = props::find(PropKind::Building, asset)
        .ok_or_else(|| format!("{asset} is not in the kit"))?;
    let (sin, cos) = structure.yaw_rad.sin_cos();
    let units = row.max(1);
    let mut out = Vec::new();
    for i in 0..units {
        let along = (i as f32 - (units as f32 - 1.0) / 2.0) * kit.length_m;
        let (x, y) = (
            structure.centre[0] + cos * along,
            structure.centre[1] + sin * along,
        );
        let Some((x, y)) = clear_landmark(path, x, y, structure.yaw_rad, kit, structure.side)
        else {
            continue;
        };
        if terrain.in_water(x, y) {
            continue;
        }
        let (sample, lat) = nearest_cross_section(path, x, y);
        out.push(Prop {
            id: 0,
            kind: PropKind::Building,
            asset: asset.to_string(),
            x,
            y,
            z: seat_z(terrain, &sample, lat, x, y),
            yaw_rad: structure.yaw_rad,
            scale: 1.0,
            text: None,
            length_m: None,
            source: None,
        });
    }
    if out.is_empty() {
        Err("no room beside the road".to_string())
    } else {
        Ok(out)
    }
}

/// Place a building on its real footprint. A footprint longer than the
/// kit's block is filled with a row of them, which is what a 200 m
/// hospitality terrace looks like anyway.
fn lay_structure(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    lane: Option<&CenterlinePath>,
    surfaces: &[Surface],
    structure: &Structure,
    city: bool,
) -> Result<Vec<Prop>, String> {
    if structure.asset.as_deref() == Some("-") {
        return Err("covered by a landmark prop".to_string());
    }
    if structure.area_m2 < MIN_BUILDING_AREA_M2 {
        return Err("footprint too small".to_string());
    }
    // OSM maps a footbridge over the track as `building=bridge`; laid as
    // a row of clubhouses it stands across the road.
    if structure.osm_building.as_deref() == Some("bridge") {
        return Err("a bridge, not a building".to_string());
    }
    if let Some(lane) = lane {
        let (x, y) = (structure.centre[0], structure.centre[1]);
        if lane_gap(lane, x, y) < PIT_BUILDING_RANGE_M {
            return Err("in the pit complex, which the bake generates".to_string());
        }
    }
    // A landmark with a mesh of its own, or a city block on a street
    // circuit, stands centred on its footprint rather than being a row of
    // venue blocks pivoted on their fronts.
    if let Some(named) = structure.asset.as_deref() {
        if props::find(PropKind::Building, named).is_some() {
            return lay_centred_building(path, terrain, structure, named, 1);
        }
    }
    if city {
        let (asset, row) = city_building_asset(structure);
        return lay_centred_building(path, terrain, structure, asset, row);
    }
    let paddock = lane.is_some_and(|lane| {
        lane_gap(lane, structure.centre[0], structure.centre[1]) < PADDOCK_RANGE_M
    });
    let (asset, unit_m) = building_asset(structure, paddock);
    let units = (structure.length_m / unit_m).round().max(1.0) as usize;
    let (sin, cos) = structure.yaw_rad.sin_cos();
    // The pivot is the face toward the road: the kit's footprint runs from
    // the pivot away from it.
    let (centre_sample, centre_lat) =
        nearest_cross_section(path, structure.centre[0], structure.centre[1]);
    // Toward the road is whichever of the block's two sides faces the
    // nearest centerline point. Deciding it from the road's left/right
    // alone is only right while the block's yaw runs with the course; a
    // block mapped with the opposite yaw came out facing away.
    let to_road = (
        centre_sample.pos.0 - structure.centre[0],
        centre_sample.pos.1 - structure.centre[1],
    );
    let toward_road = if -sin * to_road.0 + cos * to_road.1 >= 0.0 {
        1.0
    } else {
        -1.0
    };
    let normal = (-sin * toward_road, cos * toward_road);
    let mut front_x = structure.centre[0] + normal.0 * structure.depth_m / 2.0;
    let mut front_y = structure.centre[1] + normal.1 * structure.depth_m / 2.0;
    let unit_at = |front: (f32, f32), i: usize| {
        let along = (i as f32 - (units as f32 - 1.0) / 2.0) * unit_m;
        (front.0 + cos * along, front.1 + sin * along)
    };

    // A block whose front is nearest the road on the far side of it is
    // where a long row reaches round a bend or onto another leg: it is left
    // out and the rest of the row kept, rather than the building dropped
    // whole (R14: Lakeside Stadium).
    let mut row: Vec<usize> = (0..units)
        .filter(|&i| {
            let (x, y) = unit_at((front_x, front_y), i);
            (nearest_cross_section(path, x, y).1 >= 0.0) == (centre_lat >= 0.0)
        })
        .collect();
    // A `building=roof` outline is a stand's or a car park's canopy: what
    // is left of it after trimming is no building at all.
    let roof = structure.osm_building.as_deref() == Some("roof");
    if row.is_empty() || (roof && row.len() < units) {
        return Err("reaches across the road".to_string());
    }

    // Move the row back until every unit's front is clear of the road.
    // The row's normal is not the road's where the building sits at an
    // angle to it, so the push is scaled by the cosine between them and
    // repeated; a row that runs across the road rather than beside it
    // cannot be pushed clear.
    let shortfall = |front: (f32, f32)| -> Result<f32, String> {
        let mut worst: f32 = 0.0;
        for &i in &row {
            let (x, y) = unit_at(front, i);
            let (sample, lat) = nearest_cross_section(path, x, y);
            if (lat >= 0.0) != (centre_lat >= 0.0) {
                return Err("reaches across the road".to_string());
            }
            let side = if lat >= 0.0 { Side::Left } else { Side::Right };
            worst = worst.max(side_half_width(&sample, side) + BUILDING_ROAD_CLEAR_M - lat.abs());
        }
        Ok(worst)
    };
    for _ in 0..4 {
        let short = shortfall((front_x, front_y))?;
        if short <= 0.05 {
            break;
        }
        let (sample, _) = nearest_cross_section(path, front_x, front_y);
        let cosine = (structure.yaw_rad - sample.heading_rad).cos().abs();
        if cosine < 0.5 {
            return Err("runs across the road".to_string());
        }
        front_x -= normal.0 * short / cosine;
        front_y -= normal.1 * short / cosine;
    }
    if shortfall((front_x, front_y))? > 0.5 {
        return Err("cannot be laid clear of the road".to_string());
    }
    // The fronts are clear; now the whole of every unit. The kit block
    // reaches `depth` back from its front and half its length either side,
    // and behind it there may be another section of the course: a building
    // in the infield of a hairpin, or one wedged between two legs that run
    // side by side, had its front checked against the near road only and
    // was laid with its back on the far one (Zandvoort's Hunserug).
    let kit_depth = props::resolve(PropKind::Building, asset).map_or(unit_m * 0.6, |a| a.depth_m);
    let away = (-normal.0, -normal.1);
    let unit_gap = |front: (f32, f32), i: usize| -> f32 {
        let mut worst = f32::MAX;
        let (ux, uy) = unit_at(front, i);
        for a in 0..=FOOTPRINT_PROBES {
            let along = (a as f32 / FOOTPRINT_PROBES as f32 - 0.5) * unit_m;
            for b in 0..=FOOTPRINT_PROBES {
                let back = b as f32 / FOOTPRINT_PROBES as f32 * kit_depth;
                let x = ux + cos * along + away.0 * back;
                let y = uy + sin * along + away.1 * back;
                let (sample, lat) = nearest_cross_section(path, x, y);
                let side = if lat >= 0.0 { Side::Left } else { Side::Right };
                worst = worst.min(lat.abs() - side_half_width(&sample, side));
            }
        }
        worst
    };
    let footprint_gap = |front: (f32, f32), row: &[usize]| -> f32 {
        row.iter()
            .map(|&i| unit_gap(front, i))
            .fold(f32::MAX, f32::min)
    };
    let mapped_front = (front_x, front_y);
    let mut pushed = 0.0f32;
    loop {
        let gap = footprint_gap((front_x, front_y), &row);
        if gap >= BUILDING_ROAD_CLEAR_M - 0.05 {
            break;
        }
        // Stepping back from the near road can only help when the far
        // side has room; walk until it does. A row that finds none keeps
        // the blocks clear where they stand and loses the ones on another
        // road section (R14: Lakeside Stadium, Barcelona's VIP Rooftop).
        pushed += 1.0;
        if pushed > BUILDING_MAX_PUSH_M {
            (front_x, front_y) = mapped_front;
            row.retain(|&i| unit_gap(mapped_front, i) >= BUILDING_ROAD_CLEAR_M - 0.05);
            if row.is_empty() || roof {
                return Err(format!(
                    "no room for its footprint between the road sections ({gap:.1} m to the asphalt)"
                ));
            }
            break;
        }
        front_x += away.0;
        front_y += away.1;
    }

    // A unit standing on a paved run-off apron is left out: clear of the asphalt is not clear of the run-off, and a
    // row reaching into the run-off at a corner's entry stood a media
    // centre over Eau Rouge's apron and its corner board. The rest of the
    // row stays.
    let total = path.total_length_m();
    let on_runoff = |front: (f32, f32), i: usize| -> bool {
        let (ux, uy) = unit_at(front, i);
        (0..=FOOTPRINT_PROBES).any(|a| {
            let along = (a as f32 / FOOTPRINT_PROBES as f32 - 0.5) * unit_m;
            (0..=FOOTPRINT_PROBES).any(|b| {
                let back = b as f32 / FOOTPRINT_PROBES as f32 * kit_depth;
                let x = ux + cos * along + away.0 * back;
                let y = uy + sin * along + away.1 * back;
                let (sample, lat) = nearest_cross_section(path, x, y);
                let side = if lat >= 0.0 { Side::Left } else { Side::Right };
                lat.abs() - side_half_width(&sample, side)
                    < runoff_reach(surfaces, total, sample.station_m, side)
            })
        })
    };
    let clear_units = |front: (f32, f32)| -> Vec<usize> {
        row.iter()
            .copied()
            .filter(|&i| !on_runoff(front, i))
            .collect()
    };
    let mut kept = clear_units((front_x, front_y));
    // A row the apron takes whole is a big building beside a corner, not a
    // row reaching into one: step it back off the apron rather than lose
    // it (the BrandsScratch MSV Centre, Spa's Salons de l'Eau Rouge).
    let mut stepped = 0.0f32;
    while kept.is_empty() {
        stepped += 1.0;
        if stepped > BUILDING_RUNOFF_MAX_STEP_M {
            return Err("stands on the run-off".to_string());
        }
        front_x += away.0;
        front_y += away.1;
        if footprint_gap((front_x, front_y), &row) >= BUILDING_ROAD_CLEAR_M - 0.05 {
            kept = clear_units((front_x, front_y));
        }
    }

    let mut out = Vec::new();
    for i in kept {
        let (x, y) = unit_at((front_x, front_y), i);
        let (sample, lat) = nearest_cross_section(path, x, y);
        out.push(Prop {
            id: 0,
            kind: PropKind::Building,
            asset: asset.to_string(),
            x,
            y,
            z: seat_z(terrain, &sample, lat, x, y),
            yaw_rad: structure.yaw_rad,
            scale: 1.0,
            text: None,
            length_m: None,
            source: None,
        });
    }
    Ok(out)
}

// ---- Bridges and landmarks ------------------------------------------------

/// Something that crosses the road, on the road's centre at its station.
/// The pivot is the road centre and the Unreal side scales the span to the
/// road width, so nothing here needs to know how wide the bridge is.
///
/// Most things cross *over* the road (a footbridge, the tyre arch, a
/// viaduct, a sign gantry, a timing gantry, a concrete overpass). A
/// `deck_*` crossing is the other way round: the road runs *on* the bridge
/// (over water, or over another road for `deck_track`), and the dossier gives the
/// stations it spans (`from_m`..`to_m`). The deck is tiled along them,
/// module after module, each at the road's height with its own heading, so
/// a long bridge curves with the road. The bake leaves ground out of the
/// span (see [`lay_water_and_bridges`]).
fn lay_crossing(path: &CenterlinePath, crossing: &Crossing) -> Vec<Prop> {
    let total = path.total_length_m();
    let one = |asset: &str, station: f32| {
        let sample = path.sample_at(station);
        Prop {
            id: 0,
            kind: PropKind::Bridge,
            asset: asset.to_string(),
            x: sample.pos.0,
            y: sample.pos.1,
            z: sample.pos.2,
            yaw_rad: sample.heading_rad,
            scale: 1.0,
            text: brand_text(crossing.brand.as_ref()),
            length_m: None,
            source: None,
        }
    };
    let deck = match crossing.kind.as_str() {
        "deck_arch" => Some("bridge_arch_steel"),
        "deck_wide" => Some("bridge_deck_wide"),
        // The lap carried over a road on a bridge of its own (the B 257 at
        // Quiddelbach and Breidscheid): parapets and fascias over the span.
        // The ground under it is not cut away, so it is no `.ats` bridge
        // span (see `lay_water_and_bridges`).
        "deck_track" => Some("track_bridge_10m"),
        _ => None,
    };
    if let (Some(asset), Some(from), Some(to)) = (deck, crossing.from_m, crossing.to_m) {
        let span = (to - from).rem_euclid(total);
        let len = props::find(PropKind::Bridge, asset).map_or(30.0, |k| k.length_m);
        let n = (span / len).round().max(1.0) as usize;
        // Modules end to end, centred on the span.
        let first = from + span / 2.0 - len * (n as f32 - 1.0) / 2.0;
        return (0..n)
            .map(|i| one(asset, (first + i as f32 * len).rem_euclid(total)))
            .collect();
    }
    let asset = match crossing.kind.as_str() {
        "footbridge" => "truss_bridge",
        // The covered walkway over a city street.
        "link" => "linkbridge_covered",
        // An elevated expressway over the road.
        "viaduct" => "viaduct_deck",
        "gantry" => "sign_gantry",
        // The gantry a lap is timed to (the Nordschleife's on the
        // Döttinger Höhe).
        "timing" => "timing_gantry",
        // A lane or forest track's concrete bridge over the road, and any
        // public road crossing over the lap (R4): OSM's `road` crossings
        // are bridges, not a sponsor's tyre arch.
        "overpass" | "road" => "overpass_concrete",
        // A building that spans the road (R5): OSM maps it as a building
        // whose footprint reaches across, which the building pass skips.
        "building" => "span_building",
        // Viewing platforms cantilevered over the straight from both sides.
        "sky_platform" => "sky_platform",
        // The old banked oval on piers, crossing over the road.
        "banked_oval" => "banked_oval_span",
        _ => "tyre_bridge",
    };
    vec![one(asset, crossing.station_m)]
}

/// How far above the ground an asset stands when the dossier places it: a
/// balloon and the wordmark ride the pit building's roof deck, a ribbon sits
/// on top of the Tecpro. Grooming re-seats a dressed prop on the ground, so
/// it adds this back (`groom`), which is why it is by asset.
pub fn fixed_lift_m(asset: &str) -> f32 {
    match asset {
        "lamp_balloon_tether" | "lamp_balloon_tether_orange" | "roof_wordmark_block" => 22.0,
        "led_ribbon_3m" => 1.15,
        _ => 0.0,
    }
}

/// Expand the dossier's furniture runs into props.
fn lay_furniture(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    furniture: &[crate::layout::Furniture],
) -> Vec<Prop> {
    let total = path.total_length_m();
    let mut out = Vec::new();
    for f in furniture {
        let Ok(kind) = PropKind::parse(&f.kind) else {
            continue;
        };
        let every = f.every_m.filter(|e| *e > 0.1);
        let span = f.to_m.map_or(0.0, |to| (to - f.from_m).rem_euclid(total));
        let count = every.map_or(1, |e| (span / e).floor() as usize + 1);
        for i in 0..count {
            let station = (f.from_m + i as f32 * every.unwrap_or(0.0)).rem_euclid(total);
            let sample = path.sample_at(station);
            let lat = match f.side.as_str() {
                "left" => side_half_width(&sample, Side::Left) + f.offset_m,
                "right" => -(side_half_width(&sample, Side::Right) + f.offset_m),
                _ => f.offset_m,
            };
            let (x, y, road_z) = offset_point(&sample, lat);
            if terrain.in_water(x, y) && kind != PropKind::Bridge {
                continue;
            }
            // A bridge spans the road at the road's height; everything
            // else stands on the ground.
            let z = if kind == PropKind::Bridge {
                road_z
            } else {
                seat_z(terrain, &sample, lat, x, y) + fixed_lift_m(&f.asset)
            };
            out.push(Prop {
                id: 0,
                kind,
                asset: f.asset.clone(),
                x,
                y,
                z,
                yaw_rad: sample.heading_rad + f.yaw_deg.to_radians(),
                scale: 1.0,
                text: f.text.clone(),
                length_m: None,
                source: None,
            });
        }
    }
    out
}

/// Distance between the street lamps of a city circuit, metres, and how
/// far behind the road edge their poles stand. They alternate sides, so
/// each side has one every twice the spacing and the arms reach across.
const STREET_LAMP_SPACING_M: f32 = 40.0;
const STREET_LAMP_SETBACK_M: f32 = 4.5;
const STREET_LAMP_BRIDGE_SETBACK_M: f32 = 2.0;

/// Twin-arm street lamps along both verges, alternating sides. On a bridge
/// span the pole stands at road height on the deck's footway,
/// `STREET_LAMP_BRIDGE_SETBACK_M` past the road edge instead of
/// `STREET_LAMP_SETBACK_M`; off a bridge none is laid in the water.
fn lay_street_lamps(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    bridges: &[crate::ats::BridgeSpan],
) -> Vec<Prop> {
    let total = path.total_length_m();
    let count = (total / STREET_LAMP_SPACING_M).floor() as usize;
    let mut out = Vec::new();
    for i in 0..count {
        let station = i as f32 * STREET_LAMP_SPACING_M;
        // On a bridge the pole stands on the footway, just inside the
        // parapet (the deck's own lamps carry no light source), and the
        // water under it is no reason to leave it out.
        let on_bridge = bridges.iter().any(|b| b.covers(station, total));
        let sample = path.sample_at(station);
        let (side, sign) = if i % 2 == 0 {
            (Side::Left, 1.0)
        } else {
            (Side::Right, -1.0)
        };
        let setback = if on_bridge {
            STREET_LAMP_BRIDGE_SETBACK_M
        } else {
            STREET_LAMP_SETBACK_M
        };
        let lat = sign * (side_half_width(&sample, side) + setback);
        let (x, y, road_z) = offset_point(&sample, lat);
        if !on_bridge && terrain.in_water(x, y) {
            continue;
        }
        out.push(Prop {
            id: 0,
            kind: PropKind::Light,
            asset: "lamp_arm_twin".to_string(),
            x,
            y,
            z: if on_bridge {
                road_z
            } else {
                seat_z(terrain, &sample, lat, x, y)
            },
            yaw_rad: sample.heading_rad,
            scale: 1.0,
            text: None,
            length_m: None,
            source: None,
        });
    }
    out
}

/// The scene's water and bridge spans, from the dossier. Ids are reused
/// lowest first, so re-dressing an unchanged circuit rewrites the same
/// file. A circuit with neither leaves both lists empty.
fn lay_water_and_bridges(scene: &mut AtsScene, layout: &Layout) {
    let mut ids: Vec<u64> = scene
        .water
        .iter()
        .map(|w| w.id)
        .chain(scene.bridges.iter().map(|b| b.id))
        .collect();
    ids.sort_unstable();
    ids.reverse();
    scene.water.clear();
    scene.bridges.clear();
    let mut take = |scene: &mut AtsScene| ids.pop().unwrap_or_else(|| scene.alloc_id());
    for body in &layout.water {
        let id = take(scene);
        scene.water.push(crate::ats::Water {
            id,
            name: body.name.clone(),
            rings: body.rings.clone(),
        });
    }
    for crossing in &layout.crossings {
        if !matches!(crossing.kind.as_str(), "deck_arch" | "deck_wide") {
            continue;
        }
        if let (Some(from), Some(to)) = (crossing.from_m, crossing.to_m) {
            let id = take(scene);
            scene.bridges.push(crate::ats::BridgeSpan {
                id,
                start_m: from,
                end_m: to,
            });
        }
    }
}

/// How far along a road passing under the lap it is laid either side of
/// the bridge, metres.
const UNDER_ROAD_REACH_M: f32 = 200.0;
/// Spacing of an under-road's nodes, metres.
const UNDER_ROAD_NODE_M: f32 = 5.0;
/// Endpoints of two dossier ways this close are one junction, metres.
const WAY_JOIN_M: f32 = 0.5;
/// Clear of the lap's edge a road beneath is cut short of, away from its
/// own bridge, metres: past the lap's verge blend, so a road in a cutting
/// never lowers the ground beside another leg of the lap.
const UNDER_ROAD_CLEAR_M: f32 = crate::terrain::BLEND_END_M;

/// Width of a dossier road by its kind, metres.
fn road_width_m(kind: &str) -> f32 {
    match kind {
        "major" => 7.0,
        "minor" => 6.0,
        "service" => 4.5,
        "track" => 3.5,
        _ => 2.5,
    }
}

/// Preference among the dossier's roads crossing under one bridge: the
/// public road first.
fn road_rank(kind: &str) -> u8 {
    match kind {
        "major" => 0,
        "minor" => 1,
        "service" => 2,
        "track" => 3,
        _ => 4,
    }
}

fn polyline_length(line: &[[f32; 2]]) -> f32 {
    line.windows(2)
        .map(|w| (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]))
        .sum()
}

/// The public roads under the lap's own bridges (`deck_track`), from the
/// dossier: for each bridge, the road crossing the lap inside its span
/// (the most major, then the nearest its station), its OSM ways joined
/// end to end for [`UNDER_ROAD_REACH_M`] either way and cut short of
/// where it comes back near the lap, resampled every
/// [`UNDER_ROAD_NODE_M`], with heights from
/// [`TerrainHeightfield::under_road_heights`]. `(name, nodes, width)` per
/// road.
fn lay_under_roads(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    layout: &Layout,
) -> Vec<(Option<String>, Vec<[f32; 3]>, f32)> {
    let total = path.total_length_m();
    let samples = path.samples();
    let mut out = Vec::new();
    for crossing in &layout.crossings {
        if crossing.kind != "deck_track" {
            continue;
        }
        let (Some(from), Some(to)) = (crossing.from_m, crossing.to_m) else {
            continue;
        };
        let span = (to - from).rem_euclid(total);
        let in_span = |station: f32| (station - from).rem_euclid(total) <= span;
        // (rank, gap to the bridge's station, road, segment, along it, lap station)
        let mut best: Option<(u8, f32, usize, usize, f32, f32)> = None;
        for (ri, road) in layout.roads.iter().enumerate() {
            for (j, seg) in road.line.windows(2).enumerate() {
                for i in 0..samples.len() {
                    let a = samples[i];
                    if !in_span(a.station_m) {
                        continue;
                    }
                    let b = samples[(i + 1) % samples.len()];
                    let Some((t, u)) = segments_cross(
                        (a.pos.0, a.pos.1),
                        (b.pos.0, b.pos.1),
                        (seg[0][0], seg[0][1]),
                        (seg[1][0], seg[1][1]),
                    ) else {
                        continue;
                    };
                    let b_station = if i + 1 < samples.len() {
                        b.station_m
                    } else {
                        total
                    };
                    let station = a.station_m + (b_station - a.station_m) * t;
                    let gap = (station - crossing.station_m).rem_euclid(total);
                    let gap = gap.min(total - gap);
                    let key = (road_rank(&road.kind), gap, ri, j, u, station);
                    if best.is_none_or(|b| (key.0, key.1) < (b.0, b.1)) {
                        best = Some(key);
                    }
                }
            }
        }
        let Some((_, _, ri, j, u, station)) = best else {
            continue;
        };
        let road = &layout.roads[ri];
        let width_m = road_width_m(&road.kind);
        let (line, at) = joined_way(layout, ri, j, u);
        let (points, at) = resample_around(&line, at, UNDER_ROAD_REACH_M, UNDER_ROAD_NODE_M);
        let Some((points, at)) = short_of_the_lap(terrain, &points, at, station, span, width_m)
        else {
            continue;
        };
        // The lowest the lap gets over the bridge.
        let mut over_z = f32::INFINITY;
        let mut s = 0.0f32;
        while s <= span {
            over_z = over_z.min(path.sample_at((from + s).rem_euclid(total)).pos.2);
            s += 1.0;
        }
        let z = terrain.under_road_heights(&points, at, over_z);
        let nodes = points.iter().zip(z).map(|(p, z)| [p.0, p.1, z]).collect();
        let name = crossing
            .name
            .as_deref()
            .map(|n| n.trim_end_matches(" bridge").to_string())
            .or_else(|| road.name.clone());
        out.push((name, nodes, width_m));
    }
    out
}

/// Parameters `(t, u)` along `a-b` and `c-d` where the two cross.
fn segments_cross(
    a: (f32, f32),
    b: (f32, f32),
    c: (f32, f32),
    d: (f32, f32),
) -> Option<(f32, f32)> {
    let r = (b.0 - a.0, b.1 - a.1);
    let q = (d.0 - c.0, d.1 - c.1);
    let den = r.0 * q.1 - r.1 * q.0;
    if den.abs() < 1e-9 {
        return None;
    }
    let t = ((c.0 - a.0) * q.1 - (c.1 - a.1) * q.0) / den;
    let u = ((c.0 - a.0) * r.1 - (c.1 - a.1) * r.0) / den;
    ((0.0..=1.0).contains(&t) && (0.0..=1.0).contains(&u)).then_some((t, u))
}

/// Way `ri` of the dossier's roads joined end to end with the ways that
/// continue it (at each end the one turning least, the same kind first,
/// never a turn past a right angle) until it reaches
/// [`UNDER_ROAD_REACH_M`] past the point `u` along its segment `j`.
/// Returns the polyline and how far along it that point is.
fn joined_way(layout: &Layout, ri: usize, j: usize, u: f32) -> (Vec<[f32; 2]>, f32) {
    use std::f32::consts::{FRAC_PI_2, PI, TAU};
    let kind = layout.roads[ri].kind.as_str();
    let mut used = vec![ri];
    let mut line = layout.roads[ri].line.clone();
    let mut at = polyline_length(&line[..=j]) + u * polyline_length(&line[j..=j + 1]);
    for forward in [true, false] {
        loop {
            let reach = if forward {
                polyline_length(&line) - at
            } else {
                at
            };
            if reach >= UNDER_ROAD_REACH_M {
                break;
            }
            let n = line.len();
            let (end, inner) = if forward {
                (line[n - 1], line[n - 2])
            } else {
                (line[0], line[1])
            };
            let heading = (end[1] - inner[1]).atan2(end[0] - inner[0]);
            // (another kind, turn, way, reversed)
            let mut next: Option<(bool, f32, usize, bool)> = None;
            for (k, other) in layout.roads.iter().enumerate() {
                if used.contains(&k) || other.line.len() < 2 {
                    continue;
                }
                let m = other.line.len();
                for reversed in [false, true] {
                    let (first, second) = if reversed {
                        (other.line[m - 1], other.line[m - 2])
                    } else {
                        (other.line[0], other.line[1])
                    };
                    if (first[0] - end[0]).hypot(first[1] - end[1]) > WAY_JOIN_M {
                        continue;
                    }
                    let onward = (second[1] - first[1]).atan2(second[0] - first[0]);
                    let turn = ((onward - heading + PI).rem_euclid(TAU) - PI).abs();
                    let key = (other.kind != kind, turn, k, reversed);
                    if next.is_none_or(|b| (key.0, key.1) < (b.0, b.1)) {
                        next = Some(key);
                    }
                }
            }
            let Some((_, turn, k, reversed)) = next else {
                break;
            };
            if turn > FRAC_PI_2 {
                break;
            }
            used.push(k);
            // `more` runs on from the joined end.
            let mut more = layout.roads[k].line.clone();
            if reversed {
                more.reverse();
            }
            if forward {
                line.extend(more.into_iter().skip(1));
            } else {
                at += polyline_length(&more);
                more.reverse();
                more.pop();
                more.extend(line);
                line = more;
            }
        }
    }
    (line, at)
}

/// Points every `step` (or a little less) along `line` from `reach` before
/// the point `at` along it to `reach` after, as far as the line goes, and
/// how far along them `at` is.
fn resample_around(line: &[[f32; 2]], at: f32, reach: f32, step: f32) -> (Vec<(f32, f32)>, f32) {
    let total = polyline_length(line);
    let start = (at - reach).max(0.0);
    let end = (at + reach).min(total);
    let point_at = |d: f32| {
        let mut left = d;
        for w in line.windows(2) {
            let len = (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]);
            if left <= len {
                let t = if len > 0.0 { left / len } else { 0.0 };
                return (
                    w[0][0] + (w[1][0] - w[0][0]) * t,
                    w[0][1] + (w[1][1] - w[0][1]) * t,
                );
            }
            left -= len;
        }
        let last = line[line.len() - 1];
        (last[0], last[1])
    };
    let count = (((end - start) / step).ceil() as usize).max(1);
    let points = (0..=count)
        .map(|i| point_at(start + (end - start) * i as f32 / count as f32))
        .collect();
    (points, at - start)
}

/// `points`, which pass under the lap `at` along them (at lap `station`),
/// cut short of where they come back within [`UNDER_ROAD_CLEAR_M`] of the
/// lap's edge away from the bridge: a junction with it, or another leg.
/// `None` when too little is left either side of the bridge.
fn short_of_the_lap(
    terrain: &TerrainHeightfield,
    points: &[(f32, f32)],
    at: f32,
    station: f32,
    span: f32,
    width_m: f32,
) -> Option<(Vec<(f32, f32)>, f32)> {
    let total = terrain.road_path(0).total_length_m();
    let near_lap = |p: (f32, f32)| {
        terrain
            .nearest_track_point(p.0, p.1, 80.0)
            .is_some_and(|(s, lat, half)| {
                let gap = (s - station).rem_euclid(total);
                let gap = gap.min(total - gap);
                gap > span / 2.0 + 40.0 && lat.abs() - half < width_m / 2.0 + UNDER_ROAD_CLEAR_M
            })
    };
    let mut along = vec![0.0f32; points.len()];
    for i in 1..points.len() {
        let (p, q) = (points[i], points[i - 1]);
        along[i] = along[i - 1] + (p.0 - q.0).hypot(p.1 - q.1);
    }
    let cross = along.iter().rposition(|&a| a <= at)?;
    let lo = (0..=cross)
        .rev()
        .find(|&i| near_lap(points[i]))
        .map_or(0, |i| i + 1);
    let hi = (cross + 1..points.len())
        .find(|&i| near_lap(points[i]))
        .map_or(points.len() - 1, |i| i - 1);
    // At least 20 m either side of the crossing.
    if lo > cross || hi <= cross || at - along[lo] < 20.0 || along[hi] - at < 20.0 {
        return None;
    }
    Some((points[lo..=hi].to_vec(), at - along[lo]))
}

/// The brands the kit has artwork for (`props/board/brands`).
/// Signage in the kit is fictionalised, so a dossier naming a real
/// sponsor gets nothing rather than a texture that does not exist.
pub(crate) const KIT_BRANDS: [&str; 8] = [
    "apexsim",
    "brix",
    "hexon",
    "kronos",
    "northwind",
    "piretti",
    "rolux",
    "velocet",
];

fn brand_text(brand: Option<&String>) -> Option<String> {
    let brand = brand?.to_ascii_lowercase();
    KIT_BRANDS.contains(&brand.as_str()).then_some(brand)
}

/// Laid on its mapped centre, unless the kit's footprint there would
/// reach onto the road: then stepped off it toward the dossier's side.
/// Monza's Torre Sud is an OSM node 0.5 m off the centerline at the line,
/// and a 13 m tower stood across the start straight, a wall the AI
/// crashed into every few laps. `Ok(None)` for a kind the kit has no
/// prop for, `Err` when there is no room beside the road.
fn lay_landmark(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    landmark: &Landmark,
) -> Result<Option<Prop>, String> {
    let (kind, asset) = match landmark.kind.as_str() {
        "big_wheel" => (PropKind::Attraction, "ferris_wheel"),
        // Marina Bay: the 150 m observation wheel, the three-tower hotel
        // with its sky deck, the lotus-shaped museum and the double helix.
        "big_wheel_xl" => (PropKind::Attraction, "landmark_big_wheel_xl"),
        "skypark" => (PropKind::Attraction, "landmark_three_towers_skypark"),
        "lotus_museum" => (PropKind::Attraction, "landmark_lotus_museum"),
        "double_helix" => (PropKind::Attraction, "bridge_double_helix"),
        "screen" => (PropKind::Attraction, "video_screen"),
        "stage" => (PropKind::Attraction, "fanzone_stage"),
        "camera_tower" => (PropKind::Attraction, "camera_tower"),
        "tower" => (PropKind::Building, "control_tower"),
        // A tower that is not race control (R11): a viewing tower like
        // Austin's, or Bahrain's round glass control tower.
        "observation_tower" => (PropKind::Building, "observation_tower"),
        "glass_tower" => (PropKind::Building, "round_glass_tower"),
        // Venue buildings the kit models (kit gaps 3, 10-12): an
        // elevated wing-roofed media building (road-facing), an ice arena,
        // the Expo pavilion, the geodesic dome, a roadside fuel station
        // (road-facing, its fascia branded) and the Olympic cauldron.
        "media_wing" => (PropKind::Building, "wing_media_centre"),
        "arena" => (PropKind::Building, "arena_dome"),
        "stadium" => (PropKind::Building, "stadium_bowl"),
        "veil_tower" => (PropKind::Building, "veil_tower"),
        "expo_pavilion" => (PropKind::Building, "expo_pavilion"),
        "geodesic_dome" => (PropKind::Building, "geodesic_dome"),
        "fuel_station" => (PropKind::Building, "fuel_station_canopy"),
        "cauldron" => (PropKind::Misc, "cauldron"),
        // Burg Nürburg: a castle on its hill, centred on its footprint.
        "castle" => (PropKind::Building, "castle_ruin"),
        // The Hohe Acht's stone lookout tower, centred on its footprint.
        "lookout_tower" => (PropKind::Building, "stone_lookout_tower"),
        // Standing letters on a slope, spelling the circuit's display name.
        "hillside_letters" => (PropKind::Sign, "hillside_letters"),
        // One lane of a toll gate (the Touristenfahrten entrance).
        "toll_gate" => (PropKind::Misc, "toll_gate_lane"),
        "statue" => (PropKind::Misc, "bull_statue"),
        "floodlight" => (PropKind::Light, "floodlight_tower"),
        "blimp" => (PropKind::Sky, "blimp"),
        "balloon" => (PropKind::Sky, "balloon"),
        "helicopter" => (PropKind::Sky, "helicopter"),
        _ => return Ok(None),
    };
    let (mut x, mut y) = (landmark.centre[0], landmark.centre[1]);
    let (sample, _) = nearest_cross_section(path, x, y);
    let yaw = landmark.yaw_rad.unwrap_or(sample.heading_rad);
    if kind != PropKind::Sky {
        if let Some(kit) = props::find(kind, asset) {
            (x, y) = clear_landmark(path, x, y, yaw, kit, landmark.side)
                .ok_or_else(|| "no room beside the road".to_string())?;
        }
    }
    let (sample, lat) = nearest_cross_section(path, x, y);
    // Sky props fly: their z is an altitude over the road, not a seat.
    let z = match kind {
        PropKind::Sky => {
            offset_point(&sample, lat).2 + landmark.altitude_m.unwrap_or(SKY_ALTITUDE_M)
        }
        _ => seat_z(terrain, &sample, lat, x, y),
    };
    // Face the road: the builder flips by side, so the course heading is
    // enough for anything with a front.
    Ok(Some(Prop {
        id: 0,
        kind,
        asset: asset.to_string(),
        x,
        y,
        z,
        yaw_rad: yaw,
        scale: 1.0,
        text: brand_text(landmark.brand.as_ref()),
        length_m: None,
        source: None,
    }))
}

/// Least distance from the road edge (any section of the course) over a
/// landmark's kit footprint pivoted at `(x, y)`: centred on the pivot for
/// a centred asset, else reaching `depth` back from it on `side`.
/// Negative is on the asphalt.
fn landmark_road_gap(
    path: &CenterlinePath,
    x: f32,
    y: f32,
    yaw: f32,
    kit: &props::KitAsset,
    away: (f32, f32),
) -> f32 {
    let (sin, cos) = yaw.sin_cos();
    // The footprint's depth axis, pointing away from the road.
    let depth_axis = if -sin * away.0 + cos * away.1 >= 0.0 {
        (-sin, cos)
    } else {
        (sin, -cos)
    };
    let (back0, back1) = if crate::ue_export::footprint_is_centred(kit.kind, kit.asset) {
        (-kit.depth_m / 2.0, kit.depth_m / 2.0)
    } else {
        (0.0, kit.depth_m)
    };
    let mut worst = f32::MAX;
    for a in 0..=FOOTPRINT_PROBES {
        let along = (a as f32 / FOOTPRINT_PROBES as f32 - 0.5) * kit.length_m;
        for b in 0..=FOOTPRINT_PROBES {
            let back = back0 + b as f32 / FOOTPRINT_PROBES as f32 * (back1 - back0);
            let px = x + cos * along + depth_axis.0 * back;
            let py = y + sin * along + depth_axis.1 * back;
            let (sample, lat) = nearest_cross_section(path, px, py);
            let side = if lat >= 0.0 { Side::Left } else { Side::Right };
            worst = worst.min(lat.abs() - side_half_width(&sample, side));
        }
    }
    worst
}

/// Where a landmark's footprint stands [`BUILDING_ROAD_CLEAR_M`] clear of
/// the road: its mapped spot when it already does, else stepped across
/// the road's normal toward the dossier's `side` (which still says where
/// it belongs when the mapped point is on the asphalt) in 1 m steps, up
/// to [`BUILDING_MAX_PUSH_M`] past the road edge.
fn clear_landmark(
    path: &CenterlinePath,
    x: f32,
    y: f32,
    yaw: f32,
    kit: &props::KitAsset,
    side: Side,
) -> Option<(f32, f32)> {
    let (sample, lat) = nearest_cross_section(path, x, y);
    let (sin, cos) = sample.heading_rad.sin_cos();
    let away = match side {
        Side::Left => (-sin, cos),
        Side::Right => (sin, -cos),
    };
    let (mut x, mut y) = (x, y);
    let limit = lat.abs() + side_half_width(&sample, side) + BUILDING_MAX_PUSH_M;
    let mut pushed = 0.0f32;
    loop {
        if landmark_road_gap(path, x, y, yaw, kit, away) >= BUILDING_ROAD_CLEAR_M - 0.05 {
            return Some((x, y));
        }
        pushed += 1.0;
        if pushed > limit {
            return None;
        }
        x += away.0;
        y += away.1;
    }
}

// ---- Shared helpers -------------------------------------------------------

/// Height over the road a sky prop flies at when the dossier does not say.
const SKY_ALTITUDE_M: f32 = 180.0;

fn round2(v: f32) -> f32 {
    (v * 100.0).round() / 100.0
}

/// How far past the asphalt the paved run-off reaches at `station_m` on
/// `side`, m: the outer edge of the widest asphalt or concrete apron there
/// (0 with none), a wedge's width taken where the station falls along it.
/// Gravel and sand are left out: their wedges are drawn out to the
/// barriers and beyond, and buildings behind them are real. `total_m` is the lap, for a strip that wraps
/// through the line.
fn runoff_reach(surfaces: &[Surface], total_m: f32, station_m: f32, side: Side) -> f32 {
    surfaces
        .iter()
        .filter(|s| {
            s.side == side && matches!(s.kind, SurfaceKind::AsphaltRunoff | SurfaceKind::Concrete)
        })
        .filter_map(|s| {
            let span = if s.end_m >= s.start_m {
                s.end_m - s.start_m
            } else {
                s.end_m + total_m - s.start_m
            };
            let into = (station_m - s.start_m).rem_euclid(total_m.max(1.0));
            (span > 0.0 && into <= span).then(|| s.inner_m + s.width_at(into / span))
        })
        .fold(0.0, f32::max)
}

fn side_half_width(sample: &PathSample, side: Side) -> f32 {
    match side {
        Side::Left => sample.width_left_m,
        Side::Right => sample.width_right_m,
    }
}

fn seat_z(terrain: &TerrainHeightfield, sample: &PathSample, lat: f32, x: f32, y: f32) -> f32 {
    let road = offset_point(sample, lat);
    surface_height(Some(terrain), sample, lat, (x, y, road.2))
}

/// Nearest cross-section of the course to a point: the sample and the
/// signed lateral offset (positive left).
fn nearest_cross_section(path: &CenterlinePath, x: f32, y: f32) -> (PathSample, f32) {
    let mut best = (f32::MAX, path.samples()[0], 0.0f32);
    for sample in path.samples() {
        let dx = x - sample.pos.0;
        let dy = y - sample.pos.1;
        let d2 = dx * dx + dy * dy;
        if d2 < best.0 {
            let (sin, cos) = sample.heading_rad.sin_cos();
            best = (d2, *sample, -sin * dx + cos * dy);
        }
    }
    (best.1, best.2)
}

/// How far along the course from where the lane's progress says it is the
/// nearest cross-section may be looked for, metres either way.
const PIT_LEG_WINDOW_M: f32 = 80.0;
/// A lane point the window finds further from its leg than this, metres,
/// is looked for again where the lane's progress puts it.
const PIT_LEG_LOST_M: f32 = 60.0;

/// [`nearest_cross_section`] restricted to the samples within `window`
/// of station `around` (wrapping on a loop), so a lane running beside one
/// leg of a folded circuit is not seated on another.
fn nearest_cross_section_near(
    path: &CenterlinePath,
    x: f32,
    y: f32,
    around: f32,
    window: f32,
) -> (PathSample, f32) {
    let total = path.total_length_m();
    let gap = |s: f32| {
        let d = (s - around).abs();
        if path.is_closed() {
            d.min(total - d)
        } else {
            d
        }
    };
    let mut best: Option<(f32, PathSample, f32)> = None;
    for sample in path.samples() {
        if gap(sample.station_m) > window {
            continue;
        }
        let dx = x - sample.pos.0;
        let dy = y - sample.pos.1;
        let d2 = dx * dx + dy * dy;
        if best.is_none_or(|b| d2 < b.0) {
            let (sin, cos) = sample.heading_rad.sin_cos();
            best = Some((d2, *sample, -sin * dx + cos * dy));
        }
    }
    match best {
        Some((_, sample, lat)) => (sample, lat),
        None => nearest_cross_section(path, x, y),
    }
}

/// Planar distance from a point to the pit lane's centerline.
fn lane_gap(lane: &CenterlinePath, x: f32, y: f32) -> f32 {
    lane.samples()
        .iter()
        .map(|s| (x - s.pos.0).hypot(y - s.pos.1))
        .fold(f32::MAX, f32::min)
}

/// The kit footprint a dressed prop will occupy, for callers that need to
/// keep clear of it.
pub fn dressed_footprint_radius_m(prop: &Prop) -> f32 {
    props::resolve(prop.kind, &prop.asset)
        .map(|a| a.footprint_radius_m())
        .unwrap_or(3.0)
}

// ---- The surroundings -----------------------------------------------------

/// Everything beside the circuit that is not a stand, a building, a bridge
/// or a landmark: the car parks and the cars in them, the camp sites, the
/// village, the marshal posts, the floodlights, the fan zone.
///
/// The kit has had all of this for months and no rule ever placed any of
/// it. A circuit came out as a road, a ring of barriers, some stands and a
/// belt of trees, which is why the Red Bull Ring reads as "really basic
/// around the track": there was nothing there because nothing put anything
/// there. What changed is that the dossier now carries the layers to place
/// it from — `areas`, `poi`, `roads` and `corners` — rather than the
/// scatter the old procedural enrichment invented.
///
/// Everything here is placed from a fact in the dossier and is laid on a
/// deterministic grid or a hash of its own position, never a random
/// number, so a second run writes a byte-identical file.
mod surroundings {
    use std::f32::consts::{PI, TAU};

    use crate::track_path::curvature_at;

    use super::*;

    /// A camp site's tents and vans, per 100 m of its perimeter.
    const CAMP_PITCHES_PER_100M: f32 = 6.0;
    /// Cars fill a car park at one per this much area.
    const CAR_PARK_AREA_PER_CAR_M2: f32 = 90.0;
    /// No car park gets more than this, however big it is: a field with
    /// two thousand instanced hatchbacks in it costs more than it adds.
    const MAX_CARS_PER_PARK: usize = 120;
    /// Lamp posts around a car park's edge.
    const LAMP_SPACING_M: f32 = 30.0;
    /// No car-park lamp this close to the road edge, metres: there the
    /// park's edge is the trackside fence, and a row of lamps along it
    /// reads as track lighting at a circuit that has none.
    const LAMP_ROAD_CLEAR_M: f32 = 25.0;
    /// A marshal post stands at every named corner, and on a straight
    /// this far apart: a real circuit has a post every 100-200 m (51 at
    /// the GP Nurburgring, 40 at Sakhir, 35 at Shanghai), three times what
    /// 400 m laid (R10).
    const MARSHAL_SPACING_M: f32 = 150.0;
    /// How far past the barrier line the marshals stand.
    const MARSHAL_BEYOND_BARRIER_M: f32 = 4.0;
    /// The marshal stands this far up the road from the post's hut.
    const MARSHAL_FIGURE_AHEAD_M: f32 = 2.5;
    /// Knots of people round a fan-zone stage, a big wheel, a food stall.
    const STAGE_CROWD_GROUPS: usize = 4;
    const WHEEL_CROWD_GROUPS: usize = 2;
    /// Race control's marshal sectors (the simulation's
    /// `race_control::MARSHAL_SECTOR_M`, which the content cannot read):
    /// every one gets a light panel, so a yellow shown in it is seen.
    const MARSHAL_SECTOR_M: f32 = 250.0;
    /// A light panel stands this far up the course from its post, beside
    /// it rather than inside it.
    const LIGHT_PANEL_UP_COURSE_M: f32 = 3.0;
    /// Floodlight masts along a circuit with none of its own, when a
    /// session after dark would otherwise be lit by headlights alone.
    const FLOODLIGHT_SPACING_M: f32 = 220.0;
    const FLOODLIGHT_BEYOND_BARRIER_M: f32 = 12.0;
    /// Forest impostors are planted on this grid across the far field.
    /// One stands in for a 40 m patch of wood, so the spacing is its own
    /// size and the hills come out continuously wooded rather than dotted.
    const IMPOSTOR_SPACING_M: f32 = 43.0;
    /// The band they cover: from beyond the detailed tree belts out to
    /// the edge of the near elevation grid. Nearer than this the real
    /// trees are planted, further than this nothing is visible anyway.
    const IMPOSTOR_NEAR_M: f32 = 260.0;
    const IMPOSTOR_FAR_M: f32 = 2_600.0;
    /// Ground steeper than this is a wooded slope in every circuit this
    /// applies to. It is a guess where the mapped woodland runs out —
    /// stated as one — because no bbox in the project reaches far enough
    /// to have surveyed the hills, and a bare 400 m slope a kilometre
    /// from the road looks far more wrong than a wooded one.
    const IMPOSTOR_MIN_SLOPE: f32 = 0.16;
    /// Nothing is planted on the flat valley floor around the circuit:
    /// that is farmland, and the areas layer already describes it.
    const IMPOSTOR_MIN_RISE_M: f32 = 25.0;
    /// Inside a mapped wood the far forest starts here, flat ground or not
    /// (R8): past the card-tree belt (which stops at 90-110 m) the woods of
    /// a flat circuit — Le Mans, Silverstone, Suzuka — were bare.
    const WOOD_IMPOSTOR_NEAR_M: f32 = 120.0;
    /// The horizon's tree line: on flat country with no mapped wood the
    /// far field was bare to the vanishing point, where a real one is
    /// closed by shelter belts, hedgerow trees and the woods along the
    /// property line. A broken belt of impostors in this band from the
    /// road, in patches of `HORIZON_PATCH_M` of which `HORIZON_PATCH_SHARE`
    /// are wooded, `HORIZON_FILL` of each patch's cells planted.
    const HORIZON_NEAR_M: f32 = 900.0;
    const HORIZON_FAR_M: f32 = 2_200.0;
    const HORIZON_PATCH_M: f32 = 400.0;
    const HORIZON_PATCH_SHARE: f32 = 0.42;
    const HORIZON_FILL: f32 = 0.7;

    /// A village house needs this much room from its neighbours.
    const HOUSE_SPACING_M: f32 = 26.0;
    /// Scenery is laid no closer than this to the road: the barrier line
    /// is already out there, and a car park or a house inside it would be
    /// on the run-off.
    const ROAD_CLEAR_M: f32 = 18.0;
    /// Trackside furniture — a marshal post, a corner board — belongs
    /// just behind the barrier and is held to this instead. Holding it to
    /// the scenery clearance is what silently dropped every one of them
    /// the first time this pass ran.
    const TRACKSIDE_CLEAR_M: f32 = 5.0;
    /// A village is a hamlet, not a town. Filling each mapped residential
    /// polygon on a 26 m grid put 345 houses around the Red Bull Ring,
    /// which has perhaps fifty within two kilometres, so each area gets a
    /// share of its own size and no more.
    const MAX_HOUSES_PER_AREA: usize = 14;
    const HOUSE_AREA_PER_BUILDING_M2: f32 = 2_400.0;

    /// The assets this pass owns. It deletes and re-lays them on every
    /// run, so it has to recognise its own output; and because it shares
    /// kinds with hand-placed props (a `sign` is also a distance board, a
    /// `misc` is also a bollard) ownership is by asset, not by kind.
    pub const OWNED: [&str; 41] = [
        // The people kit: a marshal at every post, groups at the fan zone.
        "marshal_figure",
        "crowd_group",
        "car_a",
        "car_b",
        "car_c",
        "camper_van",
        "coach",
        "motorhome",
        "tent_6m",
        "food_stall_6m",
        "ticket_gate",
        "marshal_post",
        "marshal_post_numbered",
        "light_panel",
        "corner_sign",
        "power_pylon",
        "lamp_post",
        "floodlight_tower",
        "forest_impostor",
        "forest_impostor_conifer",
        // The village is `building` kind, which the pass owns wholesale in
        // a dressed scene anyway; listing it here is what makes it owned
        // when a scene is groomed *without* its dossier too.
        "village_house_a",
        "village_house_b",
        "village_house_c",
        "barn",
        "chapel",
        "raintree_l",
        "station_entrance",
        "traffic_signal_pole",
        "bus_shelter",
        // The start straight's furniture (`Layout::furniture`): boards,
        // scaffold and signs are not dressed kinds, so they are owned here.
        "roof_wordmark_block",
        "led_ribbon_3m",
        "stair_zigzag_scaffold",
        "finish_tower_scaffold",
        "walkway_planter_4m",
        "pit_entry_board",
        // The track edge's own pieces (`lay_kerb_pieces`).
        "kerb_marker",
        "sausage_kerb_2m",
        // The endurance crowd's (`CircuitStyle::fan_dressing`): the banks
        // behind the rail and the camp villages' pieces.
        "spectator_bank_10m",
        "camp_scaffold_tower",
        "camp_gazebo",
        "camp_flag_pole",
    ];

    /// The marshal posts' number boards go up to this (`post<N>` marker
    /// textures, `build_nordschleife_kit.py`); a post past it has none.
    const MARSHAL_NUMBERS: usize = 120;
    /// The fan dressing's camp village: one piece (a scaffold tower, a
    /// gazebo or a flag pole) per this many tent pitches, each at least
    /// `CAMP_PIECE_CLEAR_M` from every other piece of the camp.
    const CAMP_PITCHES_PER_PIECE: usize = 2;
    const CAMP_PIECE_CLEAR_M: f32 = 4.5;
    /// The flags the crowd brings (`sign/flags`): the Eifel's neighbours.
    pub(super) const FAN_FLAGS: [&str; 6] = ["de", "nl", "be", "ch", "at", "de"];
    /// A car park or camp site any part of which is within this of the
    /// road edge is where a crowd stands: earth banks go behind the rail
    /// along the stretch it borders, on its side.
    const SPECTATOR_AREA_REACH_M: f32 = 60.0;
    /// Words in a car park's OSM name that make it a paddock (`Fahrerlager`),
    /// a depot (`Marshals Lager`, `Industrielager`) or a compound, not a
    /// spectator car park.
    const SPECTATOR_AREA_NOT: [&str; 2] = ["lager", "compound"];
    /// No banks this close to the start line: the start area is the GP
    /// complex (T13 and its grandstand), not a spectator bank.
    const SPECTATOR_START_CLEAR_M: f32 = 300.0;
    /// The banks' centre line past the road edge (the bank is 5 m across,
    /// so its near foot is 8.5 m out, well behind a rail at 3-4.5 m and out
    /// of a trackside camera's line to the car), their pitch, and the
    /// longest stretch one area lines.
    const SPECTATOR_BANK_BEYOND_M: f32 = 11.0;
    const SPECTATOR_BANK_PITCH_M: f32 = 10.5;
    const SPECTATOR_BANK_MAX_RUN_M: f32 = 320.0;

    /// A curb shorter than this gets no marker posts.
    const KERB_MARKER_MIN_CURB_M: f32 = 4.0;
    /// How far past the curb's outer edge its marker post stands, m.
    const KERB_MARKER_BEYOND_M: f32 = 0.3;
    /// Sausage kerbs go behind the inside curb of a bend tighter than this
    /// radius, m, over a curb no longer than `SAUSAGE_MAX_CURB_M` (an apex
    /// or a chicane, not a sweeper), one `sausage_kerb_2m` every
    /// `SAUSAGE_PITCH_M` from a metre in to a metre short.
    const SAUSAGE_MAX_RADIUS_M: f32 = 80.0;
    const SAUSAGE_MIN_CURB_M: f32 = 6.0;
    const SAUSAGE_MAX_CURB_M: f32 = 60.0;
    const SAUSAGE_PITCH_M: f32 = 2.0;
    /// The sausage kerb's near face sits this far past the curb's outer edge, m (the piece is 0.5 m deep).
    const SAUSAGE_BEYOND_M: f32 = 0.35;

    /// Kerb markers and sausage kerbs for the scene's curbs. Both are
    /// owned by this pass (`OWNED`), so a re-dress lays them again rather
    /// than doubling them.
    pub fn lay_kerb_pieces(
        path: &CenterlinePath,
        terrain: &TerrainHeightfield,
        curbs: &[crate::ats::Curb],
    ) -> Vec<Prop> {
        let total = path.total_length_m();
        if total <= 0.0 {
            return Vec::new();
        }
        let mut out = Vec::new();
        let mut place = |kind: PropKind, asset: &str, station: f32, side: Side, beyond: f32| {
            let sample = path.sample_at(station.rem_euclid(total));
            let lat = signed_lat(&sample, side, beyond);
            let (x, y, _) = offset_point(&sample, lat);
            if terrain.in_water(x, y) {
                return;
            }
            out.push(Prop {
                id: 0,
                kind,
                asset: asset.to_string(),
                x: round2(x),
                y: round2(y),
                z: round2(seat_z(terrain, &sample, lat, x, y)),
                yaw_rad: round2(sample.heading_rad),
                scale: 1.0,
                text: None,
                length_m: None,
                source: None,
            });
        };
        for curb in curbs {
            let len = (curb.end_m - curb.start_m).rem_euclid(total);
            if len < KERB_MARKER_MIN_CURB_M {
                continue;
            }
            let beyond = curb.width_m.max(0.0) + KERB_MARKER_BEYOND_M;
            place(
                PropKind::Misc,
                "kerb_marker",
                curb.start_m,
                curb.side,
                beyond,
            );
            place(PropKind::Misc, "kerb_marker", curb.end_m, curb.side, beyond);

            if !(SAUSAGE_MIN_CURB_M..=SAUSAGE_MAX_CURB_M).contains(&len) {
                continue;
            }
            // The bend's inside: a left turn (positive curvature) is on the left.
            let k =
                crate::track_path::curvature_at(path, (curb.start_m + len / 2.0).rem_euclid(total));
            if k.abs() < 1.0 / SAUSAGE_MAX_RADIUS_M {
                continue;
            }
            let inside = if k > 0.0 { Side::Left } else { Side::Right };
            if curb.side != inside {
                continue;
            }
            let beyond = curb.width_m.max(0.0) + SAUSAGE_BEYOND_M + 0.25;
            let mut d = 1.0;
            while d + 1.0 <= len {
                place(
                    PropKind::Barrier,
                    "sausage_kerb_2m",
                    curb.start_m + d,
                    curb.side,
                    beyond,
                );
                d += SAUSAGE_PITCH_M;
            }
        }
        out
    }

    /// Village buildings are `building` kind, which the dressing pass
    /// already owns wholesale, so they need no entry in [`OWNED`].
    const VILLAGE_HOUSES: [&str; 4] = [
        "village_house_a",
        "village_house_b",
        "village_house_c",
        "barn",
    ];

    pub fn owns(prop: &Prop) -> bool {
        OWNED.contains(&prop.asset.as_str())
    }

    /// A deterministic value in 0..1 from a position, so a scatter is the
    /// same on every run without a random number generator anywhere near
    /// the pass.
    pub(super) fn roll(x: f32, y: f32, salt: u32) -> f32 {
        let mut h = (x * 100.0) as i32 as u32;
        h = h.wrapping_mul(2_654_435_761) ^ ((y * 100.0) as i32 as u32);
        h = h.wrapping_mul(2_246_822_519) ^ salt.wrapping_mul(0x9E37_79B9);
        h ^= h >> 15;
        h = h.wrapping_mul(0x85EB_CA6B);
        h ^= h >> 13;
        (h % 10_000) as f32 / 10_000.0
    }

    struct Ctx<'a> {
        path: &'a CenterlinePath,
        terrain: &'a TerrainHeightfield,
        layout: &'a Layout,
        /// A desert or dune circuit: nothing grows on the slopes.
        bare: bool,
        /// The endurance crowd's lap (`CircuitStyle::fan_dressing`).
        fan: bool,
        /// A tree line closes the horizon (`HORIZON_*`): not in a city,
        /// whose horizon is its buildings.
        horizon: bool,
    }

    impl Ctx<'_> {
        /// Seat a prop on the ground at a point, facing the road, unless
        /// it is too close to the course to be scenery.
        fn place(
            &self,
            kind: PropKind,
            asset: &str,
            x: f32,
            y: f32,
            yaw: Option<f32>,
        ) -> Option<Prop> {
            self.place_clear(kind, asset, x, y, yaw, ROAD_CLEAR_M)
        }

        /// The same, for something that belongs closer to the road.
        fn place_clear(
            &self,
            kind: PropKind,
            asset: &str,
            x: f32,
            y: f32,
            yaw: Option<f32>,
            clear_m: f32,
        ) -> Option<Prop> {
            let (sample, lat) = nearest_cross_section(self.path, x, y);
            if lat.abs() < side_half_width(&sample, side_of(lat)) + clear_m {
                return None;
            }
            Some(Prop {
                id: 0,
                kind,
                asset: asset.to_string(),
                x: round2(x),
                y: round2(y),
                z: round2(seat_z(self.terrain, &sample, lat, x, y)),
                yaw_rad: round2(yaw.unwrap_or(sample.heading_rad)),
                scale: 1.0,
                text: None,
                length_m: None,
                source: None,
            })
        }
    }

    fn side_of(lat: f32) -> Side {
        if lat >= 0.0 {
            Side::Left
        } else {
            Side::Right
        }
    }

    /// Lay the lot. `night_lighting` adds floodlight masts to a circuit
    /// that has none of its own.
    pub fn lay(
        path: &CenterlinePath,
        terrain: &TerrainHeightfield,
        layout: &Layout,
        dem: Option<&DemFile>,
        night_lighting: bool,
        bare: bool,
        fan: bool,
        horizon: bool,
    ) -> Vec<Prop> {
        let ctx = Ctx {
            path,
            terrain,
            layout,
            bare,
            fan,
            horizon,
        };
        let mut out = Vec::new();
        // Each sub-layer stamps what it lays (`Prop::source`), an area's
        // props with the area's own name where the dossier gives one, so
        // `--explain` says which dossier entry each came from.
        let part = |name: &str, out: &mut Vec<Prop>, lay: &dyn Fn(&mut Vec<Prop>)| {
            let start = out.len();
            lay(out);
            stamp_from(out, start, &format!("surroundings: {name}"));
        };
        part("car parks", &mut out, &|o| car_parks(&ctx, o));
        part("camp sites", &mut out, &|o| camp_sites(&ctx, o));
        if fan {
            part("spectator banks", &mut out, &|o| spectator_banks(&ctx, o));
        }
        part("village", &mut out, &|o| village(&ctx, o));
        part("points of interest", &mut out, &|o| {
            points_of_interest(&ctx, o)
        });
        part("forest", &mut out, &|o| forests(&ctx, dem, o));
        part("marshal posts", &mut out, &|o| marshal_posts(&ctx, o));
        part("people", &mut out, &|o| people(&ctx, o));
        part("light panels", &mut out, &|o| light_panels(&ctx, o));
        part("corner signs", &mut out, &|o| corner_signs(&ctx, o));
        if night_lighting {
            part("floodlights", &mut out, &|o| floodlights(&ctx, o));
        }
        out
    }

    /// Stamp the props from `start` on that have no stamp yet.
    fn stamp_from(out: &mut [Prop], start: usize, source: &str) {
        for prop in &mut out[start..] {
            if prop.source.is_none() {
                prop.source = Some(source.to_string());
            }
        }
    }

    /// Stamp the props an area laid from `start` with its kind and name,
    /// when the dossier names it.
    fn stamp_area(out: &mut [Prop], start: usize, what: &str, area: &crate::layout::Area) {
        if let Some(name) = area.name.as_deref().filter(|n| !n.trim().is_empty()) {
            stamp_from(out, start, &format!("surroundings: {what} {}", name.trim()));
        }
    }

    /// Cars and lamp posts in every mapped car park.
    fn car_parks(ctx: &Ctx, out: &mut Vec<Prop>) {
        for area in ctx.layout.areas.iter().filter(|a| a.kind == "parking") {
            let start = out.len();
            car_park(ctx, area, out);
            stamp_area(out, start, "car park", area);
        }
    }

    /// One car park's cars and lamps.
    fn car_park(ctx: &Ctx, area: &crate::layout::Area, out: &mut Vec<Prop>) {
        let cars = ((area.area_m2() / CAR_PARK_AREA_PER_CAR_M2) as usize).min(MAX_CARS_PER_PARK);
        if cars == 0 {
            return;
        }
        // A car park is filled on its own grid rather than at random
        // points, so the rows read as rows.
        let (cx, cy) = area.centre();
        let span = area.area_m2().sqrt();
        let step = (span / (cars as f32).sqrt().max(1.0)).max(3.0);
        let per_row = (span / step).max(1.0) as usize;
        let mut placed = 0;
        for i in 0..cars * 3 {
            if placed >= cars {
                break;
            }
            let (row, col) = (i / per_row.max(1), i % per_row.max(1));
            let x = cx - span * 0.5 + col as f32 * step;
            let y = cy - span * 0.5 + row as f32 * step;
            if !area.contains(x, y) {
                continue;
            }
            let asset = match (roll(x, y, 11) * 3.0) as u32 {
                0 => "car_a",
                1 => "car_b",
                _ => "car_c",
            };
            // Parked in rows facing across the park, not at the road.
            let yaw = if roll(x, y, 12) < 0.5 { 0.0 } else { PI };
            if let Some(prop) = ctx.place(PropKind::Vehicle, asset, x, y, Some(yaw)) {
                out.push(prop);
                placed += 1;
            }
        }
        lamps_around(ctx, area, out);
    }

    /// Lamp posts along a car park's edge, at a fixed spacing so the
    /// lighting reads as laid out rather than scattered; none within
    /// [`LAMP_ROAD_CLEAR_M`] of the road, where a car park's edge runs
    /// along the trackside fence and its lamps read as track lighting.
    fn lamps_around(ctx: &Ctx, area: &crate::layout::Area, out: &mut Vec<Prop>) {
        let ring = &area.ring;
        let mut carried = 0.0f32;
        for pair in ring.windows(2) {
            let (a, b) = (pair[0], pair[1]);
            let length = (b[0] - a[0]).hypot(b[1] - a[1]);
            let mut along = LAMP_SPACING_M - carried;
            while along < length {
                let t = along / length;
                let (x, y) = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
                let (sample, lat) = nearest_cross_section(ctx.path, x, y);
                let side = if lat >= 0.0 { Side::Left } else { Side::Right };
                let off_road = lat.abs() - side_half_width(&sample, side);
                if off_road >= LAMP_ROAD_CLEAR_M {
                    if let Some(prop) = ctx.place(PropKind::Light, "lamp_post", x, y, None) {
                        out.push(prop);
                    }
                }
                along += LAMP_SPACING_M;
            }
            carried = (carried + length) % LAMP_SPACING_M;
        }
    }

    /// Tents, campers and a coach at every mapped camp site, and a cluster
    /// around one mapped only as a point.
    fn camp_sites(ctx: &Ctx, out: &mut Vec<Prop>) {
        for area in ctx.layout.areas.iter().filter(|a| a.kind == "camp_site") {
            let start = out.len();
            let perimeter: f32 = area
                .ring
                .windows(2)
                .map(|p| (p[1][0] - p[0][0]).hypot(p[1][1] - p[0][1]))
                .sum();
            let pitches = ((perimeter / 100.0) * CAMP_PITCHES_PER_100M) as usize;
            let (cx, cy) = area.centre();
            let span = area.area_m2().sqrt().max(20.0);
            let mut placed = 0;
            let mut taken: Vec<(f32, f32)> = Vec::new();
            for i in 0..pitches * 4 {
                if placed >= pitches {
                    break;
                }
                // A low-discrepancy walk that fills the area (the R2
                // sequence, `r2_point`), not one irrational step for both
                // coordinates, which lined the pitches up in a row.
                let (x, y) = r2_point(cx, cy, span, i, 0.5);
                if !area.contains(x, y) {
                    continue;
                }
                let asset = match (roll(x, y, 21) * 4.0) as u32 {
                    0 | 1 => "tent_6m",
                    2 => "camper_van",
                    _ => "motorhome",
                };
                if let Some(prop) = ctx.place(PropKind::Attraction, "tent_6m", x, y, None) {
                    let kind = if asset == "tent_6m" {
                        PropKind::Attraction
                    } else {
                        PropKind::Vehicle
                    };
                    taken.push((prop.x, prop.y));
                    out.push(Prop {
                        kind,
                        asset: asset.to_string(),
                        ..prop
                    });
                    placed += 1;
                }
            }
            if ctx.fan {
                camp_village(ctx, area, pitches, &mut taken, out);
            }
            stamp_area(out, start, "camp site", area);
        }

        // A camp mapped only as a node gets a small cluster around it.
        for poi in ctx.layout.poi.iter().filter(|p| p.kind == "camp_site") {
            for i in 0..8 {
                let angle = i as f32 / 8.0 * TAU;
                let radius = 18.0 + (i % 3) as f32 * 9.0;
                let x = poi.centre[0] + radius * angle.cos();
                let y = poi.centre[1] + radius * angle.sin();
                let asset = if i % 3 == 0 { "camper_van" } else { "tent_6m" };
                let kind = if i % 3 == 0 {
                    PropKind::Vehicle
                } else {
                    PropKind::Attraction
                };
                if let Some(prop) = ctx.place(kind, asset, x, y, None) {
                    out.push(prop);
                }
            }
        }
    }

    /// Point `i` of the R2 low-discrepancy sequence (Roberts 2018: steps of
    /// 1/g and 1/g² for the plastic number g) over a `span` square round
    /// (cx, cy), starting at `phase`: it fills a square evenly, where one
    /// irrational step used for both coordinates lines the points up.
    fn r2_point(cx: f32, cy: f32, span: f32, i: usize, phase: f32) -> (f32, f32) {
        let (a1, a2) = (0.754_877_7_f64, 0.569_840_3_f64);
        let u = (phase as f64 + a1 * i as f64).fract() as f32;
        let v = (phase as f64 + a2 * i as f64).fract() as f32;
        (cx + (u - 0.5) * span, cy + (v - 0.5) * span)
    }

    /// The rest of a 24-hour camp among its tents: home-built scaffold
    /// towers to watch from, pop-up gazebos and the crowd's flags, on a
    /// second low-discrepancy walk of the area, clear of what is there.
    fn camp_village(
        ctx: &Ctx,
        area: &crate::layout::Area,
        pitches: usize,
        taken: &mut Vec<(f32, f32)>,
        out: &mut Vec<Prop>,
    ) {
        let (cx, cy) = area.centre();
        let span = area.area_m2().sqrt().max(20.0);
        let wanted = pitches / CAMP_PITCHES_PER_PIECE;
        let mut placed = 0;
        for i in 0..wanted * 6 {
            if placed >= wanted {
                break;
            }
            let (x, y) = r2_point(cx, cy, span, i, 0.27);
            if !area.contains(x, y)
                || taken
                    .iter()
                    .any(|(tx, ty)| (tx - x).hypot(ty - y) < CAMP_PIECE_CLEAR_M)
            {
                continue;
            }
            let r = roll(x, y, 23);
            let asset = if r < 0.25 {
                "camp_scaffold_tower"
            } else if r < 0.65 {
                "camp_gazebo"
            } else {
                "camp_flag_pole"
            };
            let yaw = roll(x, y, 24) * TAU;
            if let Some(mut prop) = ctx.place(PropKind::Misc, asset, x, y, Some(yaw)) {
                if asset == "camp_flag_pole" {
                    let flag = FAN_FLAGS
                        [(roll(x, y, 25) * FAN_FLAGS.len() as f32) as usize % FAN_FLAGS.len()];
                    prop.text = Some(flag.to_string());
                }
                taken.push((prop.x, prop.y));
                out.push(prop);
                placed += 1;
            }
        }
    }

    /// An event car park named by its lot code, a letter and a number
    /// (`A1`, `D10`, `A5 + A6`), as a circuit complex numbers its lots.
    fn is_lot_code(name: &str) -> bool {
        let mut chars = name.trim().chars();
        matches!(
            (chars.next(), chars.next()),
            (Some(a), Some(b)) if a.is_ascii_uppercase() && b.is_ascii_digit()
        )
    }

    /// Earth banks with a hedge on top, behind the rail along every stretch
    /// a mapped car park or camp site borders: on the Nordschleife the
    /// crowd parks and camps where it watches (Brünnchen, Pflanzgarten,
    /// Breidscheid ...), so the areas say where the banks are and on which
    /// side. Not a spectator car park: one OSM names as a paddock, depot or
    /// compound (`SPECTATOR_AREA_NOT`), an event lot named by its code
    /// (`A1`, `D10`, `A5 + A6`: the GP complex's, `is_lot_code`), and
    /// anything within `SPECTATOR_START_CLEAR_M` of the line.
    fn spectator_banks(ctx: &Ctx, out: &mut Vec<Prop>) {
        let total = ctx.path.total_length_m();
        let mut laid: Vec<(f32, f32)> = Vec::new();
        let for_spectators = |a: &&crate::layout::Area| {
            matches!(a.kind.as_str(), "parking" | "camp_site")
                && !a.name.as_deref().is_some_and(|name| {
                    let lower = name.to_lowercase();
                    is_lot_code(name) || SPECTATOR_AREA_NOT.iter().any(|word| lower.contains(word))
                })
        };
        for area in ctx.layout.areas.iter().filter(for_spectators) {
            // Where along the lap, and on which side, the area comes
            // within reach of the road: its outline walked every 5 m.
            let mut hits: Vec<(f32, Side)> = Vec::new();
            let n = area.ring.len();
            for i in 0..n {
                let (a, b) = (area.ring[i], area.ring[(i + 1) % n]);
                let len = (b[0] - a[0]).hypot(b[1] - a[1]);
                let steps = (len / 5.0).ceil().max(1.0) as usize;
                for k in 0..steps {
                    let f = k as f32 / steps as f32;
                    let (x, y) = (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f);
                    let (sample, lat) = nearest_cross_section(ctx.path, x, y);
                    let side = side_of(lat);
                    if lat.abs() - side_half_width(&sample, side) <= SPECTATOR_AREA_REACH_M {
                        hits.push((sample.station_m, side));
                    }
                }
            }
            let Some(&(s0, _)) = hits.first() else {
                continue;
            };
            let left = hits.iter().filter(|(_, s)| *s == Side::Left).count();
            let side = if left * 2 >= hits.len() {
                Side::Left
            } else {
                Side::Right
            };
            // Stations unwrapped round the first hit, so a car park across
            // the start line is one stretch.
            let rel = |s: f32| (s - s0 + total / 2.0).rem_euclid(total) - total / 2.0;
            let (lo, hi) = hits
                .iter()
                .filter(|(_, s)| *s == side)
                .fold((f32::MAX, f32::MIN), |(lo, hi), (s, _)| {
                    (lo.min(rel(*s)), hi.max(rel(*s)))
                });
            let (lo, hi) = (lo + 4.0, hi.min(lo + SPECTATOR_BANK_MAX_RUN_M) - 4.0);
            if hi - lo < SPECTATOR_BANK_PITCH_M * 0.5 {
                continue;
            }
            let count = ((hi - lo) / SPECTATOR_BANK_PITCH_M).floor() as usize + 1;
            let first = (lo + hi) / 2.0 - SPECTATOR_BANK_PITCH_M * (count as f32 - 1.0) / 2.0;
            for i in 0..count {
                let station = (s0 + first + i as f32 * SPECTATOR_BANK_PITCH_M).rem_euclid(total);
                if station.min(total - station) < SPECTATOR_START_CLEAR_M {
                    continue;
                }
                let sample = ctx.path.sample_at(station);
                let lat = signed_lat(&sample, side, SPECTATOR_BANK_BEYOND_M);
                let point = offset_point(&sample, lat);
                if laid
                    .iter()
                    .any(|(x, y)| (x - point.0).hypot(y - point.1) < SPECTATOR_BANK_PITCH_M * 0.6)
                {
                    continue;
                }
                if let Some(prop) = ctx.place_clear(
                    PropKind::Misc,
                    "spectator_bank_10m",
                    point.0,
                    point.1,
                    Some(sample.heading_rad),
                    TRACKSIDE_CLEAR_M,
                ) {
                    laid.push((prop.x, prop.y));
                    out.push(prop);
                }
            }
        }
    }

    /// Houses and barns across the villages and farmyards the circuit sits
    /// among. The Red Bull Ring has two hamlets and several farms inside
    /// two kilometres, and the horizon looked like a golf course without
    /// them.
    fn village(ctx: &Ctx, out: &mut Vec<Prop>) {
        for area in ctx
            .layout
            .areas
            .iter()
            .filter(|a| matches!(a.kind.as_str(), "residential" | "farmyard"))
        {
            let start = out.len();
            let (cx, cy) = area.centre();
            let span = area.area_m2().sqrt().max(30.0);
            let steps = ((span / HOUSE_SPACING_M) as i32).clamp(1, 8);
            let budget = ((area.area_m2() / HOUSE_AREA_PER_BUILDING_M2) as usize)
                .clamp(1, MAX_HOUSES_PER_AREA);
            let mut placed = 0;
            for row in -steps..=steps {
                for col in -steps..=steps {
                    if placed >= budget {
                        break;
                    }
                    let x = cx + col as f32 * HOUSE_SPACING_M;
                    let y = cy + row as f32 * HOUSE_SPACING_M;
                    if !area.contains(x, y) {
                        continue;
                    }
                    let r = roll(x, y, 31);
                    // A farmyard is a barn and a house or two; a village
                    // is houses.
                    let asset = if area.kind == "farmyard" && r < 0.45 {
                        "barn"
                    } else {
                        VILLAGE_HOUSES[(r * 3.0) as usize % 3]
                    };
                    // Houses face the lane, not the circuit: a yaw from
                    // their own position reads as a village rather than a
                    // grandstand row.
                    let yaw = roll(x, y, 32) * TAU;
                    if let Some(prop) = ctx.place(PropKind::Building, asset, x, y, Some(yaw)) {
                        out.push(prop);
                        placed += 1;
                    }
                }
            }
            stamp_area(out, start, "village", area);
        }
    }

    /// Woodland on the slopes, as one billboard cluster per 40 m patch.
    ///
    /// The detailed belts reach 90 m from the road and stop, and past
    /// that every circuit had bare ground to the horizon. Planting real
    /// trees out there is not affordable — a 550-triangle impostor of
    /// billboards covers what forty-six card trees would — and the mapped woodland
    /// does not reach that far either, because the OSM extracts are a
    /// couple of kilometres across.
    ///
    /// So this reads the land instead: where the elevation model says the
    /// ground rises and slopes, it is a wooded slope, which at the Red
    /// Bull Ring, Spa, the Nürburgring and Zandvoort's dunes is right for
    /// three of the four. Zandvoort is the exception and is handled the
    /// way it already was, by the track's `environment_type`: a desert or
    /// dune circuit plants none.
    fn forests(ctx: &Ctx, dem: Option<&DemFile>, out: &mut Vec<Prop>) {
        let Some(dem) = dem else {
            return;
        };
        if ctx.bare {
            return;
        }
        let total = ctx.path.total_length_m();
        // A grid over the whole far field, walked from the circuit's own
        // bounding box so the result does not depend on where the origin
        // happens to be.
        let samples = ctx.path.samples();
        let (mut min_x, mut min_y) = (f32::MAX, f32::MAX);
        let (mut max_x, mut max_y) = (f32::MIN, f32::MIN);
        for sample in samples {
            min_x = min_x.min(sample.pos.0);
            min_y = min_y.min(sample.pos.1);
            max_x = max_x.max(sample.pos.0);
            max_y = max_y.max(sample.pos.1);
        }
        let _ = total;
        let step = IMPOSTOR_SPACING_M;
        let mut y = min_y - IMPOSTOR_FAR_M;
        while y <= max_y + IMPOSTOR_FAR_M {
            let mut x = min_x - IMPOSTOR_FAR_M;
            while x <= max_x + IMPOSTOR_FAR_M {
                // Jittered off the lattice so the wood does not read as a
                // plantation, deterministically.
                let jx = x + (roll(x, y, 41) - 0.5) * step * 0.6;
                let jy = y + (roll(x, y, 42) - 0.5) * step * 0.6;
                x += step;

                let (sample, lat) = nearest_cross_section(ctx.path, jx, jy);
                let from_road = lat.abs() - side_half_width(&sample, side_of(lat));
                let z = dem.height_at(jx, jy);
                // A mapped wood is forest wherever it lies past the belt,
                // its impostors by its leaf type.
                let wood = (WOOD_IMPOSTOR_NEAR_M..=IMPOSTOR_FAR_M)
                    .contains(&from_road)
                    .then(|| ctx.layout.woods.iter().find(|w| w.contains(jx, jy)))
                    .flatten();
                let asset = if let Some(wood) = wood {
                    match wood.leaf.as_str() {
                        "needleleaved" => "forest_impostor_conifer",
                        "broadleaved" => "forest_impostor",
                        _ if roll(jx, jy, 45) < 0.5 => "forest_impostor_conifer",
                        _ => "forest_impostor",
                    }
                } else {
                    if !(IMPOSTOR_NEAR_M..=IMPOSTOR_FAR_M).contains(&from_road) {
                        continue;
                    }
                    let reach = step;
                    let dzdx = (dem.height_at(jx + reach, jy) - dem.height_at(jx - reach, jy))
                        / (2.0 * reach);
                    let dzdy = (dem.height_at(jx, jy + reach) - dem.height_at(jx, jy - reach))
                        / (2.0 * reach);
                    let slope = dzdx.hypot(dzdy);
                    if z - sample.pos.2 >= IMPOSTOR_MIN_RISE_M && slope >= IMPOSTOR_MIN_SLOPE {
                        // Spruce on the steeper, higher ground and mixed
                        // below, which is how a real tree line goes.
                        if slope > IMPOSTOR_MIN_SLOPE * 2.0 {
                            "forest_impostor_conifer"
                        } else {
                            "forest_impostor"
                        }
                    } else if ctx.horizon
                        && (HORIZON_NEAR_M..=HORIZON_FAR_M).contains(&from_road)
                        && !ctx.terrain.in_water(jx, jy)
                        && roll(
                            (jx / HORIZON_PATCH_M).floor(),
                            (jy / HORIZON_PATCH_M).floor(),
                            46,
                        ) < HORIZON_PATCH_SHARE
                        && roll(jx, jy, 47) < HORIZON_FILL
                    {
                        "forest_impostor"
                    } else {
                        continue;
                    }
                };
                out.push(Prop {
                    id: 0,
                    kind: PropKind::Tree,
                    asset: asset.to_string(),
                    x: round2(jx),
                    y: round2(jy),
                    z: round2(z),
                    yaw_rad: round2(roll(jx, jy, 43) * TAU),
                    scale: round2(0.85 + roll(jx, jy, 44) * 0.5),
                    text: None,
                    length_m: None,
                    source: None,
                });
            }
            y += step;
        }
    }

    /// The point features: chapels, pylons, food stalls, gates.
    fn points_of_interest(ctx: &Ctx, out: &mut Vec<Prop>) {
        for poi in &ctx.layout.poi {
            let (kind, asset) = match poi.kind.as_str() {
                "chapel" => (PropKind::Building, "chapel"),
                "pylon" => (PropKind::Misc, "power_pylon"),
                "food" => (PropKind::Attraction, "food_stall_6m"),
                // A city circuit's street furniture (`CITY_POI_KINDS`).
                "tree" => (PropKind::Tree, "raintree_l"),
                "station" => (PropKind::Misc, "station_entrance"),
                "signal" => (PropKind::Sign, "traffic_signal_pole"),
                "bus_shelter" => (PropKind::Misc, "bus_shelter"),
                // `tourism=information` is a map board or a noticeboard,
                // not an entrance; the kit's ticket gate would be a lie.
                _ => continue,
            };
            if let Some(prop) = ctx.place(kind, asset, poi.centre[0], poi.centre[1], None) {
                out.push(prop);
            }
        }
    }

    /// Where the marshal posts stand: every named corner, and along the
    /// straights.
    fn marshal_stations(ctx: &Ctx) -> Vec<f32> {
        let total = ctx.path.total_length_m();
        let mut stations: Vec<f32> = ctx.layout.corners.iter().map(|c| c.station_m).collect();
        let mut station = 0.0;
        while station < total {
            if !stations
                .iter()
                .any(|s| (s - station).abs() < MARSHAL_SPACING_M * 0.5)
            {
                stations.push(station);
            }
            station += MARSHAL_SPACING_M;
        }
        stations.sort_by(f32::total_cmp);
        stations
    }

    /// Knots of standing spectators where a crowd gathers: in front of a
    /// fan-zone stage, round a big wheel, by a food stall. Each faces the
    /// road; a group that would stand on the run-off is left out.
    fn people(ctx: &Ctx, out: &mut Vec<Prop>) {
        let mut spots: Vec<(f32, f32, usize, f32)> = Vec::new();
        for l in &ctx.layout.landmarks {
            match l.kind.as_str() {
                "stage" => spots.push((l.centre[0], l.centre[1], STAGE_CROWD_GROUPS, 9.0)),
                "big_wheel" => spots.push((l.centre[0], l.centre[1], WHEEL_CROWD_GROUPS, 34.0)),
                _ => {}
            }
        }
        for poi in ctx.layout.poi.iter().filter(|p| p.kind == "food") {
            spots.push((poi.centre[0], poi.centre[1], 1, 6.0));
        }
        for (cx, cy, groups, reach) in spots {
            for k in 0..groups {
                let a = (k as f32 + roll(cx, cy, 51 + k as u32)) / groups as f32 * TAU;
                let r = reach + roll(cx, cy, 61 + k as u32) * 4.0;
                let (x, y) = (cx + a.cos() * r, cy + a.sin() * r);
                let (sample, _) = nearest_cross_section(ctx.path, x, y);
                if let Some(prop) = ctx.place(
                    PropKind::Misc,
                    "crowd_group",
                    x,
                    y,
                    Some(sample.heading_rad),
                ) {
                    out.push(prop);
                }
            }
        }
    }

    /// The side a post at `station` stands on: the outside of the bend.
    fn marshal_side(ctx: &Ctx, station: f32) -> Side {
        if curvature_at(ctx.path, station) < 0.0 {
            Side::Left
        } else {
            Side::Right
        }
    }

    /// A marshal post at every named corner and along the straights.
    fn marshal_posts(ctx: &Ctx, out: &mut Vec<Prop>) {
        let stations = marshal_stations(ctx);
        // On the fan lap the posts are numbered round the lap from the line.
        let asset = if ctx.fan {
            "marshal_post_numbered"
        } else {
            "marshal_post"
        };
        let mut number = 0;
        for station in stations {
            let sample = ctx.path.sample_at(station);
            let side = marshal_side(ctx, station);
            let lat = signed_lat(&sample, side, MARSHAL_BEYOND_BARRIER_M + 8.0);
            let point = offset_point(&sample, lat);
            if let Some(mut prop) = ctx.place_clear(
                PropKind::Sign,
                asset,
                point.0,
                point.1,
                Some(sample.heading_rad),
                TRACKSIDE_CLEAR_M,
            ) {
                if ctx.fan {
                    number += 1;
                    if number <= MARSHAL_NUMBERS {
                        prop.text = Some(format!("post{number}"));
                    }
                }
                out.push(prop);
                // Its marshal, a couple of metres up the road from the
                // post, facing the track.
                let (sin, cos) = sample.heading_rad.sin_cos();
                let (fx, fy) = (
                    point.0 - cos * MARSHAL_FIGURE_AHEAD_M,
                    point.1 - sin * MARSHAL_FIGURE_AHEAD_M,
                );
                if let Some(figure) = ctx.place_clear(
                    PropKind::Misc,
                    "marshal_figure",
                    fx,
                    fy,
                    Some(sample.heading_rad),
                    TRACKSIDE_CLEAR_M,
                ) {
                    out.push(figure);
                }
            }
        }
    }

    /// The marshals' light panels (`board/light_panel`), which the client
    /// lights from race control's flags: one beside every marshal post,
    /// and one in the middle of any marshal sector that has no post, so
    /// every sector's yellow shows somewhere. The client finds each
    /// panel's sector from where it stands.
    fn light_panels(ctx: &Ctx, out: &mut Vec<Prop>) {
        let total = ctx.path.total_length_m();
        let mut stations = marshal_stations(ctx);
        let sectors = (total / MARSHAL_SECTOR_M).ceil() as usize;
        for k in 0..sectors {
            let (from, to) = (
                k as f32 * MARSHAL_SECTOR_M,
                (k + 1) as f32 * MARSHAL_SECTOR_M,
            );
            if !stations.iter().any(|&s| s >= from && s < to) {
                stations.push(((from + to) / 2.0).min(total - 1.0));
            }
        }
        stations.sort_by(f32::total_cmp);
        for station in stations {
            let at = (station - LIGHT_PANEL_UP_COURSE_M).rem_euclid(total);
            let sample = ctx.path.sample_at(at);
            let side = marshal_side(ctx, station);
            let lat = signed_lat(&sample, side, MARSHAL_BEYOND_BARRIER_M + 8.0);
            let point = offset_point(&sample, lat);
            if let Some(prop) = ctx.place_clear(
                PropKind::Board,
                "light_panel",
                point.0,
                point.1,
                Some(sample.heading_rad),
                TRACKSIDE_CLEAR_M,
            ) {
                out.push(prop);
            }
        }
    }

    /// The board carrying a corner's name, at its entry: the dossier's
    /// `display_name`, never the real `name`, which is as often a sponsor's
    /// trademark (Remus, Schlossgold, Würth Kurve) as a place. A corner with
    /// no display name gets no board.
    fn corner_signs(ctx: &Ctx, out: &mut Vec<Prop>) {
        for corner in &ctx.layout.corners {
            let Some(label) = corner
                .display_name
                .as_deref()
                .map(str::trim)
                .filter(|label| !label.is_empty())
            else {
                continue;
            };
            let station = if corner.from_m > 0.0 {
                corner.from_m
            } else {
                corner.station_m
            };
            let sample = ctx.path.sample_at(station);
            let side = if curvature_at(ctx.path, corner.station_m) < 0.0 {
                Side::Left
            } else {
                Side::Right
            };
            let lat = signed_lat(&sample, side, 10.0);
            let point = offset_point(&sample, lat);
            if let Some(mut prop) = ctx.place_clear(
                PropKind::Board,
                "corner_sign",
                point.0,
                point.1,
                Some(sample.heading_rad),
                TRACKSIDE_CLEAR_M,
            ) {
                prop.text = Some(label.to_string());
                out.push(prop);
            }
        }
    }

    /// Masts for a circuit that has none of its own.
    ///
    /// The Red Bull Ring is not floodlit and OpenStreetMap rightly maps no
    /// lighting masts there, so a session set after dark was lit by a one
    /// lux moon and the car's own headlights and nothing else. A circuit
    /// that cannot be driven at night is worse than one that is lit more
    /// generously than the real place, so this puts masts round a circuit
    /// with none — behind the barrier, at the corners first.
    fn floodlights(ctx: &Ctx, out: &mut Vec<Prop>) {
        let total = ctx.path.total_length_m();
        let mut station = 0.0;
        while station < total {
            let sample = ctx.path.sample_at(station);
            let side = if curvature_at(ctx.path, station) < 0.0 {
                Side::Left
            } else {
                Side::Right
            };
            let lat = signed_lat(&sample, side, FLOODLIGHT_BEYOND_BARRIER_M + 10.0);
            let point = offset_point(&sample, lat);
            if let Some(prop) = ctx.place(
                PropKind::Light,
                "floodlight_tower",
                point.0,
                point.1,
                Some(sample.heading_rad),
            ) {
                out.push(prop);
            }
            station += FLOODLIGHT_SPACING_M;
        }
    }

    fn signed_lat(sample: &PathSample, side: Side, beyond_edge: f32) -> f32 {
        let lat = side_half_width(sample, side) + beyond_edge;
        match side {
            Side::Left => lat,
            Side::Right => -lat,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ats::AtsScene;
    use crate::layout::{Layout, PitRoad, Wood};
    use crate::track_data::{TrackFile, TrackNode};

    fn node(x: f32, y: f32) -> TrackNode {
        TrackNode {
            x,
            y,
            z: 0.0,
            width: None,
            width_left: None,
            width_right: None,
            banking: None,
            friction: None,
            surface_type: None,
        }
    }

    /// A flat stadium, counter-clockwise: 800 m along `y = 0`, a 70 m
    /// hairpin, the return straight at `y = 140`, a second hairpin home.
    fn track() -> TrackFile {
        const R: f32 = 70.0;
        let mut nodes = Vec::new();
        for i in 0..=32 {
            nodes.push(node(i as f32 * 25.0, 0.0));
        }
        for deg in (-80..=80).step_by(10) {
            let (sin, cos) = (deg as f32).to_radians().sin_cos();
            nodes.push(node(800.0 + R * cos, R + R * sin));
        }
        for i in (0..=32).rev() {
            nodes.push(node(i as f32 * 25.0, 2.0 * R));
        }
        for deg in (100..=260).step_by(10) {
            let (sin, cos) = (deg as f32).to_radians().sin_cos();
            nodes.push(node(R * cos, R + R * sin));
        }
        TrackFile {
            name: "Dress".to_string(),
            display_name: None,
            track_id: None,
            nodes,
            checkpoints: vec![],
            sectors: vec![],
            spawn_points: vec![],
            default_width: 12.0,
            closed_loop: true,
            raceline: vec![],
            drs_zones: vec![],
            metadata: None,
        }
    }

    /// Aliases so a test that binds a local called `scene` or `layout`
    /// can still build another one.
    fn make_scene(track: &TrackFile) -> AtsScene {
        scene(track)
    }

    fn make_layout() -> Layout {
        layout()
    }

    fn layout() -> Layout {
        Layout {
            format: crate::layout::LAYOUT_FORMAT.to_string(),
            version: crate::layout::LAYOUT_VERSION,
            source_track: "Dress.yaml".to_string(),
            track_name: "Dress".to_string(),
            attribution: String::new(),
            corners: vec![],
            pit_lane: None,
            stands: vec![],
            structures: vec![],
            crossings: vec![],
            landmarks: vec![],
            woods: vec![],
            ..Default::default()
        }
    }

    fn structure(centre: [f32; 2], length_m: f32, depth_m: f32, yaw_rad: f32) -> Structure {
        Structure {
            name: None,
            station_m: centre[0],
            side: if centre[1] >= 0.0 {
                Side::Left
            } else {
                Side::Right
            },
            offset_m: centre[1].abs(),
            length_m,
            depth_m,
            yaw_rad,
            centre,
            levels: 1,
            area_m2: 900.0,
            osm_building: Some("yes".to_string()),
            asset: None,
        }
    }

    fn building_lats(track: &TrackFile, scene: &AtsScene) -> Vec<f32> {
        let path = CenterlinePath::from_track(track).unwrap();
        scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Building)
            .map(|p| nearest_cross_section(&path, p.x, p.y).1)
            .collect()
    }

    /// A slightly skewed building right of the main straight whose
    /// bounding box reaches onto the asphalt (front at y = -2 on a 6 m
    /// half-width) is moved back until its front clears the road.
    /// Graffiti comes from the dossier, is trimmed to the tarmac, and a
    /// second dressing writes the same decals under the same ids; a decal
    /// of another set is left alone.
    #[test]
    fn graffiti_is_laid_on_the_road_and_redressed_in_place() {
        let track = track();
        let mut scene = scene(&track);
        let other = scene.alloc_id();
        scene.decals.push(Decal {
            id: other,
            image: "logo/start".to_string(),
            start_m: 5.0,
            length_m: 4.0,
            lat_m: 0.0,
            width_m: 4.0,
            reversed: false,
        });
        let mut layout = layout();
        let entry =
            |image: &str, station_m: f32, lat_m: f32, width_m: f32| crate::layout::Graffiti {
                image: image.to_string(),
                station_m,
                lat_m,
                length_m: 16.0,
                width_m,
                reversed: false,
                name: None,
            };
        layout.graffiti = vec![
            entry("graffiti/vollgas", 100.0, 0.0, 7.0),
            // Wider than the 12 m road: trimmed to it.
            entry("graffiti/eifel", 300.0, 2.0, 20.0),
            // Entirely on the verge: left out.
            entry("graffiti/kalle", 400.0, 12.0, 3.0),
            entry("not_graffiti/x", 500.0, 0.0, 4.0),
        ];
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(report.graffiti, 2, "{:?}", report.skipped);
        assert_eq!(report.skipped.len(), 2, "{:?}", report.skipped);
        let eifel = scene
            .decals
            .iter()
            .find(|d| d.image == "graffiti/eifel")
            .unwrap();
        assert!(
            eifel.lat_m + eifel.width_m / 2.0 <= 6.0 && eifel.lat_m - eifel.width_m / 2.0 >= -6.0
        );
        assert!(scene.decals.iter().any(|d| d.id == other));
        scene.validate().unwrap();

        let first = scene.clone();
        dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(scene.decals, first.decals);
        assert_eq!(scene.next_id, first.next_id);
    }

    /// A row of venue blocks beside the straight whose last block would
    /// stand on a paved run-off apron loses that block and keeps the
    /// rest: clear of the asphalt is not clear of the run-off (Spa's
    /// Stand Endurance put a media centre over Eau Rouge's apron). A gravel
    /// trap there takes nothing away.
    #[test]
    fn a_building_unit_on_a_paved_runoff_is_left_out() {
        let track = track();
        let mut layout = layout();
        layout
            .structures
            .push(structure([300.0, -30.0], 120.0, 20.0, 0.0));
        let row = |kind: crate::ats::SurfaceKind| {
            let mut scene = scene(&track);
            let id = scene.alloc_id();
            scene.surfaces.push(Surface {
                id,
                kind,
                side: Side::Right,
                start_m: 330.0,
                end_m: 380.0,
                inner_m: 0.0,
                width_m: 40.0,
                end_width_m: None,
                paint: None,
            });
            dress_scene(&track, &mut scene, &layout).unwrap();
            building_lats(&track, &scene).len()
        };
        let all = row(crate::ats::SurfaceKind::Gravel);
        assert_eq!(all, 3, "a gravel trap takes nothing");
        assert_eq!(
            row(crate::ats::SurfaceKind::AsphaltRunoff),
            all - 1,
            "the block on the apron is left out"
        );
    }

    /// The paddock blocks (open ground floors) stand only near the pit
    /// lane; a big building elsewhere is an office block, or a hall when it
    /// has one storey.
    #[test]
    fn paddock_blocks_only_by_the_pit_lane() {
        let mut big = structure([0.0, 0.0], 80.0, 20.0, 0.0);
        big.area_m2 = 1600.0;
        big.levels = 0;
        assert_eq!(building_asset(&big, true).0, "media_centre");
        assert_eq!(building_asset(&big, false).0, "office_block");
        big.levels = 1;
        assert_eq!(building_asset(&big, false).0, "venue_hall");
        let mut small = structure([0.0, 0.0], 25.0, 20.0, 0.0);
        small.area_m2 = 400.0;
        assert_eq!(building_asset(&small, false).0, "clubhouse");
        for asset in ["office_block", "venue_hall"] {
            assert!(
                props::find(PropKind::Building, asset).is_some(),
                "{asset} is in the kit"
            );
        }
    }

    /// A long infield row whose tail reaches out past the hairpin loses the
    /// blocks beyond it and keeps the rest (R14: Lakeside Stadium).
    #[test]
    fn a_row_reaching_round_a_bend_keeps_the_blocks_on_its_side() {
        let track = track();
        let path = CenterlinePath::from_track(&track).unwrap();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout
            .structures
            .push(structure([680.0, 40.0], 520.0, 10.0, 0.0));
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        let laid: Vec<_> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Building)
            .collect();
        assert!(!laid.is_empty(), "{:?}", report.skipped);
        for p in &laid {
            assert!(
                nearest_cross_section(&path, p.x, p.y).1 > 0.0,
                "a block outside the hairpin at {}",
                p.x
            );
        }

        // A roof outline (a stand's canopy) is not trimmed into a row.
        let mut roofed = self::scene(&track);
        layout.structures[0].osm_building = Some("roof".to_string());
        let report = dress_scene(&track, &mut roofed, &layout).unwrap();
        assert!(building_lats(&track, &roofed).is_empty());
        assert!(
            report.skipped.iter().any(|s| s.contains("across the road")),
            "{:?}",
            report.skipped
        );
    }

    /// A building the apron takes whole is stepped back off it rather than
    /// left out (R14: the MSV Centre, the Salons de l'Eau Rouge).
    #[test]
    fn a_building_wholly_on_a_paved_runoff_is_stepped_back_off_it() {
        let track = track();
        let mut layout = layout();
        layout
            .structures
            .push(structure([300.0, -30.0], 40.0, 20.0, 0.0));
        let mut scene = scene(&track);
        let id = scene.alloc_id();
        scene.surfaces.push(Surface {
            id,
            kind: crate::ats::SurfaceKind::AsphaltRunoff,
            side: Side::Right,
            start_m: 250.0,
            end_m: 350.0,
            inner_m: 0.0,
            width_m: 30.0,
            end_width_m: None,
            paint: None,
        });
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        let lats = building_lats(&track, &scene);
        assert!(!lats.is_empty(), "{:?}", report.skipped);
        for lat in &lats {
            assert!(
                *lat <= -(6.0 + 30.0) + 0.1,
                "a building front at lateral {lat} m: {lats:?}"
            );
        }
    }

    #[test]
    fn a_building_boxed_onto_the_road_is_laid_clear_of_it() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout
            .structures
            .push(structure([300.0, -12.0], 60.0, 20.0, 0.1));
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert!(report.buildings >= 1, "{:?}", report.skipped);
        let lats = building_lats(&track, &scene);
        for lat in &lats {
            assert!(
                *lat <= -(6.0 + BUILDING_ROAD_CLEAR_M) + 0.1,
                "a building front at lateral {lat} m: {lats:?}"
            );
        }
    }

    /// The venue landmarks the kit models each lay their own asset, and a
    /// dressing re-run owns them (the cauldron is `misc`, which the pass
    /// otherwise leaves alone).
    #[test]
    fn venue_landmark_kinds_lay_their_kit_asset() {
        for (kind, prop_kind, asset) in [
            ("observation_tower", PropKind::Building, "observation_tower"),
            ("glass_tower", PropKind::Building, "round_glass_tower"),
            ("media_wing", PropKind::Building, "wing_media_centre"),
            ("arena", PropKind::Building, "arena_dome"),
            ("stadium", PropKind::Building, "stadium_bowl"),
            ("veil_tower", PropKind::Building, "veil_tower"),
            ("expo_pavilion", PropKind::Building, "expo_pavilion"),
            ("geodesic_dome", PropKind::Building, "geodesic_dome"),
            ("fuel_station", PropKind::Building, "fuel_station_canopy"),
            ("cauldron", PropKind::Misc, "cauldron"),
        ] {
            assert!(
                props::find(prop_kind, asset).is_some(),
                "{asset} is in the kit"
            );
            let track = track();
            let mut scene = scene(&track);
            let mut layout = layout();
            layout.landmarks.push(Landmark {
                kind: kind.to_string(),
                name: None,
                station_m: 300.0,
                side: Side::Left,
                centre: [300.0, 500.0],
                brand: None,
                yaw_rad: None,
                altitude_m: None,
                pit_roof: false,
            });
            let report = dress_scene(&track, &mut scene, &layout).unwrap();
            assert_eq!(report.landmarks, 1, "{kind}: {:?}", report.skipped);
            let laid = scene
                .props
                .iter()
                .find(|p| p.kind == prop_kind && p.asset == asset)
                .unwrap_or_else(|| panic!("{kind} lays {asset}"));
            assert!(dressed_prop(laid), "{asset} is the dressing's own");
        }
    }

    /// Monza's Torre Sud: an OSM tower node mapped half a metre off the
    /// centerline put a 13 m tower across the start straight. It is
    /// stepped off the road toward the dossier's side, whole footprint
    /// clear.
    #[test]
    fn a_landmark_mapped_on_the_road_is_stepped_off_it() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.landmarks.push(Landmark {
            kind: "tower".to_string(),
            name: Some("Torre".to_string()),
            station_m: 300.0,
            side: Side::Left,
            centre: [300.0, -0.5],
            brand: None,
            yaw_rad: None,
            altitude_m: None,
            pit_roof: false,
        });
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(report.landmarks, 1, "{:?}", report.skipped);
        let tower = scene
            .props
            .iter()
            .find(|p| p.asset == "control_tower")
            .expect("the tower is laid");
        let kit = props::find(PropKind::Building, "control_tower").unwrap();
        let near_edge = tower.y - kit.depth_m / 2.0;
        assert!(
            near_edge >= 6.0 + BUILDING_ROAD_CLEAR_M - 0.1,
            "the tower's road side is {near_edge} m left of the centerline"
        );
    }

    /// Two legs of the course 28 m apart (centre to centre), joined by
    /// hairpins: 16 m of grass between the road edges.
    fn hairpin_track() -> TrackFile {
        const R: f32 = 14.0;
        let mut nodes = Vec::new();
        for i in 0..=40 {
            nodes.push(node(i as f32 * 10.0, 0.0));
        }
        for deg in (-80..=80).step_by(10) {
            let (sin, cos) = (deg as f32).to_radians().sin_cos();
            nodes.push(node(400.0 + R * cos, R + R * sin));
        }
        for i in (0..=40).rev() {
            nodes.push(node(i as f32 * 10.0, 2.0 * R));
        }
        for deg in (100..=260).step_by(10) {
            let (sin, cos) = (deg as f32).to_radians().sin_cos();
            nodes.push(node(R * cos, R + R * sin));
        }
        TrackFile {
            name: "Hairpin".to_string(),
            nodes,
            ..track()
        }
    }

    /// Zandvoort's Hunserug: a building between two legs of the course
    /// had its front checked against the near road only, and the kit block
    /// behind that front reached across the far one. A block with no room
    /// for its whole footprint is left out rather than laid on the road.
    #[test]
    fn a_building_with_road_behind_it_is_not_laid_across_that_road() {
        let track = hairpin_track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout
            .structures
            .push(structure([200.0, 14.0], 60.0, 10.0, 0.0));
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        for p in scene.props.iter().filter(|p| p.kind == PropKind::Building) {
            let kit = props::resolve(p.kind, &p.asset).unwrap();
            let (gap, _) =
                footprint_road_gap(&path, p.x, p.y, p.yaw_rad, kit.length_m, kit.depth_m);
            assert!(gap > 0.0, "{} laid {gap:.1} m onto the road", p.asset);
        }
        assert!(
            report.skipped.iter().any(|s| s.contains("no room")),
            "{:?}",
            report.skipped
        );
    }

    /// A stand's bays are as deep as the kit makes them, whatever its
    /// traced front: a front 2 m off the asphalt on the outside of the
    /// straight is stepped back until no bay reaches the road.
    #[test]
    fn a_stand_traced_against_the_road_is_stepped_back_off_it() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.stands.push(stand(
            "Kerbside",
            vec![[200.0, -8.0], [320.0, -8.0]],
            20.0,
            true,
        ));
        dress_scene(&track, &mut scene, &layout).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let stands: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Grandstand)
            .collect();
        assert!(!stands.is_empty());
        for p in stands {
            let kit = props::resolve(p.kind, &p.asset).unwrap();
            let (gap, _) =
                footprint_road_gap(&path, p.x, p.y, p.yaw_rad, p.length_m.unwrap(), kit.depth_m);
            assert!(
                gap >= STAND_ROAD_CLEAR_M - 0.1,
                "a bay {gap:.1} m from the road"
            );
        }
    }

    /// A box that straddles the centerline is not a building beside the
    /// road, and a footbridge mapped as a building is not one at all.
    #[test]
    fn a_deck_bridge_is_tiled_along_its_span_at_road_height() {
        let track = track();
        let path = CenterlinePath::from_track(&track).unwrap();
        let crossing = Crossing {
            name: Some("Esplanade".to_string()),
            station_m: 400.0,
            kind: "deck_wide".to_string(),
            brand: None,
            from_m: Some(200.0),
            to_m: Some(600.0),
        };
        let tiles = lay_crossing(&path, &crossing);
        // 400 m of 40 m modules.
        assert_eq!(tiles.len(), 10);
        assert!(tiles
            .iter()
            .all(|p| p.asset == "bridge_deck_wide" && p.kind == PropKind::Bridge));
        // Contiguous: neighbours are one module apart along the road.
        for pair in tiles.windows(2) {
            let gap = (pair[1].x - pair[0].x).hypot(pair[1].y - pair[0].y);
            assert!((gap - 40.2).abs() < 1.5, "{gap}");
        }
        // A footbridge is still one prop over the road.
        let foot = Crossing {
            kind: "link".to_string(),
            from_m: None,
            to_m: None,
            ..crossing
        };
        let one = lay_crossing(&path, &foot);
        assert_eq!(one.len(), 1);
        assert_eq!(one[0].asset, "linkbridge_covered");
    }

    #[test]
    fn water_and_bridge_spans_come_from_the_dossier() {
        let track = track();
        let mut scene = make_scene(&track);
        let mut layout = make_layout();
        layout.water.push(crate::layout::WaterBody {
            name: Some("Bay".to_string()),
            rings: vec![vec![
                [300.0, 30.0],
                [500.0, 30.0],
                [500.0, 110.0],
                [300.0, 110.0],
            ]],
        });
        layout.crossings.push(Crossing {
            name: None,
            station_m: 400.0,
            kind: "deck_arch".to_string(),
            brand: None,
            from_m: Some(350.0),
            to_m: Some(450.0),
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(scene.water.len(), 1);
        assert_eq!(scene.bridges.len(), 1);
        let ids: Vec<u64> = scene
            .water
            .iter()
            .map(|w| w.id)
            .chain(scene.bridges.iter().map(|b| b.id))
            .collect();
        let again = {
            dress_scene(&track, &mut scene, &layout).unwrap();
            scene
                .water
                .iter()
                .map(|w| w.id)
                .chain(scene.bridges.iter().map(|b| b.id))
                .collect::<Vec<_>>()
        };
        assert_eq!(ids, again, "re-dressing must reuse the ids");
        // Nothing dressed stands in the bay.
        let field = crate::groom::seating_terrain(
            &CenterlinePath::from_track(&track).unwrap(),
            &scene,
            None,
        )
        .unwrap();
        assert!(scene
            .props
            .iter()
            .filter(|p| !matches!(p.kind, PropKind::Bridge | PropKind::Sky))
            .all(|p| !field.in_water(p.x, p.y)));
    }

    #[test]
    fn buildings_across_the_road_are_skipped() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout
            .structures
            .push(structure([300.0, -12.0], 60.0, 40.0, 0.1));
        let mut bridge = structure([500.0, 0.0], 70.0, 4.8, 1.6);
        bridge.osm_building = Some("bridge".to_string());
        layout.structures.push(bridge);
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(report.buildings, 0, "{:?}", report.skipped);
        assert!(
            report.skipped.iter().any(|s| s.contains("across the road")),
            "{:?}",
            report.skipped
        );
        assert!(
            report.skipped.iter().any(|s| s.contains("a bridge")),
            "{:?}",
            report.skipped
        );
    }

    fn stand(name: &str, front: Vec<[f32; 2]>, depth_m: f32, covered: bool) -> Stand {
        let length_m = front
            .windows(2)
            .map(|w| (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]))
            .sum();
        Stand {
            name: Some(name.to_string()),
            station_m: 0.0,
            side: Side::Right,
            offset_m: 12.0,
            length_m,
            depth_m,
            covered,
            roof: None,
            front,
            centre: [0.0, 0.0],
            yaw_rad: 0.0,
            family: None,
        }
    }

    fn scene(track: &TrackFile) -> AtsScene {
        AtsScene::new_for_track(track, "Dress.yaml")
    }

    /// An old oval's banking is tiled along its mapped line, its high side
    /// on the outside of the curve, and none of it near the circuit's road.
    #[test]
    fn a_banking_line_is_tiled_with_its_high_side_outside() {
        let track = track();
        let mut layout = layout();
        // A left-turning arc well inside the infield's far side.
        let arc: Vec<[f32; 2]> = (0..=12)
            .map(|i| {
                let a = -0.6 + 1.2 * i as f32 / 12.0;
                [400.0 + 200.0 * a.sin(), -300.0 + 200.0 * (1.0 - a.cos())]
            })
            .collect();
        layout.banking = vec![crate::layout::Line {
            kind: "banking".to_string(),
            name: Some("Oval".to_string()),
            line: arc,
            closed: false,
            bridge: false,
            tunnel: false,
        }];
        let mut scene = scene(&track);
        dress_scene(&track, &mut scene, &layout).unwrap();
        let mods: Vec<_> = scene
            .props
            .iter()
            .filter(|p| p.asset == "banked_oval_10m")
            .collect();
        assert!(mods.len() >= 20, "{} modules", mods.len());
        // The arc turns left (centre at +y), so its outside is on the
        // right: the module's +Y must point away from the centre.
        let centre = (400.0f32, -100.0f32);
        for m in &mods {
            let plus_y = (-m.yaw_rad.sin(), m.yaw_rad.cos());
            let out = (m.x - centre.0, m.y - centre.1);
            assert!(
                plus_y.0 * out.0 + plus_y.1 * out.1 > 0.0,
                "a module's high side faces the centre"
            );
        }
    }

    /// A general-admission area is laid as loose knots of standing
    /// spectators clear of the road, not as a raked bank of seats.
    #[test]
    fn a_general_admission_area_is_people_standing_about() {
        let track = track();
        let mut layout = layout();
        let mut ga = stand("GA1", vec![[200.0, -14.0], [400.0, -14.0]], 11.4, false);
        ga.station_m = 300.0;
        ga.family = Some("bank".to_string());
        layout.stands = vec![ga];
        let mut scene = scene(&track);
        dress_scene(&track, &mut scene, &layout).unwrap();
        let laid: Vec<_> = scene
            .props
            .iter()
            .filter(|p| p.source.as_deref() == Some("stand GA1"))
            .collect();
        assert!(laid.len() >= 20, "{} groups", laid.len());
        assert!(laid
            .iter()
            .all(|p| p.kind == PropKind::Misc && p.asset == "crowd_group"));
        let path = CenterlinePath::from_track(&track).unwrap();
        for p in &laid {
            let lat = nearest_cross_section(&path, p.x, p.y).1;
            assert!(
                lat < -(6.0 + BANK_ROAD_CLEAR_M) + 0.5,
                "a group at lateral {lat}"
            );
        }
    }

    /// A stand whose dossier station is the main straight but whose bays
    /// stand against the back straight is laid and flagged (R17: Sepang's
    /// mis-sided main stand); one beside its own straight is not.
    #[test]
    fn a_stand_laid_against_another_leg_is_flagged() {
        let track = track();
        let mut layout = layout();
        let mut wrong = stand("Main", vec![[240.0, 122.0], [360.0, 122.0]], 15.0, false);
        wrong.station_m = 300.0;
        wrong.side = Side::Left;
        let mut right = stand("Main", vec![[240.0, -20.0], [360.0, -20.0]], 15.0, false);
        right.station_m = 300.0;
        layout.stands = vec![wrong];
        let report = dress_scene(&track, &mut scene(&track), &layout).unwrap();
        assert_eq!(report.stands, 1, "{:?}", report.skipped);
        assert!(
            report
                .checks
                .iter()
                .any(|c| c.contains("far side of a leg")),
            "{:?}",
            report.checks
        );
        layout.stands = vec![right];
        let report = dress_scene(&track, &mut scene(&track), &layout).unwrap();
        assert_eq!(report.stands, 1, "{:?}", report.skipped);
        assert!(report.checks.is_empty(), "{:?}", report.checks);
    }

    #[test]
    fn a_stand_is_laid_as_runs_along_the_front_it_really_has() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        // 120 m of seats down the main straight, 20 m off the road.
        layout.stands.push(stand(
            "Main",
            vec![[100.0, -20.0], [220.0, -20.0]],
            26.0,
            true,
        ));
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(report.stands, 1);
        let stands: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Grandstand)
            .collect();
        assert_eq!(stands.len(), 3, "120 m of stand at a 40 m run");
        let total: f32 = stands.iter().map(|p| p.length_m.unwrap()).sum();
        assert!((total - 120.0).abs() < 0.5, "laid {total} m of stand");
        for p in &stands {
            // Deep and covered: the tall roofed family.
            assert_eq!(p.asset, "bay_10m_large_roof");
            // On the front line, running along it.
            assert!((p.y + 20.0).abs() < 0.01, "off the front line at {}", p.y);
            assert!(p.yaw_rad.abs() < 1e-3, "yaw {}", p.yaw_rad);
            assert!((100.0..=220.0).contains(&p.x));
        }
    }

    #[test]
    fn a_curved_front_is_followed_rather_than_cut_across() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        // Seats wrapped round the hairpin at 90 m radius.
        let front: Vec<[f32; 2]> = (-60..=60)
            .step_by(10)
            .map(|deg| {
                let (sin, cos) = (deg as f32).to_radians().sin_cos();
                [800.0 + 90.0 * cos, 70.0 + 90.0 * sin]
            })
            .collect();
        layout.stands.push(stand("Hairpin", front, 12.0, false));
        dress_scene(&track, &mut scene, &layout).unwrap();
        let runs: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Grandstand)
            .collect();
        assert!(runs.len() >= 3, "{} runs", runs.len());
        // Every run sits on the arc rather than on a chord across it.
        for p in &runs {
            let r = (p.x - 800.0).hypot(p.y - 70.0);
            assert!((r - 90.0).abs() < 6.0, "run {r} m from the hairpin centre");
        }
        // And they turn with it.
        let spread = runs
            .windows(2)
            .map(|w| yaw_gap(w[0].yaw_rad, w[1].yaw_rad))
            .fold(0.0f32, f32::max);
        assert!(spread > 0.1, "the runs all point the same way");
    }

    fn yaw_gap(a: f32, b: f32) -> f32 {
        let d = (b - a).rem_euclid(std::f32::consts::TAU);
        d.min(std::f32::consts::TAU - d)
    }

    #[test]
    fn the_family_follows_the_real_stand_size() {
        let mut s = stand("x", vec![[0.0, 0.0], [40.0, 0.0]], 8.0, false);
        assert_eq!(stand_family(&s), "bay_10m");
        s.covered = true;
        assert_eq!(stand_family(&s), "bay_10m_roof");
        s.covered = false;
        s.depth_m = 18.0;
        assert_eq!(stand_family(&s), "bay_10m_large");
        s.depth_m = 30.0;
        assert_eq!(
            stand_family(&s),
            "bay_10m_large_roof",
            "too deep to be open"
        );
    }

    /// A named family is the module the dossier asks for, whatever the
    /// stand's depth or roof (R2, R13), and every one is in the kit.
    #[test]
    fn a_named_family_lays_its_module() {
        for (family, asset) in [
            ("bank", "grass_bank_10m"),
            ("scaffold", "scaffold_10m"),
            ("banking_seats", "banking_seats"),
            ("tent", "tent_roof_10m"),
            ("palm", "palm_canopy_10m"),
            ("lotus", "lotus_canopy_10m"),
            ("stone", "stone_tribune_10m"),
            ("stone_centre", "stone_tribune_centre_10m"),
            ("stadium", "bay_10m_stadium_roof"),
        ] {
            let mut s = stand("x", vec![[0.0, 0.0], [40.0, 0.0]], 30.0, true);
            s.family = Some(family.to_string());
            assert_eq!(stand_family(&s), asset, "{family}");
            assert!(
                props::find(PropKind::Grandstand, asset).is_some(),
                "{asset} is in the kit"
            );
        }
    }

    #[test]
    fn the_seating_maps_roof_beats_the_depth_rule() {
        let mut s = stand("x", vec![[0.0, 0.0], [40.0, 0.0]], 30.0, false);
        s.roof = Some("open".to_string());
        assert_eq!(stand_family(&s), "bay_10m_large", "an open terrace");
        s.depth_m = 8.0;
        s.roof = Some("covered".to_string());
        assert_eq!(stand_family(&s), "bay_10m_roof");
        s.covered = true;
        s.roof = Some("open".to_string());
        assert_eq!(stand_family(&s), "bay_10m", "over the tags too");
    }

    /// A screen the dossier puts on the pit building stands on its roof,
    /// over the garages (R20).
    #[test]
    fn a_screen_over_the_garages_goes_on_the_roof() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.pit_lane = Some(PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            podium_m: None,
        });
        let screen = |x: f32, y: f32| Landmark {
            kind: "screen".to_string(),
            name: Some(format!("screen {y}")),
            station_m: x,
            side: Side::Right,
            centre: [x, y],
            brand: None,
            yaw_rad: None,
            altitude_m: None,
            pit_roof: true,
        };
        layout.landmarks = vec![screen(250.0, -33.0)];
        dress_scene(&track, &mut scene, &layout).unwrap();
        let screens: Vec<_> = scene
            .props
            .iter()
            .filter(|p| p.asset == "video_screen")
            .collect();
        assert_eq!(screens.len(), 1);
        assert!(on_the_pit_roof(screens[0]));
        let garage = props::resolve(PropKind::Pit, "garage_6m").unwrap().height_m;
        assert!(
            screens[0].z >= garage - 0.5,
            "on the roof: z {}",
            screens[0].z
        );
        let before = screens[0].clone();
        crate::groom::groom_props(&track, &mut scene).unwrap();
        let after = scene
            .props
            .iter()
            .find(|p| p.asset == "video_screen")
            .unwrap();
        assert_eq!(
            (after.x, after.y, after.z),
            (before.x, before.y, before.z),
            "grooming leaves it on the roof"
        );
    }

    #[test]
    fn the_real_pit_lane_replaces_the_generated_one_and_is_left_alone() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.pit_lane = Some(PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            podium_m: None,
        });
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert!(report.pit_lane);
        let pit = scene.pit_lane.clone().expect("lane");
        assert!(
            pit.authored,
            "the real lane is a fact, not a generated shape"
        );
        assert!(pit.box_count >= 8);
        // It leaves and rejoins the road, and keeps an apron off it
        // through its middle.
        let path = CenterlinePath::from_track(&track).unwrap();
        let lat_of = |n: &[f32; 3]| {
            let (sample, lat) = nearest_cross_section(&path, n[0], n[1]);
            (lat, sample.width_right_m)
        };
        for end in [pit.nodes.first().unwrap(), pit.nodes.last().unwrap()] {
            let (lat, half) = lat_of(end);
            assert!(
                lat < 0.0 && -lat < half,
                "an end on the road: {lat} of {half}"
            );
        }
        let (lat, half) = lat_of(&pit.nodes[pit.nodes.len() / 2]);
        assert!(
            -lat >= half + PIT_APRON_M + PIT_WIDTH_M / 2.0 - 0.1,
            "the middle clear of the road: {lat}"
        );

        // Grooming must not replace it.
        let before = scene.pit_lane.clone();
        crate::groom::groom_scene_with(&track, &mut scene, Some(&layout)).unwrap();
        assert_eq!(scene.pit_lane, before);
    }

    #[test]
    fn a_lane_with_room_gets_a_full_grids_boxes() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.pit_lane = Some(PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            podium_m: None,
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(
            scene.pit_lane.expect("lane").box_count,
            PIT_GRID_BOXES,
            "every car its own garage"
        );
    }

    #[test]
    fn a_stand_behind_the_pit_entry_is_laid_and_one_over_the_garages_is_not() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.pit_lane = Some(PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            podium_m: None,
        });
        // 13 m beyond the lane's centre: behind the lane where it leaves
        // the road, and over the garages where they stand, mid-lane.
        layout.stands.push(stand(
            "Entry",
            vec![[105.0, -35.0], [145.0, -35.0]],
            10.0,
            false,
        ));
        layout.stands.push(stand(
            "Terrace",
            vec![[220.0, -35.0], [280.0, -35.0]],
            10.0,
            false,
        ));
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        let laid = |name: &str| {
            scene
                .props
                .iter()
                .filter(|p| p.source.as_deref() == Some(&format!("stand {name}")))
                .count()
        };
        assert!(laid("Entry") > 0, "{:?}", report.skipped);
        assert_eq!(laid("Terrace"), 0);
        assert!(
            report
                .skipped
                .iter()
                .any(|s| s.starts_with("stand Terrace: over the garage row")),
            "{:?}",
            report.skipped
        );
    }

    #[test]
    fn buildings_in_the_pit_complex_are_left_to_the_bake() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.pit_lane = Some(PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            podium_m: None,
        });
        layout.structures.push(Structure {
            name: Some("Pit building".to_string()),
            station_m: 200.0,
            side: Side::Right,
            offset_m: 30.0,
            length_m: 120.0,
            depth_m: 20.0,
            yaw_rad: 0.0,
            centre: [200.0, -40.0],
            levels: 2,
            area_m2: 2400.0,
            osm_building: Some("yes".to_string()),
            asset: None,
        });
        layout.structures.push(Structure {
            name: Some("Clubhouse".to_string()),
            station_m: 600.0,
            side: Side::Left,
            offset_m: 40.0,
            length_m: 26.0,
            depth_m: 18.0,
            yaw_rad: 0.0,
            centre: [600.0, 180.0],
            levels: 1,
            area_m2: 470.0,
            osm_building: Some("yes".to_string()),
            asset: None,
        });
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        let buildings: Vec<&String> = report
            .skipped
            .iter()
            .filter(|s| s.starts_with("building"))
            .collect();
        assert_eq!(buildings.len(), 1, "{:?}", report.skipped);
        assert!(buildings[0].contains("pit complex"));
        let built: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Building)
            .collect();
        assert_eq!(built.len(), 1);
        assert_eq!(built[0].asset, "clubhouse");
    }

    #[test]
    fn dressing_twice_changes_nothing() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.stands.push(stand(
            "Main",
            vec![[100.0, -20.0], [220.0, -20.0]],
            26.0,
            true,
        ));
        layout.landmarks.push(Landmark {
            kind: "big_wheel".to_string(),
            name: None,
            station_m: 400.0,
            side: Side::Right,
            centre: [400.0, -80.0],
            brand: None,
            yaw_rad: None,
            altitude_m: None,
            pit_roof: false,
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        let once = scene.clone();
        dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(once, scene, "a second dressing moved something");
    }

    #[test]
    fn a_car_park_is_filled_and_lit() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        // A 100 m square car park well clear of the road.
        layout.areas.push(crate::layout::Area {
            kind: "parking".to_string(),
            name: Some("P1".to_string()),
            ring: vec![
                [200.0, -120.0],
                [300.0, -120.0],
                [300.0, -220.0],
                [200.0, -220.0],
                [200.0, -120.0],
            ],
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        let cars = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Vehicle)
            .count();
        let lamps = scene
            .props
            .iter()
            .filter(|p| p.asset == "lamp_post")
            .count();
        assert!(cars > 20, "only {cars} cars in a 10 000 m2 car park");
        assert!(lamps >= 4, "only {lamps} lamps around the car park");
        for prop in scene.props.iter().filter(|p| p.kind == PropKind::Vehicle) {
            assert!(
                (200.0..=300.0).contains(&prop.x) && (-220.0..=-120.0).contains(&prop.y),
                "a car parked outside the car park at {},{}",
                prop.x,
                prop.y
            );
        }
    }

    #[test]
    fn curbs_get_marker_posts_and_tight_inside_curbs_sausage_kerbs() {
        let track = track();
        let mut scene = scene(&track);
        let layout = layout();
        // The fixture's first bend (station 800 on) turns left at 70 m: a
        // curb on its inside (left) gets sausage kerbs, one on the straight
        // (station 100-140, right) only its markers.
        for (id, side, start, end) in [
            (1, Side::Left, 820.0, 860.0),
            (2, Side::Right, 100.0, 140.0),
        ] {
            scene.curbs.push(crate::ats::Curb {
                id,
                side,
                start_m: start,
                end_m: end,
                width_m: 1.0,
                style: "red_white".to_string(),
            });
        }
        dress_scene(&track, &mut scene, &layout).unwrap();
        let markers = scene
            .props
            .iter()
            .filter(|p| p.asset == "kerb_marker")
            .count();
        let sausages: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.asset == "sausage_kerb_2m")
            .collect();
        assert_eq!(markers, 4, "a marker post at each end of each curb");
        assert_eq!(
            sausages.len(),
            20,
            "20 pieces over 40 m behind the inside curb: {}",
            sausages.len()
        );
        assert!(
            sausages
                .iter()
                .all(|p| p.kind == PropKind::Barrier && p.x > 790.0),
            "sausage kerbs only at the bend"
        );
        let before = scene.props.len();
        dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(scene.props.len(), before, "a re-dress lays the same pieces");
    }

    #[test]
    fn every_named_corner_gets_a_board_and_a_marshal() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        // The board shows the display name, never the real one, and a corner
        // with none (a sponsor's, the circuit's own name) gets no board.
        for (name, display, station) in [
            ("Turn One", Some("First Bend"), 120.0),
            ("Acme Oil Kurve", None, 300.0),
            ("The Sweeper", Some("The Sweeper"), 480.0),
        ] {
            layout.corners.push(crate::layout::Corner {
                name: name.to_string(),
                display_name: display.map(str::to_string),
                station_m: station,
                from_m: station - 20.0,
                to_m: station + 20.0,
            });
        }
        dress_scene(&track, &mut scene, &layout).unwrap();
        let boards: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.asset == "corner_sign")
            .collect();
        assert_eq!(boards.len(), 2, "corner boards: {boards:?}");
        let mut names: Vec<&str> = boards
            .iter()
            .map(|b| b.text.as_deref().unwrap_or(""))
            .collect();
        names.sort_unstable();
        assert_eq!(names, ["First Bend", "The Sweeper"]);
        assert!(
            scene.props.iter().any(|p| p.asset == "marshal_post"),
            "no marshal posts"
        );
    }

    #[test]
    fn the_fan_lap_numbers_its_posts_banks_its_car_parks_and_builds_camp_villages() {
        let track = track();
        // The Nordschleife's style is the fan dressing.
        let mut scene = AtsScene::new_for_track(&track, "Mordschleife.yaml");
        let mut layout = layout();
        let square = |x0: f32, y0: f32, x1: f32, y1: f32| {
            vec![[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]]
        };
        // A car park 20 m right of the first straight, a paddock beside it,
        // and a camp site well back.
        for (kind, name, ring) in [
            ("parking", "Spectators", square(300.0, -26.0, 400.0, -70.0)),
            ("parking", "D10", square(620.0, -26.0, 720.0, -70.0)),
            (
                "parking",
                "Historisches Fahrerlager",
                square(500.0, -26.0, 600.0, -70.0),
            ),
            ("camp_site", "Camp", square(200.0, -150.0, 400.0, -300.0)),
        ] {
            layout.areas.push(crate::layout::Area {
                kind: kind.to_string(),
                name: Some(name.to_string()),
                ring,
            });
        }
        dress_scene(&track, &mut scene, &layout).unwrap();

        let posts: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Sign && p.asset.starts_with("marshal_post"))
            .collect();
        assert!(!posts.is_empty());
        assert!(posts.iter().all(|p| p.asset == "marshal_post_numbered"));
        let mut numbers: Vec<u32> = posts
            .iter()
            .map(|p| {
                p.text
                    .as_deref()
                    .and_then(|t| t.strip_prefix("post"))
                    .unwrap()
                    .parse()
                    .unwrap()
            })
            .collect();
        numbers.sort_unstable();
        assert_eq!(numbers, (1..=posts.len() as u32).collect::<Vec<_>>());

        let banks: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.asset == "spectator_bank_10m")
            .collect();
        assert!(
            banks.len() >= 6,
            "{} banks beside a 100 m car park",
            banks.len()
        );
        for bank in &banks {
            assert!(
                bank.y < -10.0 && bank.y > -20.0,
                "a bank at {},{}",
                bank.x,
                bank.y
            );
            assert!(
                (290.0..=410.0).contains(&bank.x),
                "a bank by the paddock at {}",
                bank.x
            );
        }

        let village: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.asset.starts_with("camp_"))
            .collect();
        assert!(!village.is_empty(), "no camp village");
        for piece in &village {
            assert!((200.0..=400.0).contains(&piece.x) && (-300.0..=-150.0).contains(&piece.y));
            if piece.asset == "camp_flag_pole" {
                assert!(surroundings::FAN_FLAGS.contains(&piece.text.as_deref().unwrap_or("")));
            }
        }

        // A circuit without the fan dressing keeps its plain posts.
        let mut plain = make_scene(&track);
        dress_scene(&track, &mut plain, &layout).unwrap();
        assert!(plain
            .props
            .iter()
            .any(|p| p.asset == "marshal_post" && p.text.is_none()));
        assert!(!plain
            .props
            .iter()
            .any(|p| p.asset == "spectator_bank_10m" || p.asset.starts_with("camp_")));

        // And it is all owned: a second dress lays the same.
        let once = scene.props.clone();
        dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(once, scene.props);
    }

    #[test]
    fn the_lap_over_a_road_has_parapets_and_keeps_its_ground() {
        let track = track();
        let path = CenterlinePath::from_track(&track).unwrap();
        let crossing = Crossing {
            name: Some("B 257".to_string()),
            station_m: 230.0,
            kind: "deck_track".to_string(),
            brand: None,
            from_m: Some(200.0),
            to_m: Some(260.0),
        };
        let tiles = lay_crossing(&path, &crossing);
        assert_eq!(tiles.len(), 6);
        assert!(tiles.iter().all(|p| p.asset == "track_bridge_10m"));
        // Not a bridge span: the ground under it stays.
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.crossings.push(crossing.clone());
        lay_water_and_bridges(&mut scene, &layout);
        assert!(scene.bridges.is_empty());
        // The gantry and a forest track's bridge are one prop each.
        for (kind, asset) in [
            ("timing", "timing_gantry"),
            ("overpass", "overpass_concrete"),
            // R4: a public road over the lap is a concrete bridge.
            ("road", "overpass_concrete"),
            ("building", "span_building"),
            ("sky_platform", "sky_platform"),
            ("banked_oval", "banked_oval_span"),
            ("arch", "tyre_bridge"),
        ] {
            let one = lay_crossing(
                &path,
                &Crossing {
                    kind: kind.to_string(),
                    from_m: None,
                    to_m: None,
                    ..crossing.clone()
                },
            );
            assert_eq!(one.len(), 1);
            assert_eq!(one[0].asset, asset);
        }
    }

    #[test]
    fn the_nordschleife_landmarks_are_laid_once_and_owned() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        for (kind, centre) in [
            ("lookout_tower", [400.0, -900.0]),
            ("toll_gate", [300.0, -30.0]),
            ("hillside_letters", [500.0, -250.0]),
        ] {
            layout.landmarks.push(Landmark {
                kind: kind.to_string(),
                name: None,
                station_m: 0.0,
                side: Side::Right,
                centre,
                brand: None,
                yaw_rad: Some(0.3),
                altitude_m: None,
                pit_roof: false,
            });
        }
        dress_scene(&track, &mut scene, &layout).unwrap();
        dress_scene(&track, &mut scene, &layout).unwrap();
        for asset in ["stone_lookout_tower", "toll_gate_lane", "hillside_letters"] {
            let laid: Vec<&Prop> = scene.props.iter().filter(|p| p.asset == asset).collect();
            assert_eq!(laid.len(), 1, "{asset}");
            assert!(dressed_prop(laid[0]), "{asset}");
        }
    }

    #[test]
    fn a_circuit_with_no_lighting_of_its_own_is_given_some() {
        // The Red Bull Ring is not floodlit and OSM maps no masts there,
        // so a night session was lit by headlights and a one lux moon.
        let track = track();
        let mut scene = scene(&track);
        let layout = layout();
        dress_scene(&track, &mut scene, &layout).unwrap();
        let masts = scene
            .props
            .iter()
            .filter(|p| p.asset == "floodlight_tower")
            .count();
        assert!(masts >= 2, "only {masts} floodlight masts");

        // A circuit that has its own keeps exactly those and gets no
        // generated set on top.
        let mut lit_scene = make_scene(&track);
        let mut lit = make_layout();
        lit.landmarks.push(Landmark {
            kind: "floodlight".to_string(),
            name: None,
            station_m: 100.0,
            side: Side::Left,
            centre: [100.0, 40.0],
            brand: None,
            yaw_rad: None,
            altitude_m: None,
            pit_roof: false,
        });
        dress_scene(&track, &mut lit_scene, &lit).unwrap();
        let masts = lit_scene
            .props
            .iter()
            .filter(|p| p.asset == "floodlight_tower")
            .count();
        assert_eq!(masts, 1, "a lit circuit was given extra masts");
    }

    #[test]
    fn nothing_in_the_surroundings_lands_on_the_road() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        // A car park drawn straight across the course, which is the kind
        // of thing an OSM polygon does near a street circuit.
        layout.areas.push(crate::layout::Area {
            kind: "parking".to_string(),
            name: None,
            ring: vec![
                [0.0, -60.0],
                [200.0, -60.0],
                [200.0, 60.0],
                [0.0, 60.0],
                [0.0, -60.0],
            ],
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        for prop in &scene.props {
            if !surroundings::owns(prop) {
                continue;
            }
            let (sample, lat) = nearest_cross_section(&path, prop.x, prop.y);
            let edge = side_half_width(&sample, if lat >= 0.0 { Side::Left } else { Side::Right });
            assert!(
                lat.abs() > edge,
                "{} sits on the road at lateral {lat}",
                prop.asset
            );
        }
    }

    #[test]
    fn a_statue_landmark_is_laid_once_and_owned() {
        let track = track();
        let mut scene = scene(&track);
        // A hand-placed misc prop the pass must leave alone.
        scene.props.push(Prop {
            id: 4242,
            kind: PropKind::Misc,
            asset: "bollard".to_string(),
            x: 50.0,
            y: -30.0,
            z: 0.0,
            yaw_rad: 0.0,
            scale: 1.0,
            text: None,
            length_m: None,
            source: None,
        });
        let mut layout = layout();
        layout.landmarks.push(Landmark {
            kind: "statue".to_string(),
            name: Some("Red Bull Statue".to_string()),
            station_m: 400.0,
            side: Side::Right,
            centre: [400.0, -90.0],
            brand: None,
            yaw_rad: None,
            altitude_m: None,
            pit_roof: false,
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        dress_scene(&track, &mut scene, &layout).unwrap();
        let statues: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.asset == "bull_statue")
            .collect();
        assert_eq!(statues.len(), 1, "re-dressing stacked another statue");
        assert_eq!(statues[0].kind, PropKind::Misc);
        assert!(dressed_prop(statues[0]));
        let bollards = scene.props.iter().filter(|p| p.asset == "bollard").count();
        assert_eq!(bollards, 1, "the hand-placed misc prop was swept");
        let placed = (statues[0].x, statues[0].y, statues[0].yaw_rad);
        crate::groom::groom_scene_with(&track, &mut scene, Some(&layout)).unwrap();
        let after = scene
            .props
            .iter()
            .find(|p| p.asset == "bull_statue")
            .unwrap();
        assert_eq!(
            placed,
            (after.x, after.y, after.yaw_rad),
            "grooming pushed the statue"
        );
    }

    #[test]
    fn a_dressed_stand_is_never_pushed_by_grooming() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.stands.push(stand(
            "Main",
            vec![[100.0, -20.0], [220.0, -20.0]],
            26.0,
            true,
        ));
        dress_scene(&track, &mut scene, &layout).unwrap();
        let placed: Vec<(f32, f32, f32)> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Grandstand)
            .map(|p| (p.x, p.y, p.yaw_rad))
            .collect();
        crate::groom::groom_scene_with(&track, &mut scene, Some(&layout)).unwrap();
        let after: Vec<(f32, f32, f32)> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Grandstand)
            .map(|p| (p.x, p.y, p.yaw_rad))
            .collect();
        assert_eq!(placed, after);
    }

    #[test]
    fn tree_belts_stay_inside_the_real_woods() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        // One wood beside the main straight, nothing anywhere else.
        layout.woods.push(Wood {
            leaf: "needleleaved".to_string(),
            ring: vec![
                [200.0, -120.0],
                [500.0, -120.0],
                [500.0, -30.0],
                [200.0, -30.0],
                [200.0, -120.0],
            ],
        });
        dress_scene(&track, &mut scene, &layout).unwrap();
        crate::groom::groom_scene_with(&track, &mut scene, Some(&layout)).unwrap();
        // Ground cover follows the verge, not the woodland, so the wood
        // rule is about trees only.
        let trees: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Tree && !crate::groom::is_ground_cover(&p.asset))
            .collect();
        assert!(!trees.is_empty(), "no trees planted in the wood");
        for t in &trees {
            assert!(
                layout.woods[0].contains(t.x, t.y),
                "tree at ({}, {}) is outside the wood",
                t.x,
                t.y
            );
            assert!(
                ["conifer_m", "conifer_l", "poplar", "conifer_m_near"].contains(&t.asset.as_str()),
                "{} is not a conifer",
                t.asset
            );
        }
    }
}

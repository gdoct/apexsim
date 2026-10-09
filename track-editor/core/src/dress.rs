//! Dress a `.ats` scene from the circuit's real-world layout dossier.
//!
//! The shipped scenes were dressed by a procedural enrichment pass that
//! knew nothing about the actual venue: grandstands landed wherever there
//! was room, the pit lane took whichever side of the road the old data
//! guessed, and no circuit had its landmarks. This pass replaces all of
//! that with what is really there, as recorded in
//! `content/tracks/default/<Stem>.layout.json` (see [`crate::layout`]).
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

use crate::ats::{AtsScene, Decal, PitLane, Prop, PropKind, Side};
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
/// Seating mapped this close to the pit lane's centerline is the terrace
/// over the garages, and the bake already builds that whole complex from
/// the lane; a stand laid there would stand in the working lane or in the
/// garages behind it, which reach 14 m back from a 12 m lane.
const PIT_STAND_RANGE_M: f32 = 20.0;
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
/// Least room between the road edge and any part of a stand bay.
const STAND_ROAD_CLEAR_M: f32 = 3.0;
/// A building with a footprint at least this square and this many levels
/// is a tower.
const TOWER_LEVELS: u32 = 4;

/// Pit boxes are one garage module apart (`ApexTrackAssetBuilder`'s 6 m
/// pitch); the count is what the dossier's lane has room for.
const PIT_BOX_PITCH_M: f32 = 6.0;
const PIT_BOXES: std::ops::RangeInclusive<u32> = 8..=40;
const PIT_WIDTH_M: f32 = 12.0;
const PIT_SPEED_LIMIT_KMH: f32 = 80.0;
/// Boxes a lane gets when it has room, whatever a survey counted: every
/// car of a full grid (the create screen's 20) its own garage, and spares.
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
    /// Road graffiti laid from the dossier as `.ats` decals.
    pub graffiti: usize,
    /// Dossier entries that could not be placed, with the reason.
    pub skipped: Vec<String>,
}

impl DressReport {
    pub fn placed(&self) -> usize {
        self.stand_props + self.buildings + self.bridges + self.landmarks + self.surroundings
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

/// The `misc` assets [`lay_landmark`] can produce. `misc` as a kind is not
/// owned by this pass — bollards, kerb markers and generators are placed
/// by hand — but a landmark laid from the dossier is, or every re-dress
/// would stack another statue on the same spot and grooming would treat
/// the surveyed position as a guess it may push.
const LANDMARK_MISC_ASSETS: [&str; 1] = ["bull_statue"];

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
    surroundings::owns(prop)
        || (prop.kind == PropKind::Misc && LANDMARK_MISC_ASSETS.contains(&prop.asset.as_str()))
}

pub fn dressed_prop(prop: &Prop) -> bool {
    dressed_kind(prop.kind)
        || (prop.kind == PropKind::Misc && LANDMARK_MISC_ASSETS.contains(&prop.asset.as_str()))
        || surroundings::owns(prop)
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
    let terrain = crate::groom::seating_terrain(&path, scene, dem)?;
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

    let mut laid: Vec<Prop> = Vec::new();
    for stand in &layout.stands {
        let props = lay_stand(&path, &terrain, lane.as_ref(), stand);
        if props.is_empty() {
            let why = if stand.length_m < MIN_STAND_M {
                "too small to be seating"
            } else {
                "nothing to build clear of the pit lane"
            };
            report
                .skipped
                .push(format!("stand {}: {why}", name_of(&stand.name)));
            continue;
        }
        report.stands += 1;
        report.stand_props += props.len();
        laid.extend(props);
    }
    let city = crate::circuit_style::CircuitStyle::for_scene(scene).city_buildings;
    for structure in &layout.structures {
        match lay_structure(&path, &terrain, lane.as_ref(), structure, city) {
            Ok(props) => {
                report.buildings += props.len();
                laid.extend(props);
            }
            Err(reason) => report
                .skipped
                .push(format!("building {}: {reason}", name_of(&structure.name))),
        }
    }
    for crossing in &layout.crossings {
        for prop in lay_crossing(&path, crossing) {
            report.bridges += 1;
            laid.push(prop);
        }
    }
    if crate::circuit_style::CircuitStyle::for_scene(scene).city_buildings {
        laid.extend(lay_street_lamps(&path, &terrain, &scene.bridges));
    }
    for landmark in &layout.landmarks {
        match lay_landmark(&path, &terrain, landmark) {
            Ok(Some(prop)) => {
                report.landmarks += 1;
                laid.push(prop);
            }
            Ok(None) => {}
            Err(reason) => report
                .skipped
                .push(format!("landmark {}: {reason}", name_of(&landmark.name))),
        }
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
    );
    report.surroundings = extras.len();
    laid.extend(extras);

    // Nothing dressed stands in the water. A bridge, a sky prop and a
    // landmark that is meant to stand over it (the double helix) keep
    // their place.
    if terrain.has_water() {
        let before = laid.len();
        laid.retain(|p| {
            matches!(p.kind, PropKind::Bridge | PropKind::Sky)
                || p.asset == "bridge_double_helix"
                || !terrain.in_water(p.x, p.y)
        });
        if laid.len() < before {
            report
                .skipped
                .push(format!("{} props: in the water", before - laid.len()));
        }
    }

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
        let before = laid.len();
        laid.retain(|p| !in_a_block(p));
        if laid.len() < before {
            report
                .skipped
                .push(format!("{} props: inside a building", before - laid.len()));
        }
    }

    // Nothing dressed stands in the pit lane or where its garages go.
    let zone = scene
        .pit_lane
        .as_ref()
        .zip(layout.pit_lane.as_ref())
        .and_then(|(pit, road)| PitZone::new(pit, road.side));
    if let Some(zone) = zone {
        let before = laid.len();
        laid.retain(|p| {
            matches!(p.kind, PropKind::Bridge | PropKind::Sky) || !zone.blocks(&path, p)
        });
        if laid.len() < before {
            report.skipped.push(format!(
                "{} props: in the pit lane or where its garages stand",
                before - laid.len()
            ));
        }
    }

    // The authored furniture of a circuit's own straights. It is laid
    // after the pit-zone test on purpose: its lamps and marquees stand
    // behind the garages, which that test keeps clear.
    for prop in lay_furniture(&path, &terrain, &layout.furniture) {
        if in_a_block(&prop) {
            continue;
        }
        report.landmarks += 1;
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
    ("Spa", "red_yellow"),
    ("YasMarina", "blue_white"),
    ("Sakhir", "blue_red"),
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
    let mut station: Option<f32> = None;
    let frame: Vec<(PathSample, f32)> = mapped
        .iter()
        .enumerate()
        .map(|(i, p)| {
            let found = match station {
                _ if i >= exit_from => nearest_cross_section(path, p[0], p[1]),
                Some(s) => nearest_cross_section_near(
                    path,
                    p[0],
                    p[1],
                    s + PIT_LANE_STEP_M,
                    PIT_LEG_WINDOW_M,
                ),
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
    let nodes: Vec<[f32; 3]> = nodes
        .iter()
        .zip(&frame)
        .map(|(n, (mapped_at, _))| {
            let (sample, lat) =
                nearest_cross_section_near(path, n[0], n[1], mapped_at.station_m, PIT_LEG_WINDOW_M);
            let on_road = lat.clamp(-sample.width_right_m, sample.width_left_m);
            [n[0], n[1], offset_point(&sample, on_road).2]
        })
        .collect();

    // The garages line the stretch between the tapers.
    let clear = (length - 2.0 * taper).max(0.0);
    let room = (clear / PIT_BOX_PITCH_M) as u32;
    // Every car its own garage: at least a full grid's worth where the
    // lane has room, a survey's count when it found more.
    let box_count = road
        .box_count
        .unwrap_or(0)
        .max(PIT_GRID_BOXES)
        .min(room)
        .clamp(*PIT_BOXES.start(), *PIT_BOXES.end());
    Some(PitLane {
        nodes,
        width_m: PIT_WIDTH_M,
        box_count,
        speed_limit_kmh: PIT_SPEED_LIMIT_KMH,
        authored: true,
    })
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
}

/// The zone reaches this far past the lane's road-side edge (the apron
/// and the pit wall)…
const PIT_ZONE_APRON_M: f32 = 2.5;
/// …and this far past its garage-side edge (a garage's depth and a
/// little).
const PIT_ZONE_GARAGES_M: f32 = 17.0;

impl PitZone {
    fn new(pit: &PitLane, side: Side) -> Option<Self> {
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
        })
    }

    fn contains(&self, x: f32, y: f32) -> bool {
        let (mut best, mut lat) = (f32::MAX, 0.0f32);
        let mut at_end = false;
        let samples = self.lane.samples();
        for (i, sample) in samples.iter().enumerate() {
            let (dx, dy) = (x - sample.pos.0, y - sample.pos.1);
            let d2 = dx * dx + dy * dy;
            if d2 < best {
                best = d2;
                let (sin, cos) = sample.heading_rad.sin_cos();
                lat = -sin * dx + cos * dy;
                at_end = i == 0 || i == samples.len() - 1;
            }
        }
        // Beyond either end of the lane is not beside it.
        if at_end && best.sqrt() > lat.abs() + 1.0 {
            return false;
        }
        let toward_garages = lat * self.garage_side;
        if toward_garages >= 0.0 {
            toward_garages < self.half + PIT_ZONE_GARAGES_M
        } else {
            -toward_garages < self.half + PIT_ZONE_APRON_M
        }
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
/// too deep to be anything else).
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
    let large = stand.depth_m >= LARGE_STAND_DEPTH_M;
    let roof = stand.covered || stand.depth_m >= ASSUMED_ROOF_DEPTH_M;
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
fn lay_stand(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    lane: Option<&CenterlinePath>,
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
        if lane.is_some_and(|lane| lane_gap(lane, x, y) < PIT_STAND_RANGE_M) {
            continue;
        }
        let yaw = (b.1 - a.1).atan2(b.0 - a.0);
        // The front is the real one, but the kit bay is as deep as it is:
        // a front traced a few metres off the asphalt put the back rows of
        // a deep bay — or the front of one on the far side of a bend —
        // over the road. Step the run back until the whole bay is clear.
        let Some((x, y)) = clear_footprint(path, x, y, yaw, len, bay_depth, STAND_ROAD_CLEAR_M)
        else {
            continue;
        };
        let (sample, lat) = nearest_cross_section(path, x, y);
        out.push(Prop {
            id: 0,
            kind: PropKind::Grandstand,
            asset: asset.to_string(),
            x,
            y,
            z: seat_z(terrain, &sample, lat, x, y),
            yaw_rad: yaw,
            scale: 1.0,
            text: None,
            length_m: Some(round2(len)),
        });
        walked = to;
    }
    let _ = walked;
    out
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
fn building_asset(structure: &Structure) -> (&'static str, f32) {
    let square = structure.depth_m / structure.length_m.max(1.0);
    if structure.levels >= TOWER_LEVELS && square > 0.6 && structure.length_m < 30.0 {
        return ("control_tower", 13.2);
    }
    if structure.length_m >= 35.0 && structure.depth_m >= 12.0 {
        return ("media_centre", 40.6);
    }
    if structure.levels >= 2 || structure.area_m2 >= 700.0 {
        return ("hospitality_3f", 30.6);
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
    let (asset, unit_m) = building_asset(structure);
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

    // Move the row back until every unit's front is clear of the road.
    // The row's normal is not the road's where the building sits at an
    // angle to it, so the push is scaled by the cosine between them and
    // repeated; a row that runs across the road rather than beside it
    // cannot be pushed clear.
    let shortfall = |front: (f32, f32)| -> Result<f32, String> {
        let mut worst: f32 = 0.0;
        for i in 0..units {
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
    let footprint_gap = |front: (f32, f32)| -> f32 {
        let mut worst = f32::MAX;
        for i in 0..units {
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
        }
        worst
    };
    let mut pushed = 0.0f32;
    loop {
        let gap = footprint_gap((front_x, front_y));
        if gap >= BUILDING_ROAD_CLEAR_M - 0.05 {
            break;
        }
        // Stepping back from the near road can only help when the far
        // side has room; walk until it does, or give up.
        pushed += 1.0;
        if pushed > BUILDING_MAX_PUSH_M {
            return Err(format!(
                "no room for its footprint between the road sections ({gap:.1} m to the asphalt)"
            ));
        }
        front_x += away.0;
        front_y += away.1;
    }

    let mut out = Vec::new();
    for i in 0..units {
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
/// viaduct, a sign gantry). A `deck_*` crossing is the other way round: the
/// road runs *on* the bridge, over water, and the dossier gives the
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
        }
    };
    let deck = match crossing.kind.as_str() {
        "deck_arch" => Some("bridge_arch_steel"),
        "deck_wide" => Some("bridge_deck_wide"),
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

/// Twin-arm street lamps along both verges, none on a bridge (the deck has
/// its own) and none in the water.
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
        if bridges.iter().any(|b| b.covers(station, total)) {
            continue;
        }
        let sample = path.sample_at(station);
        let (side, sign) = if i % 2 == 0 {
            (Side::Left, 1.0)
        } else {
            (Side::Right, -1.0)
        };
        let lat = sign * (side_half_width(&sample, side) + STREET_LAMP_SETBACK_M);
        let (x, y, _) = offset_point(&sample, lat);
        if terrain.in_water(x, y) {
            continue;
        }
        out.push(Prop {
            id: 0,
            kind: PropKind::Light,
            asset: "lamp_arm_twin".to_string(),
            x,
            y,
            z: seat_z(terrain, &sample, lat, x, y),
            yaw_rad: sample.heading_rad,
            scale: 1.0,
            text: None,
            length_m: None,
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

/// The brands the kit has artwork for (`content/props/board/brands`).
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
        // Burg Nürburg: a castle on its hill, centred on its footprint.
        "castle" => (PropKind::Building, "castle_ruin"),
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
    /// A marshal post stands at every named corner, and on a straight
    /// this far apart.
    const MARSHAL_SPACING_M: f32 = 400.0;
    /// How far past the barrier line the marshals stand.
    const MARSHAL_BEYOND_BARRIER_M: f32 = 4.0;
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
    pub const OWNED: [&str; 31] = [
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
    ];

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
    fn roll(x: f32, y: f32, salt: u32) -> f32 {
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
    ) -> Vec<Prop> {
        let ctx = Ctx {
            path,
            terrain,
            layout,
            bare,
        };
        let mut out = Vec::new();
        car_parks(&ctx, &mut out);
        camp_sites(&ctx, &mut out);
        village(&ctx, &mut out);
        points_of_interest(&ctx, &mut out);
        forests(&ctx, dem, &mut out);
        marshal_posts(&ctx, &mut out);
        corner_signs(&ctx, &mut out);
        if night_lighting {
            floodlights(&ctx, &mut out);
        }
        out
    }

    /// Cars and lamp posts in every mapped car park.
    fn car_parks(ctx: &Ctx, out: &mut Vec<Prop>) {
        for area in ctx.layout.areas.iter().filter(|a| a.kind == "parking") {
            let cars =
                ((area.area_m2() / CAR_PARK_AREA_PER_CAR_M2) as usize).min(MAX_CARS_PER_PARK);
            if cars == 0 {
                continue;
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
    }

    /// Lamp posts along a car park's edge, at a fixed spacing so the
    /// lighting reads as laid out rather than scattered.
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
                if let Some(prop) = ctx.place(PropKind::Light, "lamp_post", x, y, None) {
                    out.push(prop);
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
            let perimeter: f32 = area
                .ring
                .windows(2)
                .map(|p| (p[1][0] - p[0][0]).hypot(p[1][1] - p[0][1]))
                .sum();
            let pitches = ((perimeter / 100.0) * CAMP_PITCHES_PER_100M) as usize;
            let (cx, cy) = area.centre();
            let span = area.area_m2().sqrt().max(20.0);
            let mut placed = 0;
            for i in 0..pitches * 4 {
                if placed >= pitches {
                    break;
                }
                let t = i as f32 * 0.618_034; // a low-discrepancy walk
                let x = cx + (t.fract() - 0.5) * span;
                let y = cy + ((t * 1.618).fract() - 0.5) * span;
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
                    out.push(Prop {
                        kind,
                        asset: asset.to_string(),
                        ..prop
                    });
                    placed += 1;
                }
            }
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
                if !(IMPOSTOR_NEAR_M..=IMPOSTOR_FAR_M).contains(&from_road) {
                    continue;
                }
                let z = dem.height_at(jx, jy);
                if z - sample.pos.2 < IMPOSTOR_MIN_RISE_M {
                    continue;
                }
                let reach = step;
                let dzdx =
                    (dem.height_at(jx + reach, jy) - dem.height_at(jx - reach, jy)) / (2.0 * reach);
                let dzdy =
                    (dem.height_at(jx, jy + reach) - dem.height_at(jx, jy - reach)) / (2.0 * reach);
                if dzdx.hypot(dzdy) < IMPOSTOR_MIN_SLOPE {
                    continue;
                }
                // Spruce on the steeper, higher ground and mixed below,
                // which is how a real tree line goes.
                let asset = if dzdx.hypot(dzdy) > IMPOSTOR_MIN_SLOPE * 2.0 {
                    "forest_impostor_conifer"
                } else {
                    "forest_impostor"
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

    /// A marshal post at every named corner and along the straights.
    fn marshal_posts(ctx: &Ctx, out: &mut Vec<Prop>) {
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
        for station in stations {
            let sample = ctx.path.sample_at(station);
            // Outside of the bend, which is where a marshal post stands.
            let side = if curvature_at(ctx.path, station) < 0.0 {
                Side::Left
            } else {
                Side::Right
            };
            let lat = signed_lat(&sample, side, MARSHAL_BEYOND_BARRIER_M + 8.0);
            let point = offset_point(&sample, lat);
            if let Some(prop) = ctx.place_clear(
                PropKind::Sign,
                "marshal_post",
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
        assert!(tiles.iter().all(|p| p.asset == "bridge_deck_wide" && p.kind == PropKind::Bridge));
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
            rings: vec![vec![[300.0, 30.0], [500.0, 30.0], [500.0, 110.0], [300.0, 110.0]]],
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
        let ids: Vec<u64> = scene.water.iter().map(|w| w.id).chain(scene.bridges.iter().map(|b| b.id)).collect();
        let again = {
            dress_scene(&track, &mut scene, &layout).unwrap();
            scene.water.iter().map(|w| w.id).chain(scene.bridges.iter().map(|b| b.id)).collect::<Vec<_>>()
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
            front,
            centre: [0.0, 0.0],
            yaw_rad: 0.0,
            family: None,
        }
    }

    fn scene(track: &TrackFile) -> AtsScene {
        AtsScene::new_for_track(track, "Dress.yaml")
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

    #[test]
    fn the_real_pit_lane_replaces_the_generated_one_and_is_left_alone() {
        let track = track();
        let mut scene = scene(&track);
        let mut layout = layout();
        layout.pit_lane = Some(PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            box_count: None,
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
    fn a_lane_gets_a_full_grids_boxes_or_a_surveys_within_its_room() {
        let track = track();
        let lane = |count| PitRoad {
            side: Side::Right,
            length_m: 300.0,
            nodes: (0..=15).map(|i| [100.0 + i as f32 * 20.0, -22.0]).collect(),
            box_count: count,
        };
        let boxes = |count| {
            let mut scene = scene(&track);
            let mut layout = layout();
            layout.pit_lane = Some(lane(count));
            dress_scene(&track, &mut scene, &layout).unwrap();
            scene.pit_lane.expect("lane").box_count
        };
        let room = boxes(Some(200));
        assert!(room > PIT_GRID_BOXES, "this lane holds a grid: {room}");
        assert_eq!(boxes(None), PIT_GRID_BOXES, "every car its own garage");
        assert_eq!(
            boxes(Some(12)),
            PIT_GRID_BOXES,
            "a short survey is not a limit"
        );
        assert_eq!(boxes(Some(room - 1)), room - 1, "a longer one is used");
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
            box_count: None,
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

//! Dress a `.ats` scene from the circuit's real-world layout dossier.
//!
//! The shipped scenes were dressed by a procedural enrichment pass that
//! knew nothing about the actual venue: grandstands landed wherever there
//! was room, the pit lane took whichever side of the road the old data
//! guessed, and no circuit had its landmarks. This pass replaces all of
//! that with what is really there, as recorded in
//! `content/tracks/real/<Stem>.layout.json` (see [`crate::layout`]).
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

use crate::ats::{AtsScene, PitLane, Prop, PropKind, Side};
use crate::layout::{Crossing, Landmark, Layout, Stand, Structure};
use crate::props;
use crate::terrain::TerrainHeightfield;
use crate::track_data::TrackFile;
use crate::track_mesh::surface_height;
use crate::track_path::{offset_point, CenterlinePath, PathSample};

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
/// A building with a footprint at least this square and this many levels
/// is a tower.
const TOWER_LEVELS: u32 = 4;

/// Pit boxes are one garage module apart (`ApexTrackAssetBuilder`'s 6 m
/// pitch); the count is what the dossier's lane has room for.
const PIT_BOX_PITCH_M: f32 = 6.0;
const PIT_BOXES: std::ops::RangeInclusive<u32> = 8..=40;
const PIT_WIDTH_M: f32 = 12.0;
const PIT_SPEED_LIMIT_KMH: f32 = 80.0;

#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct DressReport {
    /// Props of the dressed kinds removed before re-laying.
    pub removed: usize,
    pub stands: usize,
    pub stand_props: usize,
    pub buildings: usize,
    pub bridges: usize,
    pub landmarks: usize,
    pub pit_lane: bool,
    /// Dossier entries that could not be placed, with the reason.
    pub skipped: Vec<String>,
}

impl DressReport {
    pub fn placed(&self) -> usize {
        self.stand_props + self.buildings + self.bridges + self.landmarks
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

/// Apply `layout` to `scene`. Returns `None` for a degenerate centerline.
pub fn dress_scene(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: &Layout,
) -> Option<DressReport> {
    let path = CenterlinePath::from_track(track)?;
    let terrain = TerrainHeightfield::from_path(&path)?;
    let before = scene.props.len();
    // The ids of the props this pass replaces come back to it, lowest
    // first, so re-dressing an unchanged circuit rewrites the same file
    // rather than walking `next_id` up on every run.
    let mut recycled: Vec<u64> = scene
        .props
        .iter()
        .filter(|p| dressed_kind(p.kind))
        .map(|p| p.id)
        .collect();
    recycled.sort_unstable();
    recycled.reverse();
    scene.props.retain(|p| !dressed_kind(p.kind));
    let mut report = DressReport {
        removed: before - scene.props.len(),
        ..Default::default()
    };

    if let Some(pit) = build_pit_lane(&path, layout) {
        scene.pit_lane = Some(pit);
        report.pit_lane = true;
    }
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
    for structure in &layout.structures {
        match lay_structure(&path, &terrain, lane.as_ref(), structure) {
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
        if let Some(prop) = lay_crossing(&path, crossing) {
            report.bridges += 1;
            laid.push(prop);
        }
    }
    for landmark in &layout.landmarks {
        if let Some(prop) = lay_landmark(&path, &terrain, landmark) {
            report.landmarks += 1;
            laid.push(prop);
        }
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

// ---- Pit lane -------------------------------------------------------------

/// The real pit lane: the dossier's polyline, seated on the road's own
/// height at every node, with as many boxes as its parallel section holds.
/// Marked [`PitLane::authored`] so grooming leaves it alone instead of
/// replacing it with a generated ribbon.
fn build_pit_lane(path: &CenterlinePath, layout: &Layout) -> Option<PitLane> {
    let road = layout.pit_lane.as_ref()?;
    if road.nodes.len() < 3 {
        return None;
    }
    let nodes: Vec<[f32; 3]> = road
        .nodes
        .iter()
        .map(|n| {
            let (sample, lat) = nearest_cross_section(path, n[0], n[1]);
            let z = offset_point(&sample, lat).2;
            [n[0], n[1], z]
        })
        .collect();
    // The stretch that runs clear of the road is what the garages stand
    // along; the tapers at either end hold nothing.
    let clear: f32 = nodes
        .windows(2)
        .filter(|w| {
            let mid = [(w[0][0] + w[1][0]) / 2.0, (w[0][1] + w[1][1]) / 2.0];
            let (sample, lat) = nearest_cross_section(path, mid[0], mid[1]);
            lat.abs() > side_half_width(&sample, road.side) + PIT_WIDTH_M * 0.5
        })
        .map(|w| (w[1][0] - w[0][0]).hypot(w[1][1] - w[0][1]))
        .sum();
    let box_count = ((clear / PIT_BOX_PITCH_M) as u32).clamp(*PIT_BOXES.start(), *PIT_BOXES.end());
    Some(PitLane {
        nodes,
        width_m: PIT_WIDTH_M,
        box_count,
        speed_limit_kmh: PIT_SPEED_LIMIT_KMH,
        authored: true,
    })
}

// ---- Grandstands ----------------------------------------------------------

/// The bay family a stand is built from: `_large` once it is deep enough
/// to be a two-tier stand, roofed when the dossier says so (or when it is
/// too deep to be anything else).
fn stand_family(stand: &Stand) -> &'static str {
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

/// Place a building on its real footprint. A footprint longer than the
/// kit's block is filled with a row of them, which is what a 200 m
/// hospitality terrace looks like anyway.
fn lay_structure(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    lane: Option<&CenterlinePath>,
    structure: &Structure,
) -> Result<Vec<Prop>, String> {
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
    let (asset, unit_m) = building_asset(structure);
    let units = (structure.length_m / unit_m).round().max(1.0) as usize;
    let (sin, cos) = structure.yaw_rad.sin_cos();
    // The pivot is the face toward the road: the kit's footprint runs from
    // the pivot away from it.
    let (_, centre_lat) = nearest_cross_section(path, structure.centre[0], structure.centre[1]);
    let toward_road = if centre_lat >= 0.0 { -1.0 } else { 1.0 };
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
fn lay_crossing(path: &CenterlinePath, crossing: &Crossing) -> Option<Prop> {
    let sample = path.sample_at(crossing.station_m);
    let asset = if crossing.kind == "footbridge" {
        "truss_bridge"
    } else {
        "tyre_bridge"
    };
    Some(Prop {
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
    })
}

/// The brands the kit has artwork for (`content/props/board/brands`).
/// Signage in the kit is fictionalised, so a dossier naming a real
/// sponsor gets nothing rather than a texture that does not exist.
const KIT_BRANDS: [&str; 8] = [
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

fn lay_landmark(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    landmark: &Landmark,
) -> Option<Prop> {
    let (kind, asset) = match landmark.kind.as_str() {
        "big_wheel" => (PropKind::Attraction, "ferris_wheel"),
        "screen" => (PropKind::Attraction, "video_screen"),
        "stage" => (PropKind::Attraction, "fanzone_stage"),
        "camera_tower" => (PropKind::Attraction, "camera_tower"),
        "tower" => (PropKind::Building, "control_tower"),
        "statue" => (PropKind::Misc, "bull_statue"),
        "floodlight" => (PropKind::Light, "floodlight_tower"),
        "blimp" => (PropKind::Sky, "blimp"),
        "balloon" => (PropKind::Sky, "balloon"),
        "helicopter" => (PropKind::Sky, "helicopter"),
        _ => return None,
    };
    let (x, y) = (landmark.centre[0], landmark.centre[1]);
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
    Some(Prop {
        id: 0,
        kind,
        asset: asset.to_string(),
        x,
        y,
        z,
        yaw_rad: landmark.yaw_rad.unwrap_or(sample.heading_rad),
        scale: 1.0,
        text: brand_text(landmark.brand.as_ref()),
        length_m: None,
    })
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
            track_id: None,
            nodes,
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 12.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        }
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

    /// A box that straddles the centerline is not a building beside the
    /// road, and a footbridge mapped as a building is not one at all.
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
        });
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert!(report.pit_lane);
        let pit = scene.pit_lane.clone().expect("lane");
        assert!(
            pit.authored,
            "the real lane is a fact, not a generated shape"
        );
        assert_eq!(pit.nodes.len(), 16);
        assert!(pit.box_count >= 8);

        // Grooming must not replace it.
        let before = scene.pit_lane.clone();
        crate::groom::groom_scene_with(&track, &mut scene, Some(&layout)).unwrap();
        assert_eq!(scene.pit_lane, before);
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
        });
        let report = dress_scene(&track, &mut scene, &layout).unwrap();
        assert_eq!(report.skipped.len(), 1, "{:?}", report.skipped);
        assert!(report.skipped[0].contains("pit complex"));
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
        let trees: Vec<&Prop> = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Tree)
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
                ["conifer_m", "conifer_l", "poplar"].contains(&t.asset.as_str()),
                "{} is not a conifer",
                t.asset
            );
        }
    }
}

//! Baking a track project into the JSON the Unreal `ApexTrackImport`
//! commandlet turns into a level.
//!
//! The `.ats` scene alone is not enough for Unreal: every track-anchored
//! element (surface, curb, marking) is a *station span* measured along a
//! centerline that lives in the read-only `track.yaml`, and Unreal has no
//! YAML parser. So the export resolves all of that here — it samples the
//! centerline through the same [`CenterlinePath`] the viewport and the
//! server use, extrudes every element into triangles, and writes one
//! self-contained `.uescene.json` per track. The commandlet stays a dumb
//! asset writer: buffers in, `UStaticMesh` out.
//!
//! These are *not* the editor's preview meshes. [`crate::track_mesh`] builds
//! flat strips with a hardcoded up-normal, which is fine for a viewport and
//! wrong for a lit level: here normals follow the banked surface frame and
//! curbs get a raised profile with a real outer face.
//!
//! # Conventions at the boundary
//!
//! Everything in the output is already in Unreal's frame, so the commandlet
//! never converts anything:
//!
//! - **Units**: centimeters, degrees.
//! - **Position**: track `(x, y, z)` m -> UE `(x·100, −y·100, z·100)` cm.
//! - **Yaw**: track `θ` rad CCW -> UE `−θ·180/π` degrees.
//!
//! That mapping negates one axis, so its determinant is −1: it is a mirror,
//! not a rotation, and it flips triangle handedness for free. Geometry is
//! therefore emitted with ordinary right-handed CCW winding and *not*
//! reversed again — see [`Builder::emit_quad`], which is the one place the
//! convention lives.
//!
//! # Determinism
//!
//! Same inputs must give a byte-identical file: no wall clock, no unordered
//! iteration, and every float is rounded to a fixed number of decimals
//! before serialization (which also keeps the files a good deal smaller).

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::ats::{AtsScene, Curb, Marking, Side, Surface};
use crate::track_data::TrackFile;
use crate::track_mesh::{surface_kind_color, surface_lift, CURB_LIFT_M, MARKING_LIFT_M};
use crate::track_path::{offset_point, CenterlinePath, PathSample};

pub const UE_SCENE_FORMAT: &str = "apex-ue-scene";
pub const UE_SCENE_VERSION: u32 = 1;

/// Meters -> Unreal centimeters.
const M_TO_CM: f32 = 100.0;

/// Cross-section spacing when baking the track ribbon, curbs, markings and
/// the pit lane, meters.
const STEP_M: f32 = 1.0;
/// Ground patches are big and flat; they do not need road tessellation.
const SURFACE_STEP_M: f32 = 4.0;

/// Strips are cut at multiples of this station, and chunks that share a
/// section and a material are merged into one mesh. Without it a circuit
/// becomes either one 6 km mesh (no culling) or one mesh per element
/// (hundreds of actors).
const SECTION_LEN_M: f32 = 250.0;

/// Minimum `facet_quality` a triangle needs to be worth exporting: the sine
/// of the angle between its edges, so roughly half a degree.
const MIN_FACET_QUALITY: f32 = 0.01;

/// Height of a curb's outer lip above the track surface, meters.
const CURB_HEIGHT_M: f32 = 0.05;
/// Where the curb's slope breaks, as a fraction of its width / height.
const CURB_LIP_FRAC: f32 = 0.15;

// Fallback starting grid, mirroring
// `server/src/track_loader.rs::generate_start_positions` so the client puts
// its `PlayerStart`s exactly where the server will put the cars.
const GRID_SLOTS: u32 = 16;
const GRID_SPACING_M: f32 = 8.0;
const GRID_LATERAL_M: f32 = 3.0;

// ---------------------------------------------------------------------------
// Output model
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeScene {
    pub format: String,
    pub version: u32,
    #[serde(default)]
    pub track_id: Option<String>,
    pub track_name: String,
    /// File name of the logical YAML this was baked from.
    pub source_track: String,
    pub closed_loop: bool,
    pub length_cm: f32,
    pub metadata: UeMetadata,
    /// Every material key referenced by `meshes`, with what the commandlet
    /// needs to generate a material for it. Sorted by key.
    pub materials: Vec<UeMaterial>,
    pub meshes: Vec<UeMesh>,
    pub props: Vec<UeProp>,
    /// Starting grid, in finishing-order slots (slot 1 is pole).
    pub grid: Vec<UeGridSlot>,
    /// The sampled centerline, for spline-following, minimaps and AI.
    pub centerline: Vec<UeCenterlinePoint>,
    #[serde(default)]
    pub pit_lane: Option<UePitLane>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct UeMetadata {
    #[serde(default)]
    pub country: Option<String>,
    #[serde(default)]
    pub city: Option<String>,
    #[serde(default)]
    pub category: Option<String>,
    #[serde(default)]
    pub environment_type: Option<String>,
}

/// How a material key should look. The commandlet generates one material
/// instance per key; `base_color` is what the editor previewed, which is at
/// least a legible stand-in until real materials exist in the project.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeMaterial {
    pub key: String,
    /// `road`, `curb`, `surface`, `marking` or `pit_lane` — lets the
    /// commandlet pick a parent material per family.
    pub family: String,
    /// Linear RGBA.
    pub base_color: [f32; 4],
}

/// One bakeable static mesh. Buffers are flattened (`[x, y, z, x, y, z, …]`)
/// because nesting them triples the JSON size for no gain.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeMesh {
    /// `{material_key}_{section:03}`, unique within the scene.
    pub name: String,
    pub material_key: String,
    /// Vertex positions, UE centimeters.
    pub positions: Vec<f32>,
    pub normals: Vec<f32>,
    /// `u` runs along the track in meters, `v` across it in meters. Left in
    /// world scale so one material tiles identically on every track.
    pub uvs: Vec<f32>,
    /// Triangle list, already wound for Unreal's left-handed frame.
    pub indices: Vec<u32>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeProp {
    pub kind: String,
    /// Asset key the commandlet maps to a mesh or blueprint.
    pub asset: String,
    /// UE centimeters.
    pub location: [f32; 3],
    pub yaw_deg: f32,
    pub scale: f32,
    #[serde(default)]
    pub text: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeGridSlot {
    /// 1-based; slot 1 is pole.
    pub position: u32,
    pub location: [f32; 3],
    pub yaw_deg: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeCenterlinePoint {
    /// Station along the centerline, UE centimeters.
    pub s_cm: f32,
    pub location: [f32; 3],
    pub yaw_deg: f32,
    pub half_left_cm: f32,
    pub half_right_cm: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UePitLane {
    pub width_cm: f32,
    pub box_count: u32,
    pub speed_limit_kmh: f32,
}

// ---------------------------------------------------------------------------
// Baking
// ---------------------------------------------------------------------------

/// Bake a loaded track and its scene into the Unreal export.
///
/// Returns `None` only for a degenerate track the centerline sampler
/// rejects (fewer than two nodes, or zero length).
pub fn bake(track: &TrackFile, scene: &AtsScene) -> Option<UeScene> {
    let path = CenterlinePath::from_track(track)?;
    let mut chunks: Vec<Chunk> = Vec::new();
    let mut materials: BTreeMap<String, UeMaterial> = BTreeMap::new();

    bake_road(&path, &mut chunks, &mut materials);
    // Ground first, then the track, then what sits on it — the same layering
    // the viewport uses, so the export reads the way the editor looked.
    for surface in &scene.surfaces {
        bake_surface(&path, surface, &mut chunks, &mut materials);
    }
    for curb in &scene.curbs {
        bake_curb(&path, curb, &mut chunks, &mut materials);
    }
    for marking in &scene.markings {
        bake_marking(&path, marking, &mut chunks, &mut materials);
    }
    let pit_lane = scene.pit_lane.as_ref().and_then(|pit| {
        let lane = CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0)?;
        bake_pit_lane(&lane, pit.width_m, &mut chunks, &mut materials);
        Some(UePitLane {
            width_cm: round(pit.width_m * M_TO_CM, 1),
            box_count: pit.box_count,
            speed_limit_kmh: round(pit.speed_limit_kmh, 2),
        })
    });

    let metadata = track
        .metadata
        .as_ref()
        .map(|m| UeMetadata {
            country: m.country.clone(),
            city: m.city.clone(),
            category: m.category.clone(),
            environment_type: m.environment_type.clone(),
        })
        .unwrap_or_default();

    Some(UeScene {
        format: UE_SCENE_FORMAT.to_string(),
        version: UE_SCENE_VERSION,
        track_id: track.track_id.clone(),
        track_name: track.name.clone(),
        source_track: scene.source_track.clone(),
        closed_loop: track.closed_loop,
        length_cm: round(path.total_length_m() * M_TO_CM, 1),
        metadata,
        materials: materials.into_values().collect(),
        meshes: merge_chunks(chunks),
        props: bake_props(scene),
        grid: bake_grid(track, &path),
        centerline: bake_centerline(&path),
        pit_lane,
    })
}

// ---------------------------------------------------------------------------
// Strip extrusion
// ---------------------------------------------------------------------------

/// One point of a strip's cross-section: lateral offset from the centerline
/// (positive = left) and height above the surface, both meters.
#[derive(Clone, Copy, Debug)]
struct ProfilePoint {
    lat_m: f32,
    lift_m: f32,
}

/// One drawable cross-section, in track space.
struct CrossSection {
    points: Vec<(f32, f32, f32)>,
    /// Lateral arc length at each point, for the `v` texture coordinate.
    vs: Vec<f32>,
    /// Unwrapped station, so it stays monotonic across start/finish.
    station: f32,
    /// Unit course direction from the centerline heading. Unlike a
    /// direction derived from the strip's own vertices, this stays correct
    /// where the strip itself has folded, which is what makes it usable as
    /// the reference for detecting that fold.
    course: (f32, f32, f32),
}

/// Raw geometry for one (section, material) bucket, in UE units already.
struct Chunk {
    section: i32,
    material_key: String,
    positions: Vec<f32>,
    normals: Vec<f32>,
    uvs: Vec<f32>,
    indices: Vec<u32>,
}

/// Extrude a cross-section profile along `path` from `start_m` to `end_m`.
///
/// `profile` fills the cross-section at each station; it is handed the
/// sample and the progress through the span (0 at `start_m`, 1 at `end_m`)
/// so tapering elements can vary their width. The profile **must be ordered
/// by non-increasing lateral offset** (left to right): outward normals are
/// derived from that ordering, and a profile authored the other way round
/// comes out lit from underneath. [`normalize_profile`] enforces it.
///
/// On a closed path `end_m <= start_m` wraps through the start/finish line.
fn bake_strip<F>(
    path: &CenterlinePath,
    start_m: f32,
    end_m: f32,
    step_m: f32,
    material_key: &str,
    mut profile: F,
    chunks: &mut Vec<Chunk>,
) where
    F: FnMut(&PathSample, f32, &mut Vec<ProfilePoint>),
{
    let Some((start, end)) = resolve_span(path, start_m, end_m) else {
        return;
    };
    let span = end - start;
    let steps = ((span / step_m).ceil() as usize).max(1);

    // Every cross-section, in track space, before any UE conversion.
    // `None` marks a station the element cannot be drawn at — see
    // `clamp_profile` — which breaks the strip rather than distorting it.
    let mut sections: Vec<Option<CrossSection>> = Vec::with_capacity(steps + 1);
    let mut buf: Vec<ProfilePoint> = Vec::new();
    let mut profile_len = 0usize;

    for i in 0..=steps {
        // Unwrapped on purpose: a span that wraps keeps counting past the
        // total length, which is what the section index and the `u`
        // coordinate both want — each must stay monotonic across
        // start/finish.
        let station = start + span * (i as f32 / steps as f32);
        let sample = path.sample_at(station);
        buf.clear();
        profile(&sample, i as f32 / steps as f32, &mut buf);
        if buf.len() < 2 {
            return;
        }
        // Order first, then clamp: clamping is monotonic, so it cannot
        // disturb the left-to-right ordering the normals rely on, whereas
        // ordering a clamped profile could flip a cross-section whose
        // points collapsed onto the limit.
        normalize_profile(&mut buf);
        if profile_len == 0 {
            profile_len = buf.len();
        } else if buf.len() != profile_len {
            // A profile that changes point count mid-span cannot be
            // extruded as one strip.
            return;
        }
        if !clamp_profile(&mut buf, curvature_at(path, station)) {
            sections.push(None);
            continue;
        }

        let mut points = Vec::with_capacity(buf.len());
        let mut v_coords = Vec::with_capacity(buf.len());
        let mut v_acc = 0.0f32;
        for (k, p) in buf.iter().enumerate() {
            let mut pos = offset_point(&sample, p.lat_m);
            pos.2 += p.lift_m;
            if k > 0 {
                v_acc += distance(points[k - 1], pos);
            }
            points.push(pos);
            v_coords.push(v_acc);
        }
        let (sin_h, cos_h) = sample.heading_rad.sin_cos();
        sections.push(Some(CrossSection {
            points,
            vs: v_coords,
            station,
            course: (cos_h, sin_h, 0.0),
        }));
    }

    // Emit each maximal run of drawable cross-sections, cut again wherever
    // it crosses a section boundary (the shared cross-section is repeated on
    // both sides, so the seam has no gap).
    let mut builder = Builder::default();
    let mut open_section: Option<i32> = None;
    let mut flush = |builder: &mut Builder, open: &mut Option<i32>| {
        if let Some(section) = open.take() {
            let chunk = std::mem::take(builder).finish(section, material_key);
            if !chunk.indices.is_empty() {
                chunks.push(chunk);
            }
        }
    };

    for i in 0..steps {
        let (Some(a), Some(b)) = (&sections[i], &sections[i + 1]) else {
            // The strip is interrupted here; close whatever is open so the
            // two sides never get stitched across the gap.
            flush(&mut builder, &mut open_section);
            continue;
        };

        let section = section_index(a.station);
        if open_section != Some(section) {
            flush(&mut builder, &mut open_section);
            open_section = Some(section);
        }

        let forward_a = forward_at(&sections, i);
        let forward_b = forward_at(&sections, i + 1);
        for k in 0..profile_len - 1 {
            let quad = [
                (a.points[k], a.vs[k], a.station),
                (a.points[k + 1], a.vs[k + 1], a.station),
                (b.points[k + 1], b.vs[k + 1], b.station),
                (b.points[k], b.vs[k], b.station),
            ];
            builder.emit_quad(quad, forward_a, forward_b, a.course);
        }
    }
    flush(&mut builder, &mut open_section);
}

/// Signed curvature at a station, 1/m. Positive means the course turns
/// left. Taken as the rate of change of heading over arc length; on an open
/// path `sample_at` clamps at the ends, which only flattens the estimate.
fn curvature_at(path: &CenterlinePath, station_m: f32) -> f32 {
    use std::f32::consts::{PI, TAU};
    // Wide enough to average out the heading jitter of digitized nodes,
    // which sit about 2 m apart. A short window turns that jitter into
    // spikes of false curvature and clamps perfectly straight sections.
    const DELTA_M: f32 = 8.0;
    let behind = path.sample_at(station_m - DELTA_M);
    let ahead = path.sample_at(station_m + DELTA_M);
    let mut delta = ahead.heading_rad - behind.heading_rad;
    // Unwrap across ±pi, or a corner straddling the branch cut reads as an
    // enormous curvature and clamps the whole cross-section to nothing.
    while delta > PI {
        delta -= TAU;
    }
    while delta < -PI {
        delta += TAU;
    }
    delta / (2.0 * DELTA_M)
}

/// Keep lateral offsets short of the centre of curvature, and report
/// whether anything drawable is left.
///
/// A station-anchored element measures straight out from the centerline, so
/// on the inside of a corner its borders converge. Past a radius of `1/κ`
/// the offset curve folds through itself: consecutive cross-sections run
/// backwards relative to each other and the strip turns inside out. Austin
/// has apexes down to an 8 m radius on a 15 m wide track, so this is not a
/// corner case — the road's own inside edge folds there.
///
/// Clamping fixes the borders that reach too far. What it cannot fix is an
/// element that lies *entirely* beyond the limit — a curb on the inside of
/// such an apex would have to wrap around a sub-metre radius, and squeezing
/// it into the available room just yields a knot of degenerate facets.
/// Those cross-sections return `false` and the caller breaks the strip:
/// a curb interrupted for a few metres reads far better than one tied in a
/// knot.
fn clamp_profile(profile: &mut [ProfilePoint], kappa: f32) -> bool {
    /// Fraction of the turn radius a border may reach.
    const MARGIN: f32 = 0.85;
    /// Below this much lateral extent there is no facet worth emitting.
    const MIN_EXTENT_M: f32 = 0.05;
    /// Room a one-sided element needs between its inner border and the
    /// curvature limit to be worth drawing at all.
    const MIN_ROOM_M: f32 = 0.5;

    if kappa.abs() >= 1e-6 {
        let limit = MARGIN / kappa.abs();

        // An element straddling the centerline — the road, a marking across
        // it — always has room: only its far edge is clamped, and it keeps
        // the rest of its width. One lying wholly on the inside of the turn
        // is different, and needs actual room between its inner border and
        // the limit, not merely to start inside it. An element whose inner
        // border sits a hair inside gets compressed into a few centimetres
        // of facets sweeping around the centre of curvature: adjacent
        // cross-sections then barely advance while the profile writhes, and
        // the result is a knot, not a curb. Report those undrawable and let
        // the caller break the strip.
        let wholly_inside = if kappa > 0.0 {
            profile.iter().all(|p| p.lat_m > 0.0)
        } else {
            profile.iter().all(|p| p.lat_m < 0.0)
        };
        if wholly_inside {
            let innermost = profile
                .iter()
                .map(|p| p.lat_m.abs())
                .fold(f32::INFINITY, f32::min);
            if limit < innermost + MIN_ROOM_M {
                return false;
            }
        }

        for p in profile.iter_mut() {
            p.lat_m = if kappa > 0.0 {
                p.lat_m.min(limit)
            } else {
                p.lat_m.max(-limit)
            };
        }
    }
    let lo = profile
        .iter()
        .map(|p| p.lat_m)
        .fold(f32::INFINITY, f32::min);
    let hi = profile
        .iter()
        .map(|p| p.lat_m)
        .fold(f32::NEG_INFINITY, f32::max);
    hi - lo >= MIN_EXTENT_M
}

/// Enforce the "left to right" profile ordering the normals depend on.
fn normalize_profile(profile: &mut [ProfilePoint]) {
    let (Some(first), Some(last)) = (profile.first(), profile.last()) else {
        return;
    };
    if first.lat_m < last.lat_m {
        profile.reverse();
    }
}

/// Course direction at cross-section `i`, from the centers of the
/// neighbouring cross-sections. Taken from the emitted geometry rather than
/// the sample heading so it picks up elevation change as well as bearing.
///
/// Neighbours that were dropped as undrawable are skipped over; the
/// cross-section itself stands in when it has no usable neighbour on a side.
fn forward_at(sections: &[Option<CrossSection>], i: usize) -> (f32, f32, f32) {
    let mid = |s: &CrossSection| {
        let a = s.points[0];
        let b = s.points[s.points.len() - 1];
        ((a.0 + b.0) * 0.5, (a.1 + b.1) * 0.5, (a.2 + b.2) * 0.5)
    };
    let here = sections[i].as_ref().map(&mid);
    let before = (0..i).rev().find_map(|j| sections[j].as_ref().map(&mid));
    let after = (i + 1..sections.len()).find_map(|j| sections[j].as_ref().map(&mid));

    let (lo, hi) = match (before, after) {
        (Some(lo), Some(hi)) => (lo, hi),
        (Some(lo), None) => (lo, here.unwrap_or(lo)),
        (None, Some(hi)) => (here.unwrap_or(hi), hi),
        (None, None) => return (1.0, 0.0, 0.0),
    };
    let forward = normalize((hi.0 - lo.0, hi.1 - lo.1, hi.2 - lo.2));

    // Where a strip doubles back — a curb on the inside of an apex tighter
    // than the curb's own offset — successive cross-sections march
    // backwards and this difference points against the course. Its
    // magnitude is still the surface's true slope, so keep it and just fix
    // the sign; left alone, it flips the normal of every facet it touches
    // while the geometry around it stays put.
    let course = sections[i]
        .as_ref()
        .map(|s| s.course)
        .unwrap_or((1.0, 0.0, 0.0));
    if dot(forward, course) < 0.0 {
        (-forward.0, -forward.1, -forward.2)
    } else {
        forward
    }
}

#[derive(Default)]
struct Builder {
    positions: Vec<f32>,
    normals: Vec<f32>,
    uvs: Vec<f32>,
    indices: Vec<u32>,
}

impl Builder {
    /// One quad of a strip, given as
    /// `[(i,k), (i,k+1), (i+1,k+1), (i+1,k)]` in track space with each
    /// vertex's `v` coordinate and station.
    ///
    /// Vertices are not shared between quads. That costs some duplication,
    /// but it means each facet of a curb keeps its own normal without any
    /// smoothing-group machinery, while normals still vary smoothly *along*
    /// the strip because they are derived per cross-section.
    fn emit_quad(
        &mut self,
        quad: [((f32, f32, f32), f32, f32); 4],
        forward_a: (f32, f32, f32),
        forward_b: (f32, f32, f32),
        course: (f32, f32, f32),
    ) {
        // Reject a facet that has folded back on itself.
        //
        // Clamping keeps most offsets inside the turn radius, but it works
        // from an estimated curvature, and an estimate wide enough to ignore
        // the jitter of digitized nodes also flattens a genuine hairpin. So
        // the fold is caught here instead, after the fact and locally: if
        // either edge of this facet advances *against* the course, the strip
        // has turned back on itself and the facet is inside-out by
        // construction. Better to leave a hole than to ship one.
        if dot(sub(quad[3].0, quad[0].0), course) <= 0.0
            || dot(sub(quad[2].0, quad[1].0), course) <= 0.0
        {
            return;
        }

        // Lateral direction of this facet, per cross-section. Ordered left
        // to right, so `cross(lateral, forward)` points out of the surface.
        //
        // A facet can pinch to nothing where a clamped patch converges, and
        // a zero-length edge has no direction to take a normal from. Borrow
        // the other cross-section's rather than inventing one; if both have
        // pinched there is no facet left to draw.
        let raw_a = sub(quad[1].0, quad[0].0);
        let raw_b = sub(quad[2].0, quad[3].0);
        let (ok_a, ok_b) = (length(raw_a) > 1e-5, length(raw_b) > 1e-5);
        let (lat_a, lat_b) = match (ok_a, ok_b) {
            (true, true) => (normalize(raw_a), normalize(raw_b)),
            (true, false) => (normalize(raw_a), normalize(raw_a)),
            (false, true) => (normalize(raw_b), normalize(raw_b)),
            (false, false) => return,
        };

        // Slivers get dropped rather than shipped. A 130 m wide ground band
        // swept round a corner degenerates into facets a fraction of a
        // degree wide: they cover no pixels at any viewing angle, their
        // orientation is numerical noise, and they are exactly the input
        // Nanite and lightmap packing handle worst.
        let keep_first = facet_quality(quad[0].0, quad[1].0, quad[2].0) >= MIN_FACET_QUALITY;
        let keep_second = facet_quality(quad[0].0, quad[2].0, quad[3].0) >= MIN_FACET_QUALITY;
        if !keep_first && !keep_second {
            return;
        }

        let n_a = normalize(cross(lat_a, forward_a));
        let n_b = normalize(cross(lat_b, forward_b));

        // The two ends of this facet disagree about which way the surface
        // faces, so it is twisted through more than a right angle between
        // one cross-section and the next. No winding is correct for such a
        // quad — one of its triangles would be inside-out whichever way it
        // is wound — so there is nothing to emit. This is what is left of a
        // strip tangled around an apex tighter than its own offset, once
        // clamping and fold rejection have taken their share.
        if dot(n_a, n_b) <= 0.0 {
            return;
        }

        let base = (self.positions.len() / 3) as u32;

        for (vertex, normal) in [
            (quad[0], n_a),
            (quad[1], n_a),
            (quad[2], n_b),
            (quad[3], n_b),
        ] {
            let (pos, v, station) = vertex;
            push_position(&mut self.positions, pos);
            push_normal(&mut self.normals, normal);
            self.uvs.push(round(station, 3));
            self.uvs.push(round(v, 3));
        }

        // Wind each triangle to agree with the normal it ships with.
        //
        // The base ordering is right-handed CCW, deliberately *not*
        // reversed: track -> UE mirrors Y, and that mirror is itself the
        // handedness change. Unreal's front face is clockwise seen from the
        // normal — `MeshDescription` takes a triangle's normal as
        // `-cross(p1 - p0, p2 - p0)` — so negating Y turns RH CCW-front
        // into exactly that. Reversing as well would light every surface in
        // the level from underneath.
        //
        // Rather than trust that argument per triangle, each one is checked
        // against the normal derived from the surface frame and flipped if
        // it disagrees. For well-formed geometry this never fires. It
        // exists for the residue at hairpins whose radius is smaller than
        // the width of the element wrapped around them: clamping and fold
        // rejection remove the bulk, but what survives can still be
        // degenerate enough that its vertex order says one thing and its
        // frame another. There the frame wins — it is the one that knows
        // which side is up.
        let mut push = |a: usize, b: usize, c: usize| {
            let geometric = cross(sub(quad[b].0, quad[a].0), sub(quad[c].0, quad[a].0));
            let (a, b, c) = (base + a as u32, base + b as u32, base + c as u32);
            if dot(geometric, n_a) >= 0.0 {
                self.indices.extend_from_slice(&[a, b, c]);
            } else {
                self.indices.extend_from_slice(&[a, c, b]);
            }
        };
        if keep_first {
            push(0, 1, 2);
        }
        if keep_second {
            push(0, 2, 3);
        }
    }

    fn finish(self, section: i32, material_key: &str) -> Chunk {
        Chunk {
            section,
            material_key: material_key.to_string(),
            positions: self.positions,
            normals: self.normals,
            uvs: self.uvs,
            indices: self.indices,
        }
    }
}

/// Merge chunks that share a section and a material into one mesh each.
fn merge_chunks(mut chunks: Vec<Chunk>) -> Vec<UeMesh> {
    // Stable sort on a total key: the merge order — and so the vertex order
    // inside every merged mesh — must not depend on how the scene happened
    // to be laid out in memory.
    chunks.sort_by(|a, b| {
        a.material_key
            .cmp(&b.material_key)
            .then(a.section.cmp(&b.section))
    });

    let mut meshes: Vec<UeMesh> = Vec::new();
    let mut open: Option<(String, i32)> = None;
    for chunk in chunks {
        if chunk.indices.is_empty() {
            continue;
        }
        let bucket = (chunk.material_key.clone(), chunk.section);
        if open.as_ref() != Some(&bucket) {
            meshes.push(UeMesh {
                name: mesh_name(&chunk.material_key, chunk.section),
                material_key: chunk.material_key.clone(),
                positions: Vec::new(),
                normals: Vec::new(),
                uvs: Vec::new(),
                indices: Vec::new(),
            });
            open = Some(bucket);
        }
        let mesh = meshes.last_mut().unwrap();
        let offset = (mesh.positions.len() / 3) as u32;
        mesh.positions.extend_from_slice(&chunk.positions);
        mesh.normals.extend_from_slice(&chunk.normals);
        mesh.uvs.extend_from_slice(&chunk.uvs);
        mesh.indices
            .extend(chunk.indices.iter().map(|i| i + offset));
    }
    meshes
}

fn mesh_name(material_key: &str, section: i32) -> String {
    format!("{material_key}_{section:03}")
}

fn section_index(station_m: f32) -> i32 {
    (station_m / SECTION_LEN_M).floor() as i32
}

/// Resolve a station span the way the viewport does: wrapping through
/// start/finish on a closed loop, clamped on an open path.
fn resolve_span(path: &CenterlinePath, start_m: f32, end_m: f32) -> Option<(f32, f32)> {
    let total = path.total_length_m();
    if !(total.is_finite() && total > 0.0) {
        return None;
    }
    if path.is_closed() {
        let start = start_m.rem_euclid(total);
        let mut end = end_m.rem_euclid(total);
        if end <= start {
            end += total;
        }
        Some((start, end))
    } else {
        let start = start_m.clamp(0.0, total);
        let end = end_m.clamp(0.0, total);
        (end > start).then_some((start, end))
    }
}

// ---------------------------------------------------------------------------
// Per-element profiles
// ---------------------------------------------------------------------------

fn bake_road(
    path: &CenterlinePath,
    chunks: &mut Vec<Chunk>,
    materials: &mut BTreeMap<String, UeMaterial>,
) {
    let key = "road";
    register(materials, key, "road", [0.24, 0.24, 0.26, 1.0]);
    bake_strip(
        path,
        0.0,
        path.total_length_m(),
        STEP_M,
        key,
        |sample, _, out| {
            out.push(ProfilePoint {
                lat_m: sample.width_left_m,
                lift_m: 0.0,
            });
            out.push(ProfilePoint {
                lat_m: -sample.width_right_m,
                lift_m: 0.0,
            });
        },
        chunks,
    );
}

fn bake_pit_lane(
    lane: &CenterlinePath,
    width_m: f32,
    chunks: &mut Vec<Chunk>,
    materials: &mut BTreeMap<String, UeMaterial>,
) {
    let key = "pit_lane";
    register(materials, key, "pit_lane", [0.32, 0.32, 0.34, 1.0]);
    let half = width_m / 2.0;
    bake_strip(
        lane,
        0.0,
        lane.total_length_m(),
        STEP_M,
        key,
        move |_, _, out| {
            out.push(ProfilePoint {
                lat_m: half,
                lift_m: 0.0,
            });
            out.push(ProfilePoint {
                lat_m: -half,
                lift_m: 0.0,
            });
        },
        chunks,
    );
}

/// A curb: a ramp from the track edge up to a lip, then a vertical face
/// dropping back to ground level on the outside. The editor previews this
/// as a flat painted strip; in a lit level it needs the actual profile.
fn bake_curb(
    path: &CenterlinePath,
    curb: &Curb,
    chunks: &mut Vec<Chunk>,
    materials: &mut BTreeMap<String, UeMaterial>,
) {
    let key = format!("curb_{}", sanitize(&curb.style));
    register(materials, &key, "curb", curb_base_color(&curb.style));

    let side = curb.side;
    let width = curb.width_m.max(0.01);
    bake_strip(
        path,
        curb.start_m,
        curb.end_m,
        STEP_M,
        &key,
        move |sample, _, out| {
            // Signed so the same arithmetic works on both sides: `outward`
            // grows away from the centerline.
            let (edge, outward) = match side {
                Side::Left => (sample.width_left_m, 1.0),
                Side::Right => (-sample.width_right_m, -1.0),
            };
            let at = |d: f32, lift: f32| ProfilePoint {
                lat_m: edge + outward * d,
                lift_m: lift + CURB_LIFT_M,
            };
            out.push(at(0.0, 0.0));
            out.push(at(width * CURB_LIP_FRAC, CURB_HEIGHT_M * 0.6));
            out.push(at(width, CURB_HEIGHT_M));
            // Outer face, straight down to the surrounding ground.
            out.push(at(width, -CURB_LIFT_M));
        },
        chunks,
    );
}

fn bake_surface(
    path: &CenterlinePath,
    surface: &Surface,
    chunks: &mut Vec<Chunk>,
    materials: &mut BTreeMap<String, UeMaterial>,
) {
    let key = format!("surface_{}", surface.kind.label());
    register(materials, &key, "surface", surface_kind_color(surface.kind));

    let side = surface.side;
    let inner = surface.inner_m;
    let lift = surface_lift(surface.kind);
    let spec = surface.clone();
    bake_strip(
        path,
        surface.start_m,
        surface.end_m,
        SURFACE_STEP_M,
        &key,
        move |sample, progress, out| {
            let (edge, outward) = match side {
                Side::Left => (sample.width_left_m, 1.0),
                Side::Right => (-sample.width_right_m, -1.0),
            };
            let width = spec.width_at(progress);
            out.push(ProfilePoint {
                lat_m: edge + outward * inner,
                lift_m: lift,
            });
            out.push(ProfilePoint {
                lat_m: edge + outward * (inner + width),
                lift_m: lift,
            });
        },
        chunks,
    );
}

fn bake_marking(
    path: &CenterlinePath,
    marking: &Marking,
    chunks: &mut Vec<Chunk>,
    materials: &mut BTreeMap<String, UeMaterial>,
) {
    // Markings of the same kind can be painted different colors, so the key
    // carries the color too — otherwise a yellow pit-exit line and a white
    // one would collapse onto one material.
    let key = format!(
        "marking_{}_{}",
        marking.kind.label(),
        color_hex(marking.color)
    );
    register(materials, &key, "marking", marking.color);

    let (lo, hi) = if marking.lat_from_m <= marking.lat_to_m {
        (marking.lat_from_m, marking.lat_to_m)
    } else {
        (marking.lat_to_m, marking.lat_from_m)
    };
    bake_strip(
        path,
        marking.start_m,
        marking.end_m,
        STEP_M,
        &key,
        move |_, _, out| {
            out.push(ProfilePoint {
                lat_m: hi,
                lift_m: MARKING_LIFT_M,
            });
            out.push(ProfilePoint {
                lat_m: lo,
                lift_m: MARKING_LIFT_M,
            });
        },
        chunks,
    );
}

fn curb_base_color(style: &str) -> [f32; 4] {
    match style {
        "yellow_black" => [0.9, 0.75, 0.05, 1.0],
        "green_white" => [0.1, 0.5, 0.15, 1.0],
        "blue_white" => [0.1, 0.25, 0.7, 1.0],
        _ => [0.75, 0.12, 0.1, 1.0],
    }
}

fn register(
    materials: &mut BTreeMap<String, UeMaterial>,
    key: &str,
    family: &str,
    base_color: [f32; 4],
) {
    materials
        .entry(key.to_string())
        .or_insert_with(|| UeMaterial {
            key: key.to_string(),
            family: family.to_string(),
            base_color: base_color.map(|c| round(c, 4)),
        });
}

// ---------------------------------------------------------------------------
// Non-geometry payload
// ---------------------------------------------------------------------------

fn bake_props(scene: &AtsScene) -> Vec<UeProp> {
    scene
        .props
        .iter()
        .map(|p| UeProp {
            kind: p.kind.label().to_string(),
            asset: p.asset.clone(),
            location: to_ue((p.x, p.y, p.z)),
            yaw_deg: round(-p.yaw_rad.to_degrees(), 3),
            scale: round(p.scale, 4),
            text: p.text.clone(),
        })
        .collect()
}

/// The starting grid, resolved exactly the way the server resolves it, so a
/// car spawned by the client sits where the server thinks it is.
fn bake_grid(track: &TrackFile, path: &CenterlinePath) -> Vec<UeGridSlot> {
    let samples = path.samples();
    if samples.is_empty() {
        return Vec::new();
    }

    if !track.spawn_points.is_empty() {
        return track
            .spawn_points
            .iter()
            .enumerate()
            .filter_map(|(idx, spawn)| {
                let sample = samples.get(spawn.position)?;
                Some(UeGridSlot {
                    position: idx as u32 + 1,
                    location: to_ue((
                        sample.pos.0 + spawn.offset_x,
                        sample.pos.1 + spawn.offset_y,
                        sample.pos.2,
                    )),
                    yaw_deg: round(-sample.heading_rad.to_degrees(), 3),
                })
            })
            .collect();
    }

    // No authored grid: two columns stepping back from the line, matching
    // the server's fallback.
    let start = samples[0];
    let (sin_h, cos_h) = start.heading_rad.sin_cos();
    (0..GRID_SLOTS)
        .map(|i| {
            let row = (i / 2) as f32;
            let column = (i % 2) as f32;
            let forward = -row * GRID_SPACING_M;
            let lateral = (column - 0.5) * GRID_LATERAL_M;
            UeGridSlot {
                position: i + 1,
                location: to_ue((
                    start.pos.0 + forward * cos_h - lateral * sin_h,
                    start.pos.1 + forward * sin_h + lateral * cos_h,
                    start.pos.2,
                )),
                yaw_deg: round(-start.heading_rad.to_degrees(), 3),
            }
        })
        .collect()
}

fn bake_centerline(path: &CenterlinePath) -> Vec<UeCenterlinePoint> {
    path.samples()
        .iter()
        .map(|s| UeCenterlinePoint {
            s_cm: round(s.station_m * M_TO_CM, 1),
            location: to_ue(s.pos),
            yaw_deg: round(-s.heading_rad.to_degrees(), 3),
            half_left_cm: round(s.width_left_m * M_TO_CM, 1),
            half_right_cm: round(s.width_right_m * M_TO_CM, 1),
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Track meters -> Unreal centimeters, mirroring Y.
fn to_ue(p: (f32, f32, f32)) -> [f32; 3] {
    [
        round(p.0 * M_TO_CM, 1),
        round(-p.1 * M_TO_CM, 1),
        round(p.2 * M_TO_CM, 1),
    ]
}

fn push_position(out: &mut Vec<f32>, p: (f32, f32, f32)) {
    out.extend_from_slice(&to_ue(p));
}

fn push_normal(out: &mut Vec<f32>, n: (f32, f32, f32)) {
    out.push(round(n.0, 4));
    out.push(round(-n.1, 4));
    out.push(round(n.2, 4));
}

/// Fixed-precision rounding. Keeps repeated exports byte-identical and cuts
/// the file size roughly in half; `-0.0` is collapsed to `0.0` so the sign
/// of a zero can never flip between runs.
fn round(v: f32, places: i32) -> f32 {
    if !v.is_finite() {
        return 0.0;
    }
    let f = 10f32.powi(places);
    let r = (v * f).round() / f;
    if r == 0.0 {
        0.0
    } else {
        r
    }
}

fn sub(a: (f32, f32, f32), b: (f32, f32, f32)) -> (f32, f32, f32) {
    (a.0 - b.0, a.1 - b.1, a.2 - b.2)
}

fn cross(a: (f32, f32, f32), b: (f32, f32, f32)) -> (f32, f32, f32) {
    (
        a.1 * b.2 - a.2 * b.1,
        a.2 * b.0 - a.0 * b.2,
        a.0 * b.1 - a.1 * b.0,
    )
}

/// How far a triangle is from degenerate: the sine of the angle between two
/// of its edges, so it is scale-free. An absolute area test cannot work at
/// track scale, where a 0.2-degree sliver spanning 100 m still has a large
/// cross product.
fn facet_quality(a: (f32, f32, f32), b: (f32, f32, f32), c: (f32, f32, f32)) -> f32 {
    let e1 = sub(b, a);
    let e2 = sub(c, a);
    let scale = length(e1) * length(e2);
    if scale < 1e-9 {
        return 0.0;
    }
    length(cross(e1, e2)) / scale
}

fn dot(a: (f32, f32, f32), b: (f32, f32, f32)) -> f32 {
    a.0 * b.0 + a.1 * b.1 + a.2 * b.2
}

fn length(v: (f32, f32, f32)) -> f32 {
    (v.0 * v.0 + v.1 * v.1 + v.2 * v.2).sqrt()
}

fn normalize(v: (f32, f32, f32)) -> (f32, f32, f32) {
    let len = length(v);
    if len > 1e-6 {
        (v.0 / len, v.1 / len, v.2 / len)
    } else {
        (0.0, 0.0, 1.0)
    }
}

fn distance(a: (f32, f32, f32), b: (f32, f32, f32)) -> f32 {
    let d = sub(b, a);
    (d.0 * d.0 + d.1 * d.1 + d.2 * d.2).sqrt()
}

/// Lowercase hex of the RGB channels, for disambiguating material keys.
fn color_hex(c: [f32; 4]) -> String {
    let byte = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
    format!("{:02x}{:02x}{:02x}", byte(c[0]), byte(c[1]), byte(c[2]))
}

/// Reduce a free-form style key to something safe in an asset name.
fn sanitize(s: &str) -> String {
    let cleaned: String = s
        .trim()
        .to_ascii_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    if cleaned.is_empty() {
        "default".to_string()
    } else {
        cleaned
    }
}

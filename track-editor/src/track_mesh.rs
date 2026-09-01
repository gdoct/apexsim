//! Preview meshes for the editor viewport.
//!
//! Everything ribbon-shaped — the track surface itself, curbs, painted
//! markings, the pit lane — is built by one generic strip builder that
//! walks a [`CenterlinePath`] over a station span and extrudes between two
//! lateral offsets. These are editor stand-ins, not the authoritative
//! server geometry and not what Unreal renders; but because they sample the
//! same spline the server loads, what you see lines up with the simulation.

use bevy::asset::RenderAssetUsages;
use bevy::mesh::{Indices, Mesh, PrimitiveTopology};

use crate::ats::{Curb, Marking, Side, Surface, SurfaceKind};
use crate::coords;
use crate::terrain::{self, TerrainHeightfield};
use crate::track_path::{offset_point, CenterlinePath, PathSample};

/// Cross-section spacing when re-sampling a span for a strip mesh.
const STRIP_STEP_M: f32 = 1.0;
/// Curb stripes alternate color every this many meters.
const CURB_STRIPE_LEN_M: f32 = 2.0;

/// How far strips float above the surface they decorate, to avoid
/// z-fighting: ground surfaces < track < pit lane < curbs < markings.
pub const CURB_LIFT_M: f32 = 0.02;
pub const MARKING_LIFT_M: f32 = 0.04;
/// Ground patches sit just *below* the track so the ribbon always wins where
/// they meet, and each surface kind gets its own sliver of depth so a gravel
/// trap laid over the grass band doesn't z-fight with it.
const SURFACE_BASE_LIFT_M: f32 = -0.08;
const SURFACE_LAYER_STEP_M: f32 = 0.01;

pub fn surface_lift(kind: SurfaceKind) -> f32 {
    SURFACE_BASE_LIFT_M + kind.layer() as f32 * SURFACE_LAYER_STEP_M
}

/// Build a strip along `path` from `start_m` to `end_m`, spanning laterally
/// from `lat_a(sample)` to `lat_b(sample)` (meters, positive = left).
/// `height` maps each vertex's road-following position (centerline +
/// banking shear) to its final height — most strips just add a small lift,
/// ground bands blend into the terrain. `color` is evaluated per
/// cross-section.
///
/// On a closed path, `end_m <= start_m` wraps through the start/finish
/// line. Returns `None` for empty spans or degenerate paths.
pub fn build_strip_mesh(
    path: &CenterlinePath,
    start_m: f32,
    end_m: f32,
    lat_a: impl Fn(&PathSample) -> f32,
    lat_b: impl Fn(&PathSample) -> f32,
    height: impl Fn(&PathSample, f32, (f32, f32, f32)) -> f32,
    color: impl Fn(&PathSample) -> [f32; 4],
) -> Option<Mesh> {
    let total = path.total_length_m();
    let (start, end) = if path.is_closed() {
        let start = start_m.rem_euclid(total);
        let mut end = end_m.rem_euclid(total);
        if end <= start {
            end += total;
        }
        (start, end)
    } else {
        let start = start_m.clamp(0.0, total);
        let end = end_m.clamp(0.0, total);
        if end <= start {
            return None;
        }
        (start, end)
    };

    let span = end - start;
    let steps = ((span / STRIP_STEP_M).ceil() as usize).max(1);

    let mut positions: Vec<[f32; 3]> = Vec::with_capacity((steps + 1) * 2);
    let mut normals: Vec<[f32; 3]> = Vec::with_capacity((steps + 1) * 2);
    let mut colors: Vec<[f32; 4]> = Vec::with_capacity((steps + 1) * 2);
    let mut uvs: Vec<[f32; 2]> = Vec::with_capacity((steps + 1) * 2);

    for i in 0..=steps {
        let s = start + span * (i as f32 / steps as f32);
        let sample = path.sample_at(s);
        let (la, lb) = (lat_a(&sample), lat_b(&sample));
        let mut a = offset_point(&sample, la);
        let mut b = offset_point(&sample, lb);
        a.2 = height(&sample, la, a);
        b.2 = height(&sample, lb, b);

        positions.push(coords::to_bevy(a.0, a.1, a.2).to_array());
        positions.push(coords::to_bevy(b.0, b.1, b.2).to_array());
        normals.push([0.0, 1.0, 0.0]);
        normals.push([0.0, 1.0, 0.0]);
        let c = color(&sample);
        colors.push(c);
        colors.push(c);
        uvs.push([s, 0.0]);
        uvs.push([s, 1.0]);
    }

    let mut indices: Vec<u32> = Vec::with_capacity(steps * 6);
    for i in 0..steps {
        let i0 = (i * 2) as u32;
        let (i1, i2, i3) = (i0 + 1, i0 + 2, i0 + 3);
        // Two triangles per quad, wound so the top face (+Y in Bevy space)
        // is visible.
        indices.extend_from_slice(&[i0, i1, i2, i1, i3, i2]);
    }

    let mesh = Mesh::new(
        PrimitiveTopology::TriangleList,
        RenderAssetUsages::RENDER_WORLD | RenderAssetUsages::MAIN_WORLD,
    )
    .with_inserted_attribute(Mesh::ATTRIBUTE_POSITION, positions)
    .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, normals)
    .with_inserted_attribute(Mesh::ATTRIBUTE_COLOR, colors)
    .with_inserted_attribute(Mesh::ATTRIBUTE_UV_0, uvs)
    .with_inserted_indices(Indices::U32(indices));

    Some(mesh)
}

/// The main track surface: full width, colored by surface type.
pub fn build_track_ribbon(path: &CenterlinePath) -> Option<Mesh> {
    build_strip_mesh(
        path,
        0.0,
        path.total_length_m(),
        |s| s.width_left_m,
        |s| -s.width_right_m,
        |_, _, p| p.2,
        |s| s.surface_color,
    )
}

/// A curb strip hugging the track edge on its side, striped by style.
pub fn build_curb_mesh(path: &CenterlinePath, curb: &Curb) -> Option<Mesh> {
    let (primary, secondary) = curb_style_colors(&curb.style);
    let width = curb.width_m;
    let side = curb.side;
    let start = curb.start_m;
    build_strip_mesh(
        path,
        curb.start_m,
        curb.end_m,
        move |s| match side {
            Side::Left => s.width_left_m,
            Side::Right => -(s.width_right_m + width),
        },
        move |s| match side {
            Side::Left => s.width_left_m + width,
            Side::Right => -s.width_right_m,
        },
        |_, _, p| p.2 + CURB_LIFT_M,
        move |s| {
            let stripe = ((s.station_m - start).rem_euclid(2.0 * CURB_STRIPE_LEN_M)
                / CURB_STRIPE_LEN_M) as i32;
            if stripe == 0 {
                primary
            } else {
                secondary
            }
        },
    )
}

/// A ground patch beside the track: runoff, gravel trap, grass verge.
///
/// Both borders are measured outward from the track edge, so the patch keeps
/// its shape as the track widens and narrows, and the width may taper from
/// `width_m` at the start to `end_width_m` at the end. With a terrain field,
/// vertices near the track hug the road edge and blend into the terrain as
/// they extend outward — without one (degenerate paths only) the whole band
/// follows its own station's height.
///
/// Returned as one mesh per lateral strip ([`surface_lateral_strips`]) so
/// the height profile is sampled across the band's width, not just at its
/// borders.
pub fn build_surface_meshes(
    path: &CenterlinePath,
    surface: &Surface,
    terrain: Option<&TerrainHeightfield>,
) -> Vec<Mesh> {
    let total = path.total_length_m();
    let span = if path.is_closed() {
        let s = (surface.end_m - surface.start_m).rem_euclid(total);
        if s <= f32::EPSILON {
            total
        } else {
            s
        }
    } else {
        surface.end_m - surface.start_m
    };
    if span <= 0.0 || !span.is_finite() {
        return Vec::new();
    }

    let start = surface.start_m;
    let closed = path.is_closed();
    // Progress along the span, wrap-aware: `sample_at` hands back stations
    // already folded into [0, total), so a span crossing start/finish would
    // otherwise read as running backwards.
    let progress = move |s: &PathSample| {
        let along = if closed {
            (s.station_m - start).rem_euclid(total)
        } else {
            s.station_m - start
        };
        (along / span).clamp(0.0, 1.0)
    };

    let side = surface.side;
    let inner = surface.inner_m;
    let edge = move |s: &PathSample| match side {
        Side::Left => s.width_left_m + inner,
        Side::Right => -(s.width_right_m + inner),
    };

    let kind = surface.kind;
    let color = surface_kind_color(kind);
    let widths = surface.clone();
    let lift = surface_lift(kind);
    let strips = surface_lateral_strips(surface);

    // Border at `fraction` (0 = inner edge, 1 = outer edge) of the band's
    // width at this station.
    let border = move |s: &PathSample, fraction: f32| {
        let width = widths.width_at(progress(s));
        edge(s)
            + match side {
                Side::Left => width * fraction,
                Side::Right => -(width * fraction),
            }
    };

    let border = &border;
    (0..strips)
        .filter_map(|k| {
            let f0 = k as f32 / strips as f32;
            let f1 = (k + 1) as f32 / strips as f32;
            build_strip_mesh(
                path,
                surface.start_m,
                surface.end_m,
                move |s| border(s, f0),
                move |s| border(s, f1),
                move |s, lat, p| surface_height(terrain, s, lat, p) + lift,
                move |_| color,
            )
        })
        .collect()
}

/// Height of a ground-band vertex before its layer lift: the road-following
/// height near the track edge, blended into the terrain field with
/// distance — and never more than [`terrain::BAND_CLEAR_M`] above the
/// field. The blend alone is not enough: half-blended, a band from a high
/// section still hangs meters over a lower road passing 10–35 m away; the
/// clamp inherits the terrain's road-ceiling guarantee.
pub fn surface_height(
    terrain: Option<&TerrainHeightfield>,
    sample: &PathSample,
    lat_m: f32,
    pos: (f32, f32, f32),
) -> f32 {
    let Some(field) = terrain else {
        return pos.2;
    };
    let beyond_edge = if lat_m >= 0.0 {
        lat_m - sample.width_left_m
    } else {
        -lat_m - sample.width_right_m
    };
    let terrain_z = field.height_at(pos.0, pos.1);
    let blended = terrain::blend_toward_terrain(pos.2, terrain_z, beyond_edge);
    if beyond_edge <= terrain::BLEND_START_M {
        // The band's own shoulder: hug the road, whatever the terrain does.
        blended
    } else {
        blended.min(terrain_z + terrain::BAND_CLEAR_M)
    }
}

/// Lateral spacing of the extra vertex columns inside a ground band. The
/// blend and the ceiling clamp act per vertex; a band left as one quad
/// laterally would just be a plane from its inner border to its outer one,
/// sailing over anything in between.
pub const SURFACE_LATERAL_STEP_M: f32 = 10.0;
/// Upper bound on those columns, for 130 m grass aprons.
pub const SURFACE_MAX_STRIPS: usize = 16;

/// How many lateral strips a surface needs so its height profile is
/// actually sampled across its width.
pub fn surface_lateral_strips(surface: &Surface) -> usize {
    let width = surface
        .width_m
        .max(surface.end_width_m.unwrap_or(surface.width_m));
    ((width / SURFACE_LATERAL_STEP_M).ceil() as usize).clamp(1, SURFACE_MAX_STRIPS)
}

/// The world ground: one mesh over the terrain field's whole grid, sitting
/// [`terrain::GROUND_LIFT_M`] under every authored surface.
pub fn build_ground_mesh(field: &TerrainHeightfield) -> Option<Mesh> {
    let (cols, rows) = (field.cols(), field.rows());
    if cols < 2 || rows < 2 {
        return None;
    }

    let mut positions: Vec<[f32; 3]> = Vec::with_capacity(cols * rows);
    let mut normals: Vec<[f32; 3]> = Vec::with_capacity(cols * rows);
    let mut colors: Vec<[f32; 4]> = Vec::with_capacity(cols * rows);
    let mut uvs: Vec<[f32; 2]> = Vec::with_capacity(cols * rows);
    for r in 0..rows {
        for c in 0..cols {
            let (x, y, z) = field.vertex(c, r);
            positions.push(coords::to_bevy(x, y, z + terrain::GROUND_LIFT_M).to_array());
            let n = field.normal(c, r);
            // Directions map through the same rotation as points.
            normals.push([n.0, n.2, -n.1]);
            colors.push(terrain::GROUND_COLOR);
            uvs.push([x / 10.0, y / 10.0]);
        }
    }

    let mut indices: Vec<u32> = Vec::with_capacity((cols - 1) * (rows - 1) * 6);
    for r in 0..rows - 1 {
        for c in 0..cols - 1 {
            let v00 = (r * cols + c) as u32;
            let v10 = v00 + 1;
            let v01 = v00 + cols as u32;
            let v11 = v01 + 1;
            // Wound so the top face (+Y in Bevy space) is visible.
            indices.extend_from_slice(&[v00, v10, v01, v10, v11, v01]);
        }
    }

    Some(
        Mesh::new(
            PrimitiveTopology::TriangleList,
            RenderAssetUsages::RENDER_WORLD | RenderAssetUsages::MAIN_WORLD,
        )
        .with_inserted_attribute(Mesh::ATTRIBUTE_POSITION, positions)
        .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, normals)
        .with_inserted_attribute(Mesh::ATTRIBUTE_COLOR, colors)
        .with_inserted_attribute(Mesh::ATTRIBUTE_UV_0, uvs)
        .with_inserted_indices(Indices::U32(indices)),
    )
}

/// Preview color per surface kind. Deliberately close to the palette
/// `track_path::surface_color` uses for the track's own surface types, so a
/// gravel trap reads the same as a gravel-surfaced track node.
pub fn surface_kind_color(kind: SurfaceKind) -> [f32; 4] {
    match kind {
        SurfaceKind::Grass => [0.16, 0.42, 0.14, 1.0],
        SurfaceKind::Gravel => [0.62, 0.55, 0.4, 1.0],
        SurfaceKind::AsphaltRunoff => [0.3, 0.3, 0.33, 1.0],
        SurfaceKind::Concrete => [0.55, 0.55, 0.55, 1.0],
        SurfaceKind::Sand => [0.76, 0.68, 0.42, 1.0],
        SurfaceKind::Astroturf => [0.1, 0.34, 0.12, 1.0],
    }
}

/// A painted marking rectangle in station/lateral space.
pub fn build_marking_mesh(path: &CenterlinePath, marking: &Marking) -> Option<Mesh> {
    let (lo, hi) = if marking.lat_from_m <= marking.lat_to_m {
        (marking.lat_from_m, marking.lat_to_m)
    } else {
        (marking.lat_to_m, marking.lat_from_m)
    };
    let color = marking.color;
    build_strip_mesh(
        path,
        marking.start_m,
        marking.end_m,
        move |_| lo,
        move |_| hi,
        |_, _, p| p.2 + MARKING_LIFT_M,
        move |_| color,
    )
}

fn curb_style_colors(style: &str) -> ([f32; 4], [f32; 4]) {
    let white = [0.92, 0.92, 0.92, 1.0];
    match style {
        "yellow_black" => ([0.9, 0.75, 0.05, 1.0], [0.08, 0.08, 0.08, 1.0]),
        "green_white" => ([0.1, 0.5, 0.15, 1.0], [0.92, 0.92, 0.92, 1.0]),
        "blue_white" => ([0.1, 0.25, 0.7, 1.0], [0.92, 0.92, 0.92, 1.0]),
        _ => ([0.75, 0.12, 0.1, 1.0], white), // red_white (default)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ats::MarkingKind;
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

    fn loop_track() -> TrackFile {
        TrackFile {
            name: "Loop".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0),
                node(100.0, 0.0),
                node(100.0, 100.0),
                node(0.0, 100.0),
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 12.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        }
    }

    fn assert_finite_mesh(mesh: &Mesh) {
        let positions = mesh
            .attribute(Mesh::ATTRIBUTE_POSITION)
            .unwrap()
            .as_float3()
            .unwrap();
        assert!(!positions.is_empty());
        for p in positions {
            assert!(p.iter().all(|v| v.is_finite()));
        }
    }

    #[test]
    fn track_ribbon_is_finite_and_non_empty() {
        let path = CenterlinePath::from_track(&loop_track()).unwrap();
        let mesh = build_track_ribbon(&path).unwrap();
        assert_finite_mesh(&mesh);
    }

    #[test]
    fn curb_mesh_builds_and_wraps_the_start_finish_line() {
        let path = CenterlinePath::from_track(&loop_track()).unwrap();
        let curb = Curb {
            id: 1,
            side: Side::Left,
            // end < start on a closed loop: wraps through station 0
            start_m: path.total_length_m() - 20.0,
            end_m: 20.0,
            width_m: 1.2,
            style: "red_white".to_string(),
        };
        let mesh = build_curb_mesh(&path, &curb).unwrap();
        assert_finite_mesh(&mesh);
    }

    #[test]
    fn marking_mesh_builds_regardless_of_lateral_order() {
        let path = CenterlinePath::from_track(&loop_track()).unwrap();
        let mut marking = Marking {
            id: 1,
            kind: MarkingKind::StartFinish,
            start_m: 0.0,
            end_m: 0.6,
            lat_from_m: 6.0,
            lat_to_m: -6.0, // reversed on purpose
            color: [1.0; 4],
        };
        assert_finite_mesh(&build_marking_mesh(&path, &marking).unwrap());
        std::mem::swap(&mut marking.lat_from_m, &mut marking.lat_to_m);
        assert_finite_mesh(&build_marking_mesh(&path, &marking).unwrap());
    }

    #[test]
    fn surface_mesh_tapers_and_wraps() {
        let path = CenterlinePath::from_track(&loop_track()).unwrap();
        let surface = Surface {
            id: 1,
            kind: SurfaceKind::Gravel,
            side: Side::Right,
            // end < start: wraps through station 0
            start_m: path.total_length_m() - 30.0,
            end_m: 40.0,
            inner_m: 1.5,
            width_m: 10.0,
            end_width_m: Some(40.0),
        };
        let meshes = build_surface_meshes(&path, &surface, None);
        assert!(!meshes.is_empty());
        for mesh in &meshes {
            assert_finite_mesh(mesh);
        }

        // The outer border must actually widen along the span: in the
        // outermost lateral strip, the last cross-section has to be far
        // wider than the first (each strip carries a proportional share of
        // the taper).
        let mesh = meshes.last().unwrap();
        let positions = mesh
            .attribute(Mesh::ATTRIBUTE_POSITION)
            .unwrap()
            .as_float3()
            .unwrap();
        let cross_section = |i: usize| {
            let (a, b) = (positions[i], positions[i + 1]);
            (a[0] - b[0]).hypot(a[2] - b[2])
        };
        let (first, last) = (cross_section(0), cross_section(positions.len() - 2));
        assert!(last > first * 2.0, "taper: first={first} last={last}");
    }

    #[test]
    fn surface_layers_stack_under_the_track_in_kind_order() {
        for kind in SurfaceKind::ALL {
            assert!(
                surface_lift(kind) < 0.0 && surface_lift(kind) < CURB_LIFT_M,
                "{} must sit under the track and its curbs",
                kind.label()
            );
        }
        // Broad ground first, specific patches on top of it.
        assert!(surface_lift(SurfaceKind::Grass) < surface_lift(SurfaceKind::AsphaltRunoff));
        assert!(surface_lift(SurfaceKind::AsphaltRunoff) < surface_lift(SurfaceKind::Gravel));
        assert!(surface_lift(SurfaceKind::Gravel) < surface_lift(SurfaceKind::Astroturf));
    }

    #[test]
    fn empty_span_on_open_path_yields_no_mesh() {
        let mut track = loop_track();
        track.closed_loop = false;
        let path = CenterlinePath::from_track(&track).unwrap();
        assert!(build_strip_mesh(
            &path,
            50.0,
            50.0,
            |_| 1.0,
            |_| -1.0,
            |_, _, p| p.2,
            |_| [1.0; 4]
        )
        .is_none());
    }

    /// The regression behind floating terrain: a wide band from a high
    /// section must come down to the terrain field at its far edge instead
    /// of hanging at its own station's height.
    #[test]
    fn wide_surface_bands_blend_down_to_the_terrain() {
        use crate::terrain::TerrainHeightfield;

        let mut track = loop_track();
        // Raise one straight 40 m above the rest.
        track.nodes[2].z = 40.0;
        track.nodes[3].z = 40.0;
        let path = CenterlinePath::from_track(&track).unwrap();
        let terrain = TerrainHeightfield::from_path(&path).unwrap();

        let sample = path.sample_at(path.total_length_m() * 0.55); // on the high side
        let near = surface_height(Some(&terrain), &sample, sample.width_left_m + 1.0, {
            let mut p = offset_point(&sample, sample.width_left_m + 1.0);
            p.2 = sample.pos.2;
            p
        });
        let far_lat = sample.width_left_m + 120.0;
        let far_pos = offset_point(&sample, far_lat);
        let far = surface_height(
            Some(&terrain),
            &sample,
            far_lat,
            (far_pos.0, far_pos.1, sample.pos.2),
        );

        // Within the shoulder the band hugs its own road exactly. Far out
        // it obeys the terrain, which 120 m from a 40 m ridge is well
        // below it.
        assert!(
            (near - sample.pos.2).abs() < 0.01,
            "near {near} vs {}",
            sample.pos.2
        );
        assert!(
            far < sample.pos.2 - 1.0,
            "far edge {far} never came down from {}",
            sample.pos.2
        );
    }

    /// The regression behind buried roads: the blend alone leaves a band
    /// vertex 10–35 m out only *partially* lowered, so a band from a high
    /// section still hung meters over a lower road passing nearby. The
    /// ceiling clamp must bring it under that road.
    #[test]
    fn band_never_hangs_over_a_lower_parallel_road() {
        use crate::terrain::TerrainHeightfield;

        // Two long parallel legs 30 m apart, one 25 m above the other.
        let track = TrackFile {
            name: "TwoLevels".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0),
                node(500.0, 0.0),
                {
                    let mut n = node(500.0, 30.0);
                    n.z = 25.0;
                    n
                },
                {
                    let mut n = node(0.0, 30.0);
                    n.z = 25.0;
                    n
                },
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        };
        let path = CenterlinePath::from_track(&track).unwrap();
        let terrain = TerrainHeightfield::from_path(&path).unwrap();

        // A cross-section on the high leg, mid-straight.
        let sample = *path
            .samples()
            .iter()
            .find(|s| s.pos.2 > 20.0 && (s.pos.0 - 250.0).abs() < 30.0)
            .expect("high leg sample");

        // A band vertex 25 m beyond the edge, on whichever side reaches
        // over the low leg at y = 0.
        let (lat, pos) = [25.0f32, -25.0f32]
            .map(|beyond| {
                let lat = beyond.signum() * (sample.width_left_m + beyond.abs());
                (lat, offset_point(&sample, lat))
            })
            .into_iter()
            .min_by(|a, b| a.1 .1.abs().partial_cmp(&b.1 .1.abs()).unwrap())
            .unwrap();
        assert!(pos.1.abs() < 15.0, "vertex not over the low leg: {pos:?}");

        let z = surface_height(Some(&terrain), &sample, lat, pos);
        assert!(
            z < 0.0,
            "band at z = {z} still hangs over the road at z = 0"
        );
    }

    #[test]
    fn ground_mesh_covers_the_track_and_is_finite() {
        use crate::terrain::TerrainHeightfield;

        let path = CenterlinePath::from_track(&loop_track()).unwrap();
        let terrain = TerrainHeightfield::from_path(&path).unwrap();
        let mesh = build_ground_mesh(&terrain).unwrap();
        assert_finite_mesh(&mesh);

        // The ground must extend well past the track's own bounding box.
        let positions = mesh
            .attribute(Mesh::ATTRIBUTE_POSITION)
            .unwrap()
            .as_float3()
            .unwrap();
        let min_x = positions.iter().map(|p| p[0]).fold(f32::MAX, f32::min);
        let max_x = positions.iter().map(|p| p[0]).fold(f32::MIN, f32::max);
        assert!(min_x < -100.0 && max_x > 200.0, "ground {min_x}..{max_x}");
    }
}

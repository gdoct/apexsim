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
/// from `lat_a(sample)` to `lat_b(sample)` (meters, positive = left), lifted
/// `z_lift` above the surface. `color` is evaluated per cross-section.
///
/// On a closed path, `end_m <= start_m` wraps through the start/finish
/// line. Returns `None` for empty spans or degenerate paths.
pub fn build_strip_mesh(
    path: &CenterlinePath,
    start_m: f32,
    end_m: f32,
    lat_a: impl Fn(&PathSample) -> f32,
    lat_b: impl Fn(&PathSample) -> f32,
    z_lift: f32,
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
        let mut a = offset_point(&sample, lat_a(&sample));
        let mut b = offset_point(&sample, lat_b(&sample));
        a.2 += z_lift;
        b.2 += z_lift;

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
        0.0,
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
        CURB_LIFT_M,
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
/// `width_m` at the start to `end_width_m` at the end.
pub fn build_surface_mesh(path: &CenterlinePath, surface: &Surface) -> Option<Mesh> {
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
        return None;
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
    build_strip_mesh(
        path,
        surface.start_m,
        surface.end_m,
        edge,
        move |s| {
            let width = widths.width_at(progress(s));
            match side {
                Side::Left => s.width_left_m + inner + width,
                Side::Right => -(s.width_right_m + inner + width),
            }
        },
        surface_lift(kind),
        move |_| color,
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
        MARKING_LIFT_M,
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
        let mesh = build_surface_mesh(&path, &surface).unwrap();
        assert_finite_mesh(&mesh);

        // The outer border must actually widen along the span: the last
        // cross-section has to sit further from the centerline than the first.
        // The outer border must actually widen along the span: the last
        // cross-section has to be far wider than the first.
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
        assert!(
            build_strip_mesh(&path, 50.0, 50.0, |_| 1.0, |_| -1.0, 0.0, |_| [1.0; 4]).is_none()
        );
    }
}

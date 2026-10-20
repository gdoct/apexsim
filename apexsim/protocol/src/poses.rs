//! Points beside the road for a camera: a pose on the centerline moved
//! sideways and up (`pose_at`), or seated on the baked ground
//! (`pose_on_ground`), and which side of a bend to stand. What
//! `apexsim-replay pose` prints and the track guide's tripods use.

use serde::{Deserialize, Serialize};

use crate::data::*;
use crate::ground::GroundQuery as _;

/// A point beside the road, in the server frame, for a camera.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackPose {
    pub station_m: f32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// Road heading at the station, radians counter-clockwise from +X.
    pub yaw_rad: f32,
    pub yaw_deg: f32,
    pub width_left_m: f32,
    pub width_right_m: f32,
    pub lap_length_m: f32,
}

/// Which side of the road a camera stands on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Left,
    Right,
    /// The outside of the bend at the station (left on a straight).
    Outside,
    /// The inside of the bend at the station (right on a straight).
    Inside,
}

impl Side {
    pub fn parse(text: &str) -> Result<Side, String> {
        match text.trim().to_lowercase().as_str() {
            "left" | "l" => Ok(Side::Left),
            "right" | "r" => Ok(Side::Right),
            "outside" | "out" => Ok(Side::Outside),
            "inside" | "in" => Ok(Side::Inside),
            other => Err(format!(
                "unknown side '{other}' (left, right, outside, inside)"
            )),
        }
    }
}

/// +1 when the road turns left through `station_m` (heading change over
/// ±`half_m`), -1 when it turns right, 0 on a straight.
pub fn turn_sign(track: &TrackConfig, station_m: f32, half_m: f32) -> f32 {
    let before = pose_at(track, station_m - half_m, 0.0, 0.0).yaw_rad;
    let after = pose_at(track, station_m + half_m, 0.0, 0.0).yaw_rad;
    let mut delta = after - before;
    while delta > std::f32::consts::PI {
        delta -= std::f32::consts::TAU;
    }
    while delta < -std::f32::consts::PI {
        delta += std::f32::consts::TAU;
    }
    if delta.abs() < 0.05 {
        0.0
    } else {
        delta.signum()
    }
}

/// A lateral distance (always positive) on a side, as a signed lateral
/// (positive left) for [`pose_at`].
pub fn lateral_on(track: &TrackConfig, station_m: f32, distance_m: f32, side: Side) -> f32 {
    let d = distance_m.abs();
    match side {
        Side::Left => d,
        Side::Right => -d,
        // A left-hander's outside is on the right.
        Side::Outside => {
            if turn_sign(track, station_m, 40.0) > 0.0 {
                -d
            } else {
                d
            }
        }
        Side::Inside => {
            if turn_sign(track, station_m, 40.0) > 0.0 {
                d
            } else {
                -d
            }
        }
    }
}

/// [`pose_at`], but lifted off the baked ground (`<Stem>.ground.msgpack`,
/// the terrain the client renders) rather than the road's height where the
/// track has one: a camera 20 m off the road at Eau Rouge would otherwise
/// stand inside the hill or float over the valley.
pub fn pose_on_ground(
    track: &TrackConfig,
    station_m: f32,
    lateral_m: f32,
    height_m: f32,
) -> TrackPose {
    let mut pose = pose_at(track, station_m, lateral_m, height_m);
    if let Some(ground) = &track.ground {
        pose.z = ground.sample(pose.x, pose.y) + height_m;
    }
    pose
}

/// The centerline pose at `station_m`, moved `lateral_m` to the left
/// (negative: right) and lifted `height_m`.
pub fn pose_at(track: &TrackConfig, station_m: f32, lateral_m: f32, height_m: f32) -> TrackPose {
    let lap = track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(0.0);
    let s = if lap > 0.0 {
        station_m.rem_euclid(lap)
    } else {
        station_m
    };
    let (x, y, z, yaw) = crate::physics::pose_at_station(&track.centerline, s);
    let idx = track
        .centerline
        .partition_point(|p| p.distance_from_start_m < s)
        .min(track.centerline.len().saturating_sub(1));
    let p = &track.centerline[idx];
    TrackPose {
        station_m: s,
        x: x - yaw.sin() * lateral_m,
        y: y + yaw.cos() * lateral_m,
        z: z + height_m,
        yaw_rad: yaw,
        yaw_deg: yaw.to_degrees(),
        width_left_m: p.width_left_m,
        width_right_m: p.width_right_m,
        lap_length_m: lap,
    }
}

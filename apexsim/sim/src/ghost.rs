//! A recorded lap's trace, for the ghost car: the car's pose a few dozen
//! times a second from the line to the line. The session records it
//! (`game_session`); the lap record store (`records`) keeps it on disk.

use serde::{Deserialize, Serialize};

/// How often a ghost lap is sampled. 20 Hz is about 3 m at racing speed, and
/// the client interpolates between samples exactly as it does for a live car.
pub const GHOST_SAMPLE_HZ: f32 = 20.0;

/// One moment of a recorded lap. Everything the client needs to drive a
/// puppet car through it; the same fields a telemetry frame carries.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct GhostSample {
    /// Milliseconds since the lap started.
    pub t_ms: u32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    pub pitch_rad: f32,
    pub roll_rad: f32,
    pub speed_mps: f32,
    pub steering: f32,
    pub throttle: f32,
    pub brake: f32,
    pub gear: i8,
    pub engine_rpm: f32,
}

/// A recorded lap, start line to start line.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GhostLap {
    pub lap_time_ms: u32,
    pub sample_hz: f32,
    pub samples: Vec<GhostSample>,
}

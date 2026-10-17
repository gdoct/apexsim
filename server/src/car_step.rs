//! One car's own tick: the car model and its place on the lap, as every
//! session mode runs it ([`step_car`]), and the same for a car alone on the
//! track, walls included ([`step_lone_car`]). The client's prediction of its
//! own car runs [`step_lone_car`] through the C API in `server/ffi`
//! (docs/proposals/client-prediction.md), so there is one car physics.
//!
//! What a session does around a car is not here: the air of the tick (the
//! session sets the wind, [`air_for_tick`], before the cars), the pit lane,
//! DRS, the wake, the road state's passes, debris and recoveries, the
//! car-car pass. A predicting client holds those at what the server last
//! said; a hit by another car arrives as a correction.
//!
//! [`encode_state`] / [`decode_state`] carry a car's full state as a
//! versioned blob, the runtime fields serde skips on `CarState` included,
//! so a state saved and loaded steps on bit for bit.

use serde::{Deserialize, Serialize};

use crate::data::{CarConfig, CarState, PlayerInputData, TrackConfig};
use crate::laps::{LapEvent, LapTiming};
use crate::physics;
use crate::road_state::RoadWork;

/// The session clock a step runs on: the tick being simulated (the
/// session's `current_tick` during it) and the session's rate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StepClock {
    pub tick: u32,
    pub tick_rate_hz: u16,
}

impl StepClock {
    /// The fixed step, s: `1 / tick_rate_hz`, worked out the one way every
    /// caller works it, so a step is bit-identical wherever it runs.
    pub fn dt(self) -> f32 {
        1.0 / self.tick_rate_hz as f32
    }
}

/// The wind of `clock`'s tick on `track` (`wind::gusting` of the track's
/// mean): what a session sets before it steps its cars.
pub fn air_for_tick(track: &mut TrackConfig, clock: StepClock) {
    let surface = &mut track.track_surface;
    let seconds = clock.tick as f32 / clock.tick_rate_hz.max(1) as f32;
    surface.wind_now_mps = crate::wind::gusting(surface.wind_mps, seconds);
}

/// One car's tick: the car model (`physics::update_car_3d`) with
/// `config` (the driver's tuned copy, else the shared one), then `moved`
/// with the moved car (a session samples its ghost trace there), then its
/// place on the lap (`physics::update_track_progress_3d`), which returns a
/// timing line crossed.
pub fn step_car(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    track: &TrackConfig,
    clock: StepClock,
    moved: impl FnOnce(&CarState),
) -> Option<LapEvent> {
    physics::update_car_3d(state, config, input, track, clock.dt());
    moved(state);
    physics::update_track_progress_3d(state, track, clock.tick, clock.tick_rate_hz)
}

/// A car alone on the track for one tick, as a session ticks it: [`step_car`],
/// then the contact passes after the cars have moved — the car-car pass,
/// which with no other car only clears last tick's contact
/// (`physics::clear_contact`), and the walls (`physics::collide_with_walls`,
/// with the car's `shared` config as the session's wall pass reads it).
/// `nearby` is scratch for the wall query.
pub fn step_lone_car(
    state: &mut CarState,
    simulated: &CarConfig,
    shared: &CarConfig,
    input: &PlayerInputData,
    track: &TrackConfig,
    clock: StepClock,
    nearby: &mut Vec<u32>,
) -> Option<LapEvent> {
    let event = step_car(state, simulated, input, track, clock, |_| {});
    physics::clear_contact(state);
    physics::collide_with_walls(state, shared, track, clock.dt(), nearby);
    event
}

/// The state blob's version: bumped when a blob of the old one can no
/// longer be read into a `CarState` that steps the same.
pub const STATE_BLOB_VERSION: u16 = 1;

/// The blob's first bytes.
const STATE_BLOB_MAGIC: [u8; 4] = *b"APXC";

/// The `CarState` fields serde skips (runtime-only there) that the car step
/// reads or carries from tick to tick. The force-feedback collector is left
/// out: nothing in the sim reads it, and a loaded state starts it empty.
#[derive(Serialize, Deserialize)]
struct Runtime {
    nearest_centerline_idx: Option<u32>,
    next_checkpoint: u8,
    laps: LapTiming,
    wheels_off_track: bool,
    drs_armed: u32,
    drs_last_progress: f32,
    road_work: RoadWork,
}

/// Why a blob was refused.
#[derive(Debug, thiserror::Error)]
pub enum StateBlobError {
    #[error("not a car state blob")]
    NotABlob,
    #[error("car state blob version {0}, this build reads {STATE_BLOB_VERSION}")]
    Version(u16),
    #[error("car state blob does not decode: {0}")]
    Decode(String),
}

/// A car's full state as a blob: the magic, the version (u16, little
/// endian), then the state and its runtime fields as MessagePack with named
/// fields (floats as their exact bits).
pub fn encode_state(state: &CarState) -> Vec<u8> {
    let runtime = Runtime {
        nearest_centerline_idx: state.nearest_centerline_idx,
        next_checkpoint: state.next_checkpoint,
        laps: state.laps.clone(),
        wheels_off_track: state.wheels_off_track,
        drs_armed: state.drs_armed,
        drs_last_progress: state.drs_last_progress,
        road_work: state.road_work,
    };
    let mut out = Vec::with_capacity(4096);
    out.extend_from_slice(&STATE_BLOB_MAGIC);
    out.extend_from_slice(&STATE_BLOB_VERSION.to_le_bytes());
    // Serializing plain data into a Vec does not fail.
    rmp_serde::encode::write_named(&mut out, &(state, runtime)).expect("a car state serializes");
    out
}

/// The state a blob from [`encode_state`] holds.
pub fn decode_state(blob: &[u8]) -> Result<CarState, StateBlobError> {
    if blob.len() < 6 || blob[..4] != STATE_BLOB_MAGIC {
        return Err(StateBlobError::NotABlob);
    }
    let version = u16::from_le_bytes([blob[4], blob[5]]);
    if version != STATE_BLOB_VERSION {
        return Err(StateBlobError::Version(version));
    }
    let (mut state, runtime): (CarState, Runtime) =
        rmp_serde::from_slice(&blob[6..]).map_err(|e| StateBlobError::Decode(e.to_string()))?;
    state.nearest_centerline_idx = runtime.nearest_centerline_idx;
    state.next_checkpoint = runtime.next_checkpoint;
    state.laps = runtime.laps;
    state.wheels_off_track = runtime.wheels_off_track;
    state.drs_armed = runtime.drs_armed;
    state.drs_last_progress = runtime.drs_last_progress;
    state.road_work = runtime.road_work;
    Ok(state)
}

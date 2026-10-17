//! The server's car step behind a C API, for the client's prediction of its
//! own car (docs/proposals/client-prediction.md).
//!
//! One physics: [`apx_car_step`] calls `car_step::step_lone_car`, the very
//! function a `GameSession` runs for each car, with the session's own `dt`
//! and the wind the session gives the tick. Nothing here simulates; it
//! loads, converts and guards.
//!
//! Every function catches panics and returns an [`ApxStatus`]; the message
//! of the last failure on a thread is [`apx_last_error`]. Handles are owned
//! by the caller and freed with their `*_free`. A track handle is mutated
//! by each step (the tick's wind), so a track and the cars stepped on it
//! belong to one thread at a time.
//!
//! The C declarations are `apexsim_physics.h` beside this crate; keep the
//! two in step and bump [`APX_ABI_VERSION`] on any change to a signature or
//! a `#[repr(C)]` layout.

use std::cell::RefCell;
use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};

use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::{CarSetup, KNOB_COUNT};
use apexsim_server::car_step::{self, StepClock};
use apexsim_server::config::RoadContactMode;
use apexsim_server::data::{CarConfig, CarState, PlayerInputData, SessionConditions, TrackConfig};
use apexsim_server::track_loader::TrackLoader;

/// The C API's version: [`apx_abi_version`]. Bumped whenever a function's
/// signature or a `#[repr(C)]` struct changes.
pub const APX_ABI_VERSION: u32 = 1;

/// What every function returns.
pub type ApxStatus = i32;
pub const APX_OK: ApxStatus = 0;
/// A required pointer was null.
pub const APX_ERR_NULL: ApxStatus = 1;
/// A string was not UTF-8.
pub const APX_ERR_UTF8: ApxStatus = 2;
/// The track (YAML or a sidecar) did not load.
pub const APX_ERR_TRACK: ApxStatus = 3;
/// The car.toml did not parse or failed validation.
pub const APX_ERR_CAR: ApxStatus = 4;
/// The session conditions did not decode.
pub const APX_ERR_CONDITIONS: ApxStatus = 5;
/// An argument was out of range (no grid slot, a non-finite input, a zero
/// tick rate, too many setup clicks).
pub const APX_ERR_INVALID: ApxStatus = 6;
/// A state blob was not one this library wrote, or of another version.
pub const APX_ERR_STATE: ApxStatus = 7;
/// The buffer is too small; the size needed was written.
pub const APX_ERR_BUFFER: ApxStatus = 8;
/// The library panicked; the handle involved may be inconsistent.
pub const APX_ERR_PANIC: ApxStatus = 9;

/// A loaded track: its sidecars, and the session's weather and road state
/// baked in as the server bakes them.
pub struct ApxTrack {
    track: TrackConfig,
}

impl ApxTrack {
    /// The track itself, for a Rust caller: the tests hold its
    /// server-owned state (the road state the cars' passes change) to the
    /// session's, as a predicting client holds it to what the server sends.
    pub fn track_mut(&mut self) -> &mut TrackConfig {
        &mut self.track
    }
}

/// One car: its config as loaded, the copy its setup tunes (what the car
/// model runs), and its full state.
pub struct ApxCar {
    shared: CarConfig,
    simulated: Option<CarConfig>,
    state: CarState,
    /// Scratch for the wall query, kept so a step does not allocate.
    nearby: Vec<u32>,
}

/// The driver's controls for one tick: `ClientMessage::PlayerInput`'s
/// fields, flattened.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct ApxInput {
    /// 0..1.
    pub throttle: f32,
    /// 0..1.
    pub brake: f32,
    /// -1 (right) .. 1 (left).
    pub steering: f32,
    /// 0..1, read only when `has_clutch` is set.
    pub clutch: f32,
    /// -1 reverse, 0 neutral, 1.. forward; read only when `has_gear` is set.
    pub gear: i8,
    pub has_gear: u8,
    pub has_clutch: u8,
    pub drs: u8,
    /// -1 leaves the lights to the conditions, 0 off, 1 on.
    pub headlights: i8,
    pub flash: u8,
    /// -1 keeps the car's hybrid mode, else `hybrid::ErsMode` as a byte.
    pub ers_mode: i8,
    pub ers_boost: u8,
    pub handbrake: u8,
}

/// Bits of [`ApxCarPose::flags`].
pub const APX_POSE_ON_TRACK: u8 = 1;
pub const APX_POSE_AIRBORNE: u8 = 2;
pub const APX_POSE_COLLIDING: u8 = 4;
pub const APX_POSE_DRS_OPEN: u8 = 8;

/// What a renderer reads of the car: the server frame (metres, +X along
/// the track at the line, +Y left, +Z up, yaw counter-clockwise).
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ApxCarPose {
    pub pos_x: f32,
    pub pos_y: f32,
    pub pos_z: f32,
    pub yaw_rad: f32,
    pub pitch_rad: f32,
    pub roll_rad: f32,
    pub vel_x: f32,
    pub vel_y: f32,
    pub vel_z: f32,
    pub speed_mps: f32,
    pub angular_vel_yaw: f32,
    pub angular_vel_pitch: f32,
    pub angular_vel_roll: f32,
    pub steering_input: f32,
    pub throttle_input: f32,
    pub brake_input: f32,
    pub engine_rpm: f32,
    pub track_progress_m: f32,
    pub lateral_offset_m: f32,
    /// FL FR RL RR, rad/s.
    pub wheel_angular_vel: [f32; 4],
    pub current_lap: u16,
    pub gear: i8,
    /// `APX_POSE_*` bits.
    pub flags: u8,
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_last_error(message: &str) {
    let message = CString::new(message.replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|e| *e.borrow_mut() = message);
}

/// An error on its way out: its status and what to tell the caller.
struct Fail(ApxStatus, String);

impl Fail {
    fn new(status: ApxStatus, message: impl Into<String>) -> Self {
        Self(status, message.into())
    }
}

/// Run `f` with a panic boundary: no unwind crosses into C.
fn guard(f: impl FnOnce() -> Result<(), Fail>) -> ApxStatus {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => {
            set_last_error("");
            APX_OK
        }
        Ok(Err(Fail(status, message))) => {
            set_last_error(&message);
            status
        }
        Err(panic) => {
            let what = panic
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| panic.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "unknown panic".to_string());
            set_last_error(&format!("panic: {what}"));
            APX_ERR_PANIC
        }
    }
}

fn non_null<T>(p: *const T, what: &str) -> Result<(), Fail> {
    if p.is_null() {
        Err(Fail::new(APX_ERR_NULL, format!("{what} is null")))
    } else {
        Ok(())
    }
}

/// `len` bytes at `p` (an empty slice for a null `p` with `len` 0).
///
/// # Safety
/// `p` must point at `len` readable bytes when non-null.
unsafe fn bytes<'a>(p: *const u8, len: usize, what: &str) -> Result<&'a [u8], Fail> {
    if len == 0 {
        return Ok(&[]);
    }
    non_null(p, what)?;
    Ok(std::slice::from_raw_parts(p, len))
}

/// The C API's version, [`APX_ABI_VERSION`]: a client checks it before any
/// other call.
#[no_mangle]
pub extern "C" fn apx_abi_version() -> u32 {
    APX_ABI_VERSION
}

/// The version of the state blobs [`apx_car_state_save`] writes.
#[no_mangle]
pub extern "C" fn apx_state_blob_version() -> u32 {
    car_step::STATE_BLOB_VERSION as u32
}

/// The last failure's message on this thread, UTF-8, empty after a
/// success. Valid until the next call on the thread.
#[no_mangle]
pub extern "C" fn apx_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

/// Load a track from its YAML and the sidecars beside it, and bake the
/// session's conditions into it as the server does for a session's own
/// copy (`game_session::session_track`).
///
/// `road_contact`: 0 the centerline, 1 the road mesh (the server's
/// `[physics] road_contact`; without a `.road.msgpack` the centerline).
/// `conditions`: `SessionConditions` as MessagePack with named fields (as
/// `SessionJoined.conditions` carries them, already resolved), or null with
/// `conditions_len` 0 for the defaults.
///
/// # Safety
/// `yaml_path` is a NUL-terminated string; `conditions` points at
/// `conditions_len` bytes; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_track_load(
    yaml_path: *const c_char,
    road_contact: u8,
    conditions: *const u8,
    conditions_len: usize,
    out: *mut *mut ApxTrack,
) -> ApxStatus {
    guard(|| {
        non_null(yaml_path, "yaml_path")?;
        non_null(out, "out")?;
        *out = std::ptr::null_mut();
        let path = CStr::from_ptr(yaml_path)
            .to_str()
            .map_err(|_| Fail::new(APX_ERR_UTF8, "yaml_path is not UTF-8"))?;
        let mode = match road_contact {
            0 => RoadContactMode::Centerline,
            1 => RoadContactMode::Mesh,
            other => {
                return Err(Fail::new(
                    APX_ERR_INVALID,
                    format!("road_contact {other}: 0 centerline, 1 mesh"),
                ))
            }
        };
        let raw = bytes(conditions, conditions_len, "conditions")?;
        let conditions: SessionConditions = if raw.is_empty() {
            SessionConditions::default()
        } else {
            rmp_serde::from_slice(raw)
                .map_err(|e| Fail::new(APX_ERR_CONDITIONS, format!("conditions: {e}")))?
        };
        let track = TrackLoader::load_from_file_with(path, mode)
            .map_err(|e| Fail::new(APX_ERR_TRACK, format!("{path}: {e:?}")))?;
        let track = apexsim_server::game_session::session_track(track, &conditions);
        *out = Box::into_raw(Box::new(ApxTrack { track }));
        Ok(())
    })
}

/// Free a track. Null is ignored.
///
/// # Safety
/// `track` came from [`apx_track_load`] and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn apx_track_free(track: *mut ApxTrack) {
    if !track.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(track))));
    }
}

/// Create a car from its car.toml text and setup clicks, standing on the
/// track's grid slot `grid_position` (1-based; 0 the first slot).
///
/// `setup_clicks`: up to `KNOB_COUNT` knob values in `CarSetup` order (the
/// rest 0, the file's value), clamped as the server clamps a
/// `SetCarSetup`; null with `n_clicks` 0 for the stock car.
///
/// The new car's state is a fresh one on that slot; a predicting client
/// replaces it with the server's ([`apx_car_state_load`]) or its pose.
///
/// # Safety
/// `track` is a live track; `car_toml` points at `car_toml_len` bytes;
/// `setup_clicks` at `n_clicks` values; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_create(
    track: *const ApxTrack,
    car_toml: *const u8,
    car_toml_len: usize,
    setup_clicks: *const i8,
    n_clicks: usize,
    grid_position: u8,
    out: *mut *mut ApxCar,
) -> ApxStatus {
    guard(|| {
        non_null(track, "track")?;
        non_null(out, "out")?;
        *out = std::ptr::null_mut();
        let track = &(*track).track;
        let text = std::str::from_utf8(bytes(car_toml, car_toml_len, "car_toml")?)
            .map_err(|_| Fail::new(APX_ERR_UTF8, "car_toml is not UTF-8"))?;
        let shared = CarLoader::load_from_str(text, "car.toml")
            .map_err(|e| Fail::new(APX_ERR_CAR, e.to_string()))?;
        if n_clicks > KNOB_COUNT {
            return Err(Fail::new(
                APX_ERR_INVALID,
                format!("{n_clicks} setup clicks, at most {KNOB_COUNT}"),
            ));
        }
        let mut clicks = [0i8; KNOB_COUNT];
        if n_clicks > 0 {
            non_null(setup_clicks, "setup_clicks")?;
            clicks[..n_clicks].copy_from_slice(std::slice::from_raw_parts(setup_clicks, n_clicks));
        }
        let simulated = CarSetup::from_clicks(clicks)
            .clamp()
            .simulated_config(&shared);
        let slot = track
            .start_positions
            .iter()
            .find(|s| s.position == grid_position)
            .or_else(|| {
                (grid_position == 0)
                    .then(|| track.start_positions.first())
                    .flatten()
            })
            .ok_or_else(|| {
                Fail::new(
                    APX_ERR_INVALID,
                    format!("the track has no grid slot {grid_position}"),
                )
            })?;
        let mut state = CarState::new(Default::default(), shared.id, slot);
        apexsim_server::physics::seed_track_progress(&mut state, track);
        *out = Box::into_raw(Box::new(ApxCar {
            shared,
            simulated,
            state,
            nearby: Vec::new(),
        }));
        Ok(())
    })
}

/// Free a car. Null is ignored.
///
/// # Safety
/// `car` came from [`apx_car_create`] and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn apx_car_free(car: *mut ApxCar) {
    if !car.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(car))));
    }
}

/// Step the car one tick on `track`, as the server's session steps a car
/// alone on the track: the car model, its place on the lap, its walls
/// (`car_step::step_lone_car`), with the wind of tick `tick`.
///
/// `tick` is the session tick being simulated (the server's
/// `current_tick` during that tick: the one telemetry reports for its
/// result); `tick_rate_hz` the session's rate. The step's `dt` is
/// `1 / tick_rate_hz` worked out exactly as the server works it, which is
/// why the caller passes the rate and not a `dt`. The input is sanitised as
/// the server sanitises a `PlayerInput`.
///
/// # Safety
/// `car` and `track` are live handles; `input` is readable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_step(
    car: *mut ApxCar,
    track: *mut ApxTrack,
    input: *const ApxInput,
    tick: u32,
    tick_rate_hz: u16,
) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        non_null(track, "track")?;
        non_null(input, "input")?;
        if tick_rate_hz == 0 {
            return Err(Fail::new(APX_ERR_INVALID, "tick_rate_hz is 0"));
        }
        let input = player_input(&*input)
            .ok_or_else(|| Fail::new(APX_ERR_INVALID, "input has a non-finite value"))?;
        let car = &mut *car;
        let track = &mut (*track).track;
        let clock = StepClock { tick, tick_rate_hz };
        car_step::air_for_tick(track, clock);
        let simulated = car.simulated.as_ref().unwrap_or(&car.shared);
        car_step::step_lone_car(
            &mut car.state,
            simulated,
            &car.shared,
            &input,
            track,
            clock,
            &mut car.nearby,
        );
        Ok(())
    })
}

/// The driver's controls as the server takes them off the wire.
fn player_input(input: &ApxInput) -> Option<PlayerInputData> {
    PlayerInputData {
        throttle: input.throttle,
        brake: input.brake,
        steering: input.steering,
        gear: (input.has_gear != 0).then_some(input.gear),
        clutch: (input.has_clutch != 0).then_some(input.clutch),
        drs: input.drs != 0,
        headlights: (input.headlights >= 0).then_some(input.headlights != 0),
        flash: input.flash != 0,
        ers_mode: (input.ers_mode >= 0).then_some(input.ers_mode as u8),
        ers_boost: input.ers_boost != 0,
        handbrake: input.handbrake != 0,
    }
    .sanitized()
}

/// The car's pose, velocities and what the driver sees on the dash.
///
/// # Safety
/// `car` is a live handle; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_pose(car: *const ApxCar, out: *mut ApxCarPose) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        non_null(out, "out")?;
        let s = &(*car).state;
        let mut flags = 0;
        for (on, bit) in [
            (s.is_on_track, APX_POSE_ON_TRACK),
            (s.is_airborne, APX_POSE_AIRBORNE),
            (s.is_colliding, APX_POSE_COLLIDING),
            (s.drs_open, APX_POSE_DRS_OPEN),
        ] {
            if on {
                flags |= bit;
            }
        }
        *out = ApxCarPose {
            pos_x: s.pos_x,
            pos_y: s.pos_y,
            pos_z: s.pos_z,
            yaw_rad: s.yaw_rad,
            pitch_rad: s.pitch_rad,
            roll_rad: s.roll_rad,
            vel_x: s.vel_x,
            vel_y: s.vel_y,
            vel_z: s.vel_z,
            speed_mps: s.speed_mps,
            angular_vel_yaw: s.angular_vel_yaw,
            angular_vel_pitch: s.angular_vel_pitch,
            angular_vel_roll: s.angular_vel_roll,
            steering_input: s.steering_input,
            throttle_input: s.throttle_input,
            brake_input: s.brake_input,
            engine_rpm: s.engine_rpm,
            track_progress_m: s.track_progress,
            lateral_offset_m: s.lateral_offset_m,
            wheel_angular_vel: s.wheel_angular_vel,
            current_lap: s.current_lap,
            gear: s.gear,
            flags,
        };
        Ok(())
    })
}

/// Write the car's full state as a versioned blob (`car_step::encode_state`):
/// everything the car step reads, so loading it and stepping replays
/// bit for bit. `*written` gets the blob's size; with a null `buf` or a
/// `capacity` short of it nothing is copied and [`APX_ERR_BUFFER`] is
/// returned, so a first call with a null buffer asks the size. The size
/// moves a little from tick to tick (MessagePack sizes a number by its
/// value), so a ring of saved states keeps headroom.
///
/// # Safety
/// `car` is a live handle; `buf` points at `capacity` writable bytes when
/// non-null; `written` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_state_save(
    car: *const ApxCar,
    buf: *mut u8,
    capacity: usize,
    written: *mut usize,
) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        non_null(written, "written")?;
        let blob = car_step::encode_state(&(*car).state);
        *written = blob.len();
        if buf.is_null() || capacity < blob.len() {
            return Err(Fail::new(
                APX_ERR_BUFFER,
                format!("the state needs {} bytes", blob.len()),
            ));
        }
        std::ptr::copy_nonoverlapping(blob.as_ptr(), buf, blob.len());
        Ok(())
    })
}

/// Replace the car's full state with a blob [`apx_car_state_save`] (or the
/// server's `car_step::encode_state`) wrote. A blob of another version is
/// refused, as is the state of another car (its `car_config_id`), and the
/// car is left as it was.
///
/// # Safety
/// `car` is a live handle; `buf` points at `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn apx_car_state_load(
    car: *mut ApxCar,
    buf: *const u8,
    len: usize,
) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        let blob = bytes(buf, len, "buf")?;
        let state =
            car_step::decode_state(blob).map_err(|e| Fail::new(APX_ERR_STATE, e.to_string()))?;
        let car = &mut *car;
        if state.car_config_id != car.shared.id {
            return Err(Fail::new(
                APX_ERR_STATE,
                format!(
                    "the state is car {}, this handle car {}",
                    state.car_config_id, car.shared.id
                ),
            ));
        }
        car.state = state;
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The hand-written header says what this file says: the version, the
    /// struct sizes it asserts, the status codes, every function.
    #[test]
    fn the_header_matches_the_library() {
        let header = include_str!("../apexsim_physics.h");
        assert!(header.contains(&format!("#define APX_ABI_VERSION {APX_ABI_VERSION}u")));
        assert!(header.contains(&format!(
            "sizeof(ApxInput) == {}",
            std::mem::size_of::<ApxInput>()
        )));
        assert!(header.contains(&format!(
            "sizeof(ApxCarPose) == {}",
            std::mem::size_of::<ApxCarPose>()
        )));
        for f in [
            "apx_abi_version(",
            "apx_state_blob_version(",
            "apx_last_error(",
            "apx_track_load(",
            "apx_track_free(",
            "apx_car_create(",
            "apx_car_free(",
            "apx_car_step(",
            "apx_car_pose(",
            "apx_car_state_save(",
            "apx_car_state_load(",
        ] {
            assert!(header.contains(f), "the header lacks {f}");
        }
        for (name, value) in [
            ("APX_ERR_NULL", APX_ERR_NULL),
            ("APX_ERR_UTF8", APX_ERR_UTF8),
            ("APX_ERR_TRACK", APX_ERR_TRACK),
            ("APX_ERR_CAR", APX_ERR_CAR),
            ("APX_ERR_CONDITIONS", APX_ERR_CONDITIONS),
            ("APX_ERR_INVALID", APX_ERR_INVALID),
            ("APX_ERR_STATE", APX_ERR_STATE),
            ("APX_ERR_BUFFER", APX_ERR_BUFFER),
            ("APX_ERR_PANIC", APX_ERR_PANIC),
        ] {
            assert!(
                header.contains(&format!("#define {name} {value} ")),
                "{name}"
            );
        }
    }
}

//! The car step of the simulation (`apexsim-sim`, the very code the server
//! runs) behind a C API, for the client's prediction of its own car
//! (docs/proposals/client-prediction.md).
//!
//! One physics: [`apx_car_step`] calls `car_step::step_lone_car`, the very
//! function a `GameSession` runs for each car, with the session's own `dt`
//! and the wind the session gives the tick. A whole race nobody drives (a
//! showcase, a guide's field, Watch hotlap) is [`apx_race_start`] and
//! [`apx_race_tick`]: `seeded_race::start_race` and `tick_field`, the setup
//! and tick `apexsim-replay render` runs. Nothing here simulates; it loads,
//! converts and guards.
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

use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::car_setup::{CarSetup, KNOB_COUNT};
use apexsim_sim::car_step::{self, StepClock};
use apexsim_sim::data::{CarConfig, CarState, PlayerInputData, SessionConditions, TrackConfig};
use apexsim_sim::feedback::{DriverFeedback, MAX_STEER_SAMPLES};
use apexsim_sim::own_car::{self, OwnCarSnapshot};
use apexsim_sim::track_bundle::TrackFiles;
use apexsim_sim::track_loader::RoadContactMode;
use apexsim_sim::track_loader::TrackLoader;

/// The C API's version: [`apx_abi_version`]. Bumped whenever a function's
/// signature or a `#[repr(C)]` struct changes.
pub const APX_ABI_VERSION: u32 = 4;

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

/// [`ApxCarSnapshot::centerline_idx`] when the car has none yet.
pub const APX_NO_INDEX: u32 = u32::MAX;

/// Bits of [`ApxCarSnapshot::flags`] (`own_car::FLAG_*`).
pub const APX_SNAP_ON_TRACK: u8 = own_car::FLAG_ON_TRACK;
pub const APX_SNAP_AIRBORNE: u8 = own_car::FLAG_AIRBORNE;
pub const APX_SNAP_COLLIDING: u8 = own_car::FLAG_COLLIDING;
pub const APX_SNAP_DRS_OPEN: u8 = own_car::FLAG_DRS_OPEN;
pub const APX_SNAP_WHEELS_OFF_TRACK: u8 = own_car::FLAG_WHEELS_OFF_TRACK;

/// The server's `own_car::OwnCarSnapshot` without its tick and input
/// number, which the caller keeps: what the car step carries from one tick
/// to the next, as `DriverFeedback.own_car` sends it. Server frame and
/// units; per-wheel FL FR RL RR.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ApxCarSnapshot {
    pub pos: [f32; 3],
    /// Yaw, pitch, roll, rad.
    pub attitude: [f32; 3],
    pub vel: [f32; 3],
    pub speed_mps: f32,
    /// Yaw, pitch and roll rates, rad/s.
    pub angular_vel: [f32; 3],
    pub wheel_angular_vel: [f32; 4],
    pub wheel_load_n: [f32; 4],
    pub suspension_travel_m: [f32; 4],
    pub suspension_velocity_mps: [f32; 4],
    /// Lateral, longitudinal, vertical.
    pub g_forces: [f32; 3],
    pub vertical_accel_mps2: f32,
    pub ground_slope: [f32; 2],
    pub porpoise: [f32; 2],
    /// Throttle, brake, steering, clutch as the car applied them.
    pub controls: [f32; 4],
    pub engine_rpm: f32,
    pub turbo_spool: f32,
    pub track_progress: f32,
    pub lateral_offset_m: f32,
    /// [`APX_NO_INDEX`] for none.
    pub centerline_idx: u32,
    pub auto_shift_hold_ticks: u16,
    pub auto_reverse_ticks: u16,
    pub current_lap: u16,
    pub gear: i8,
    /// `APX_SNAP_*` bits.
    pub flags: u8,
}

impl From<&OwnCarSnapshot> for ApxCarSnapshot {
    fn from(s: &OwnCarSnapshot) -> Self {
        Self {
            pos: s.pos,
            attitude: s.attitude,
            vel: s.vel,
            speed_mps: s.speed_mps,
            angular_vel: s.angular_vel,
            wheel_angular_vel: s.wheel_angular_vel,
            wheel_load_n: s.wheel_load_n,
            suspension_travel_m: s.suspension_travel_m,
            suspension_velocity_mps: s.suspension_velocity_mps,
            g_forces: s.g_forces,
            vertical_accel_mps2: s.vertical_accel_mps2,
            ground_slope: s.ground_slope,
            porpoise: s.porpoise,
            controls: s.controls,
            engine_rpm: s.engine_rpm,
            turbo_spool: s.turbo_spool,
            track_progress: s.track_progress,
            lateral_offset_m: s.lateral_offset_m,
            centerline_idx: s.centerline_idx.unwrap_or(APX_NO_INDEX),
            auto_shift_hold_ticks: s.auto_shift_hold_ticks,
            auto_reverse_ticks: s.auto_reverse_ticks,
            current_lap: s.current_lap,
            gear: s.gear,
            flags: s.flags,
        }
    }
}

/// The most steering-torque samples an [`ApxFeedback`] holds: the server's
/// `feedback::MAX_STEER_SAMPLES`.
pub const APX_FEEDBACK_SAMPLES: usize = MAX_STEER_SAMPLES;

/// What the driver should feel over the steps since the last
/// [`apx_car_feedback_take`]: the server's `DriverFeedback` without its
/// tick and own-car fields, from the very collector the server drains into
/// it (`feedback::FeedbackAccumulator::take`), in the wire's units (the
/// flat spots as percent, the surfaces as `ContactSurface` bytes).
/// Per-wheel FL FR RL RR; the server's steering sign (positive left).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ApxFeedback {
    /// One steering-column torque sample per step, oldest first.
    pub steer_torque: [f32; APX_FEEDBACK_SAMPLES],
    pub steer_samples: u32,
    pub slip_ratio: [f32; 4],
    pub slip_angle: [f32; 4],
    pub suspension_mps: [f32; 4],
    pub impact_mps: f32,
    pub steer_kick: f32,
    pub steer_input: f32,
    pub steer_stiffness: f32,
    pub front_load: f32,
    pub surface: [u8; 4],
    pub flat_spot: [u8; 4],
    pub abs_active: u8,
    pub tc_active: u8,
    pub reserved: [u8; 2],
}

impl Default for ApxFeedback {
    fn default() -> Self {
        Self::from(&DriverFeedback {
            server_tick: 0,
            steer_torque: Vec::new(),
            slip_ratio: [0.0; 4],
            slip_angle: [0.0; 4],
            surface: [0; 4],
            suspension_mps: [0.0; 4],
            abs_active: false,
            tc_active: false,
            impact_mps: 0.0,
            steer_kick: 0.0,
            steer_input: 0.0,
            steer_stiffness: 0.0,
            front_load: 1.0,
            flat_spot: [0; 4],
            own_car: None,
            own_state: None,
        })
    }
}

impl From<&DriverFeedback> for ApxFeedback {
    fn from(f: &DriverFeedback) -> Self {
        let mut steer_torque = [0.0; APX_FEEDBACK_SAMPLES];
        let n = f.steer_torque.len().min(APX_FEEDBACK_SAMPLES);
        steer_torque[..n].copy_from_slice(&f.steer_torque[f.steer_torque.len() - n..]);
        Self {
            steer_torque,
            steer_samples: n as u32,
            slip_ratio: f.slip_ratio,
            slip_angle: f.slip_angle,
            suspension_mps: f.suspension_mps,
            impact_mps: f.impact_mps,
            steer_kick: f.steer_kick,
            steer_input: f.steer_input,
            steer_stiffness: f.steer_stiffness,
            front_load: f.front_load,
            surface: f.surface,
            flat_spot: f.flat_spot,
            abs_active: f.abs_active as u8,
            tc_active: f.tc_active as u8,
            reserved: [0; 2],
        }
    }
}

impl ApxCarSnapshot {
    /// The server's snapshot, at no tick and with no input number (a step
    /// reads neither).
    fn to_own_car(self) -> OwnCarSnapshot {
        OwnCarSnapshot {
            tick: 0,
            input_seq: None,
            pos: self.pos,
            attitude: self.attitude,
            vel: self.vel,
            speed_mps: self.speed_mps,
            angular_vel: self.angular_vel,
            wheel_angular_vel: self.wheel_angular_vel,
            wheel_load_n: self.wheel_load_n,
            suspension_travel_m: self.suspension_travel_m,
            suspension_velocity_mps: self.suspension_velocity_mps,
            g_forces: self.g_forces,
            vertical_accel_mps2: self.vertical_accel_mps2,
            ground_slope: self.ground_slope,
            porpoise: self.porpoise,
            controls: self.controls,
            gear: self.gear,
            engine_rpm: self.engine_rpm,
            turbo_spool: self.turbo_spool,
            auto_shift_hold_ticks: self.auto_shift_hold_ticks,
            auto_reverse_ticks: self.auto_reverse_ticks,
            track_progress: self.track_progress,
            lateral_offset_m: self.lateral_offset_m,
            current_lap: self.current_lap,
            centerline_idx: (self.centerline_idx != APX_NO_INDEX).then_some(self.centerline_idx),
            flags: self.flags,
        }
    }

    /// Every float in it.
    fn floats(&self) -> impl Iterator<Item = f32> + '_ {
        self.pos
            .iter()
            .chain(&self.attitude)
            .chain(&self.vel)
            .chain(std::iter::once(&self.speed_mps))
            .chain(&self.angular_vel)
            .chain(&self.wheel_angular_vel)
            .chain(&self.wheel_load_n)
            .chain(&self.suspension_travel_m)
            .chain(&self.suspension_velocity_mps)
            .chain(&self.g_forces)
            .chain(std::iter::once(&self.vertical_accel_mps2))
            .chain(&self.ground_slope)
            .chain(&self.porpoise)
            .chain(&self.controls)
            .chain([
                &self.engine_rpm,
                &self.turbo_spool,
                &self.track_progress,
                &self.lateral_offset_m,
            ])
            .copied()
    }
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
        let path = c_path(yaml_path)?;
        let mode = road_contact_mode(road_contact)?;
        let conditions = session_conditions(bytes(conditions, conditions_len, "conditions")?)?;
        let track = TrackLoader::load_from_file_with(path, mode)
            .map_err(|e| Fail::new(APX_ERR_TRACK, format!("{path}: {e:?}")))?;
        let track = apexsim_sim::game_session::session_track(track, &conditions);
        *out = Box::into_raw(Box::new(ApxTrack { track }));
        Ok(())
    })
}

/// A path argument as UTF-8.
///
/// # Safety
/// `path` is a NUL-terminated string.
unsafe fn c_path<'a>(path: *const c_char) -> Result<&'a str, Fail> {
    CStr::from_ptr(path)
        .to_str()
        .map_err(|_| Fail::new(APX_ERR_UTF8, "the path is not UTF-8"))
}

fn road_contact_mode(road_contact: u8) -> Result<RoadContactMode, Fail> {
    match road_contact {
        0 => Ok(RoadContactMode::Centerline),
        1 => Ok(RoadContactMode::Mesh),
        other => Err(Fail::new(
            APX_ERR_INVALID,
            format!("road_contact {other}: 0 centerline, 1 mesh"),
        )),
    }
}

/// `SessionConditions` from their MessagePack (named), the defaults for none.
fn session_conditions(raw: &[u8]) -> Result<SessionConditions, Fail> {
    if raw.is_empty() {
        return Ok(SessionConditions::default());
    }
    rmp_serde::from_slice(raw)
        .map_err(|e| Fail::new(APX_ERR_CONDITIONS, format!("conditions: {e}")))
}

/// Load a track from its physics bundle (`track_bundle`, as the server
/// sends it in `TrackBundleChunk`s and a client caches it): the YAML and
/// the sidecars in it, loaded as [`apx_track_load`] loads them from disk,
/// with the session's conditions baked in. The bundle is refused
/// ([`APX_ERR_TRACK`], nothing loaded) unless its checksum is
/// `expected_crc` (`SessionJoined.Physics.bundle_crc`).
///
/// # Safety
/// `bundle` points at `bundle_len` bytes; `conditions` at
/// `conditions_len`; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_track_load_bundle(
    bundle: *const u8,
    bundle_len: usize,
    expected_crc: u32,
    road_contact: u8,
    conditions: *const u8,
    conditions_len: usize,
    out: *mut *mut ApxTrack,
) -> ApxStatus {
    guard(|| {
        non_null(out, "out")?;
        *out = std::ptr::null_mut();
        let raw = bytes(bundle, bundle_len, "bundle")?;
        let mode = road_contact_mode(road_contact)?;
        let conditions = session_conditions(bytes(conditions, conditions_len, "conditions")?)?;
        let files = TrackFiles::decode(raw, Some(expected_crc))
            .map_err(|e| Fail::new(APX_ERR_TRACK, e.to_string()))?;
        let yaml = std::str::from_utf8(&files.yaml)
            .map_err(|_| Fail::new(APX_ERR_UTF8, "the bundle's YAML is not UTF-8"))?;
        let mut track = TrackLoader::load_from_string(yaml)
            .map_err(|e| Fail::new(APX_ERR_TRACK, format!("the bundle's YAML: {e:?}")))?;
        // The files have no place on disk; the name only labels the log.
        let label = std::path::Path::new("bundle").join(format!("{expected_crc:08X}.yaml"));
        TrackLoader::load_sidecars_from(&mut track, &files, &label, mode);
        let track = apexsim_sim::game_session::session_track(track, &conditions);
        *out = Box::into_raw(Box::new(ApxTrack { track }));
        Ok(())
    })
}

/// The physics bundle of the YAML at `yaml_path` and the sidecars beside it
/// (the road mesh only for `road_contact` 1), as the server builds it for a
/// track it loads: for a client's own tests and tools. `*written` gets its
/// size and `*crc` its checksum; with a null `buf` or a short `capacity`
/// nothing is copied and [`APX_ERR_BUFFER`] is returned.
///
/// # Safety
/// `yaml_path` is a NUL-terminated string; `buf` points at `capacity`
/// writable bytes when non-null; `written` and `crc` are writable.
#[no_mangle]
pub unsafe extern "C" fn apx_track_bundle_build(
    yaml_path: *const c_char,
    road_contact: u8,
    buf: *mut u8,
    capacity: usize,
    written: *mut usize,
    crc: *mut u32,
) -> ApxStatus {
    guard(|| {
        non_null(yaml_path, "yaml_path")?;
        non_null(written, "written")?;
        non_null(crc, "crc")?;
        let path = c_path(yaml_path)?;
        let mode = road_contact_mode(road_contact)?;
        let files = TrackFiles::read(std::path::Path::new(path), mode)
            .map_err(|e| Fail::new(APX_ERR_TRACK, format!("{path}: {e}")))?;
        let bundle = apexsim_sim::track_bundle::TrackBundle::of(&files);
        *written = bundle.bytes.len();
        *crc = bundle.crc;
        if buf.is_null() || capacity < bundle.bytes.len() {
            return Err(Fail::new(
                APX_ERR_BUFFER,
                format!("the bundle needs {} bytes", bundle.bytes.len()),
            ));
        }
        std::ptr::copy_nonoverlapping(bundle.bytes.as_ptr(), buf, bundle.bytes.len());
        Ok(())
    })
}

/// The bundle checksum of the YAML at `yaml_path` and the sidecars beside
/// it, the road mesh only for `road_contact` 1: what the server's bundle of
/// the same files would carry (`SessionJoined.Physics.bundle_crc`), so a
/// client whose own copy matches loads it from disk and asks for nothing.
///
/// # Safety
/// `yaml_path` is a NUL-terminated string; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_track_files_crc(
    yaml_path: *const c_char,
    road_contact: u8,
    out: *mut u32,
) -> ApxStatus {
    guard(|| {
        non_null(yaml_path, "yaml_path")?;
        non_null(out, "out")?;
        let path = c_path(yaml_path)?;
        let mode = road_contact_mode(road_contact)?;
        let files = TrackFiles::read(std::path::Path::new(path), mode)
            .map_err(|e| Fail::new(APX_ERR_TRACK, format!("{path}: {e}")))?;
        *out = files.checksum();
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
        apexsim_sim::physics::seed_track_progress(&mut state, track);
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

/// The car's snapshot: what the car step carries from one tick to the next
/// (`own_car::OwnCarSnapshot::of`), as the server sends its own copy in
/// `DriverFeedback.own_car`. A predicting client compares it with the
/// server's.
///
/// # Safety
/// `car` is a live handle; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_snapshot(
    car: *const ApxCar,
    out: *mut ApxCarSnapshot,
) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        non_null(out, "out")?;
        *out = ApxCarSnapshot::from(&OwnCarSnapshot::of(&(*car).state, 0, None));
        Ok(())
    })
}

/// Lay a snapshot over the car's state (`own_car::OwnCarSnapshot::apply_to`):
/// every field it carries is set, the rest (tyres, fuel, hybrid, brakes,
/// damage, lap timing) kept. The rewind to a `DriverFeedback.own_car`
/// between two full blobs ([`apx_car_state_load`]). A snapshot with a
/// non-finite number is refused and the car left as it was.
///
/// # Safety
/// `car` is a live handle; `snapshot` is readable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_snapshot_apply(
    car: *mut ApxCar,
    snapshot: *const ApxCarSnapshot,
) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        non_null(snapshot, "snapshot")?;
        let snapshot = *snapshot;
        if snapshot.floats().any(|v| !v.is_finite()) {
            return Err(Fail::new(
                APX_ERR_INVALID,
                "the snapshot holds a non-finite number",
            ));
        }
        snapshot.to_own_car().apply_to(&mut (*car).state);
        Ok(())
    })
}

/// What the driver should feel over the steps since the last call, and a
/// fresh interval: the server's own collector (`CarState::feedback`,
/// `FeedbackAccumulator::take`), which `physics::update_car_3d` and the wall
/// pass fill every tick exactly as on the server, drained as the game loop
/// drains it into each `DriverFeedback`. Taken after every step, it is that
/// tick's: one torque sample, the tick's slip, surfaces and hits. Loading a
/// state blob empties it (the collector is not in the blob); laying a
/// snapshot leaves it.
///
/// # Safety
/// `car` is a live handle; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_car_feedback_take(
    car: *mut ApxCar,
    out: *mut ApxFeedback,
) -> ApxStatus {
    guard(|| {
        non_null(car, "car")?;
        non_null(out, "out")?;
        *out = ApxFeedback::from(&(*car).state.feedback.take(0));
        Ok(())
    })
}

/// Hold the track's road state (rubber, marbles, water, the dried line,
/// snow and ice) to the server's, from the bytes of a `RoadState` message:
/// `rows` (`RoadStateData.rows`, whole `road_state::ROW_BYTES` cells from
/// `first_cell`, wrapping the lap), `snow_rows` (`.snow_rows`; empty for a
/// road with no snow, which stops the snow counting, as the server's field
/// left off means) and `lane_snow` (`.lane_snow`, the whole pit lane;
/// empty leaves the lane as it is). The values come at the wire's byte
/// resolution, so the grip under a tyre can differ from the server's by a
/// fraction of a percent. Refused, with the track left as it was, for a
/// track with no road state or bytes that are not whole cells.
///
/// # Safety
/// `track` is a live handle; each pointer points at its length in bytes
/// (or is null with length 0).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn apx_track_road_rows(
    track: *mut ApxTrack,
    first_cell: u32,
    rows: *const u8,
    rows_len: usize,
    snow_rows: *const u8,
    snow_rows_len: usize,
    lane_snow: *const u8,
    lane_snow_len: usize,
) -> ApxStatus {
    guard(|| {
        non_null(track, "track")?;
        let rows = bytes(rows, rows_len, "rows")?;
        let snow_rows = bytes(snow_rows, snow_rows_len, "snow_rows")?;
        let lane_snow = bytes(lane_snow, lane_snow_len, "lane_snow")?;
        let Some(road) = (*track).track.road_state.as_ref() else {
            return Err(Fail::new(APX_ERR_INVALID, "the track has no road state"));
        };
        // Read into a copy, so a refusal leaves the track as it was.
        let mut road = road.clone();
        let first = first_cell as usize;
        road.read_rows(first, rows).ok_or_else(|| {
            Fail::new(
                APX_ERR_INVALID,
                format!(
                    "{} bytes of rows are not whole cells of this lap",
                    rows.len()
                ),
            )
        })?;
        road.read_snow_rows(first, snow_rows).ok_or_else(|| {
            Fail::new(
                APX_ERR_INVALID,
                format!(
                    "{} bytes of snow rows are not whole cells of this lap",
                    snow_rows.len()
                ),
            )
        })?;
        if !lane_snow.is_empty() {
            road.read_lane_snow(lane_snow).ok_or_else(|| {
                Fail::new(
                    APX_ERR_INVALID,
                    format!("{} bytes of lane snow: not this lane", lane_snow.len()),
                )
            })?;
        }
        (*track).track.road_state = Some(road);
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
        assert!(header.contains(&format!(
            "sizeof(ApxCarSnapshot) == {}",
            std::mem::size_of::<ApxCarSnapshot>()
        )));
        assert!(header.contains(&format!(
            "sizeof(ApxFeedback) == {}",
            std::mem::size_of::<ApxFeedback>()
        )));
        assert!(header.contains(&format!(
            "sizeof(ApxRaceCar) == {}",
            std::mem::size_of::<ApxRaceCar>()
        )));
        assert!(header.contains(&format!(
            "#define APX_FEEDBACK_SAMPLES {APX_FEEDBACK_SAMPLES}"
        )));
        for (name, value) in [
            ("APX_SNAP_ON_TRACK", APX_SNAP_ON_TRACK),
            ("APX_SNAP_AIRBORNE", APX_SNAP_AIRBORNE),
            ("APX_SNAP_COLLIDING", APX_SNAP_COLLIDING),
            ("APX_SNAP_DRS_OPEN", APX_SNAP_DRS_OPEN),
            ("APX_SNAP_WHEELS_OFF_TRACK", APX_SNAP_WHEELS_OFF_TRACK),
        ] {
            assert!(
                header.contains(&format!("#define {name} {value}u")),
                "{name}"
            );
        }
        assert!(header.contains("#define APX_NO_INDEX 0xFFFFFFFFu"));
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
            "apx_car_snapshot(",
            "apx_car_snapshot_apply(",
            "apx_track_road_rows(",
            "apx_car_feedback_take(",
            "apx_track_load_bundle(",
            "apx_track_files_crc(",
            "apx_track_bundle_build(",
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

// ---------------------------------------------------------------------------
// A seeded race nobody drives
// ---------------------------------------------------------------------------

/// A seeded AI race: `seeded_race::start_race`, the very setup a showcase
/// render runs, counting down from its start. Owned by the caller, freed
/// with [`apx_race_free`]; one thread at a time.
pub struct ApxRace {
    race: apexsim_sim::game_session::GameSession,
}

impl ApxRace {
    /// The session, for a Rust caller (the tests compare it with the
    /// server's own render of the same seed).
    pub fn session(&self) -> &apexsim_sim::game_session::GameSession {
        &self.race
    }
}

/// One car of a race, in grid order: where it is (the server's frame,
/// metres), which way it points and how fast, and how far it has got.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ApxRaceCar {
    pub grid_position: u32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    pub speed_mps: f32,
    /// Laps begun (`current_lap`) and the place on this one, m.
    pub lap: u32,
    pub track_progress_m: f32,
    /// 1 once the car has finished the race.
    pub finished: u8,
}

/// Start a seeded race as `apexsim-replay render` starts one: the track at
/// `track_yaml` with `conditions` (as [`apx_track_load`] takes them, null
/// for the defaults) resolved for `seed`, the field of `ai_count` cars of
/// `host_car`'s class (a folder, id or name under `cars_dir`), `laps` to
/// the flag, `countdown_s` of countdown, ticked at `tick_rate`. The same
/// arguments give the same race, tick for tick, as the server's render of
/// that seed.
///
/// # Safety
/// The strings are NUL-terminated; `conditions` points at
/// `conditions_len` bytes; `out` is writable.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn apx_race_start(
    track_yaml: *const c_char,
    cars_dir: *const c_char,
    host_car: *const c_char,
    ai_count: u8,
    laps: u8,
    countdown_s: u16,
    tick_rate: u16,
    seed: u64,
    conditions: *const u8,
    conditions_len: usize,
    out: *mut *mut ApxRace,
) -> ApxStatus {
    guard(|| {
        non_null(track_yaml, "track_yaml")?;
        non_null(cars_dir, "cars_dir")?;
        non_null(host_car, "host_car")?;
        non_null(out, "out")?;
        *out = std::ptr::null_mut();
        if tick_rate == 0 || ai_count == 0 {
            return Err(Fail::new(
                APX_ERR_INVALID,
                "a race needs a tick rate and a car",
            ));
        }
        let opts = apexsim_sim::seeded_race::SimulateOptions {
            track_path: c_path(track_yaml)?.into(),
            cars_dir: c_path(cars_dir)?.into(),
            host_car: c_path(host_car)?.to_string(),
            ai_count,
            laps,
            countdown_seconds: countdown_s,
            conditions: session_conditions(bytes(conditions, conditions_len, "conditions")?)?,
            tick_rate,
            seed: Some(seed),
            ..Default::default()
        };
        let started =
            apexsim_sim::seeded_race::start_race(&opts).map_err(|e| Fail::new(APX_ERR_TRACK, e))?;
        *out = Box::into_raw(Box::new(ApxRace { race: started.race }));
        Ok(())
    })
}

/// Tick the race `ticks` times, every car driven by its AI
/// (`seeded_race::tick_field`).
///
/// # Safety
/// `race` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn apx_race_tick(race: *mut ApxRace, ticks: u32) -> ApxStatus {
    guard(|| {
        non_null(race, "race")?;
        let race = &mut (*race).race;
        for _ in 0..ticks {
            apexsim_sim::seeded_race::tick_field(race);
        }
        Ok(())
    })
}

/// The session's tick count (`current_tick`).
///
/// # Safety
/// `race` is a live handle; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_race_current_tick(race: *const ApxRace, out: *mut u32) -> ApxStatus {
    guard(|| {
        non_null(race, "race")?;
        non_null(out, "out")?;
        *out = (*race).race.session.current_tick;
        Ok(())
    })
}

/// Every car, in grid order, into `out` (room for `capacity`); `written`
/// is the field's size. [`APX_ERR_BUFFER`] (nothing written but `written`)
/// when it does not fit.
///
/// # Safety
/// `race` is a live handle; `out` has room for `capacity` cars; `written`
/// is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_race_cars(
    race: *const ApxRace,
    out: *mut ApxRaceCar,
    capacity: usize,
    written: *mut usize,
) -> ApxStatus {
    guard(|| {
        non_null(race, "race")?;
        non_null(written, "written")?;
        let cars = race_cars(&(*race).race);
        *written = cars.len();
        if cars.len() > capacity {
            return Err(Fail::new(APX_ERR_BUFFER, format!("{} cars", cars.len())));
        }
        non_null(out, "out")?;
        std::ptr::copy_nonoverlapping(cars.as_ptr(), out, cars.len());
        Ok(())
    })
}

/// The cars of a session in grid order, as [`apx_race_cars`] gives them.
pub fn race_cars(race: &apexsim_sim::game_session::GameSession) -> Vec<ApxRaceCar> {
    let mut cars: Vec<ApxRaceCar> = race
        .session
        .participants
        .values()
        .map(|c| ApxRaceCar {
            grid_position: c.grid_position as u32,
            x: c.pos_x,
            y: c.pos_y,
            z: c.pos_z,
            yaw_rad: c.yaw_rad,
            speed_mps: c.speed_mps,
            lap: c.current_lap as u32,
            track_progress_m: c.track_progress,
            finished: c.finish_position.is_some() as u8,
        })
        .collect();
    cars.sort_by_key(|c| c.grid_position);
    cars
}

/// A digest of every car's full state (its MessagePack, in the
/// participants' order) and the tick: two races that agree on it agree bit
/// for bit. FNV-1a, 64 bits.
///
/// # Safety
/// `race` is a live handle; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn apx_race_digest(race: *const ApxRace, out: *mut u64) -> ApxStatus {
    guard(|| {
        non_null(race, "race")?;
        non_null(out, "out")?;
        *out = race_digest(&(*race).race);
        Ok(())
    })
}

/// [`apx_race_digest`] of a session, for a Rust caller.
pub fn race_digest(race: &apexsim_sim::game_session::GameSession) -> u64 {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    let mut eat = |bytes: &[u8]| {
        for b in bytes {
            hash ^= *b as u64;
            hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
    };
    eat(&race.session.current_tick.to_le_bytes());
    for state in race.session.participants.values() {
        eat(&rmp_serde::to_vec(state).unwrap_or_default());
    }
    hash
}

/// Free a race. A null handle is ignored.
///
/// # Safety
/// `race` is a handle from [`apx_race_start`], not used again.
#[no_mangle]
pub unsafe extern "C" fn apx_race_free(race: *mut ApxRace) {
    if !race.is_null() {
        drop(Box::from_raw(race));
    }
}

//! The driver's own car for a predicting client
//! (docs/proposals/client-prediction.md, phase 2): what goes back in
//! `DriverFeedback` to the driver whose client numbers its inputs
//! (`PlayerInput.seq`).
//!
//! - [`OwnCarSnapshot`], on every feedback message: the car at the tick
//!   with the newest input sequence applied by then. It carries what the
//!   car step integrates from tick to tick (pose, velocities, wheel speeds,
//!   suspension, the gearbox and the engine's state, the place on the lap):
//!   a client lays it over its own state of that tick and replays its newer
//!   inputs.
//! - [`StateChunk`], a slice of the full state blob
//!   (`car_step::encode_state`) a message, a blob every
//!   [`STATE_BLOB_SECONDS`]: the slow state the snapshot leaves out (tyre
//!   temperatures and wear, fuel, the hybrid, brakes, damage), exact. The
//!   blob is several kilobytes, so it goes in slices that keep each datagram
//!   well under the MTU.
//!
//! Nothing here is read by the simulation: the feed is written by the game
//! loop around the tick, and an input's number never reaches the physics.

use serde::{Deserialize, Serialize};

use crate::data::CarState;

/// A run of spectator stream records (`crate::spectator`) in the stream's
/// own framing, `[u32 big-endian length][body]` each: MessagePack `bin` on
/// the wire, so records read from a file are forwarded without being
/// decoded, and a joining viewer's whole preamble is one message.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RecordBytes(pub Vec<u8>);

impl RecordBytes {
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

impl Serialize for RecordBytes {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_bytes(&self.0)
    }
}

impl<'de> Deserialize<'de> for RecordBytes {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct BytesVisitor;
        impl<'de> serde::de::Visitor<'de> for BytesVisitor {
            type Value = RecordBytes;
            fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                f.write_str("a record's bytes")
            }
            fn visit_bytes<E: serde::de::Error>(self, v: &[u8]) -> Result<RecordBytes, E> {
                Ok(RecordBytes(v.to_vec()))
            }
            fn visit_byte_buf<E: serde::de::Error>(self, v: Vec<u8>) -> Result<RecordBytes, E> {
                Ok(RecordBytes(v))
            }
            fn visit_seq<A: serde::de::SeqAccess<'de>>(
                self,
                mut seq: A,
            ) -> Result<RecordBytes, A::Error> {
                let mut out = Vec::with_capacity(seq.size_hint().unwrap_or(0));
                while let Some(byte) = seq.next_element::<u8>()? {
                    out.push(byte);
                }
                Ok(RecordBytes(out))
            }
        }
        deserializer.deserialize_bytes(BytesVisitor)
    }
}

/// How often a full state blob starts going out, s.
pub const STATE_BLOB_SECONDS: f32 = 0.5;

/// The most blob bytes in one feedback message. With the snapshot and the
/// rest of the message a datagram stays under 1.1 kB, below any path's MTU.
pub const STATE_CHUNK_BYTES: usize = 640;

/// Bits of [`OwnCarSnapshot::flags`].
pub const FLAG_ON_TRACK: u8 = 1;
pub const FLAG_AIRBORNE: u8 = 2;
pub const FLAG_COLLIDING: u8 = 4;
pub const FLAG_DRS_OPEN: u8 = 8;
pub const FLAG_WHEELS_OFF_TRACK: u8 = 16;

/// The car at `tick`, as the car step carries it from one tick to the next.
/// Positional on the wire (part of `DriverFeedback`); floats are the
/// server's exact `f32`s. Per-wheel arrays are FL, FR, RL, RR.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct OwnCarSnapshot {
    /// The session tick this is the car at (the tick just simulated).
    pub tick: u32,
    /// The newest `PlayerInput.seq` applied by `tick`.
    pub input_seq: Option<u32>,
    /// Position, m, server frame.
    pub pos: [f32; 3],
    /// Yaw, pitch, roll, rad.
    pub attitude: [f32; 3],
    /// Velocity, m/s, server frame, and its magnitude.
    pub vel: [f32; 3],
    pub speed_mps: f32,
    /// Yaw, pitch and roll rates, rad/s.
    pub angular_vel: [f32; 3],
    /// Wheel spin, rad/s.
    pub wheel_angular_vel: [f32; 4],
    /// Load on each wheel, N.
    pub wheel_load_n: [f32; 4],
    /// Suspension travel, m, and its speed, m/s.
    pub suspension_travel_m: [f32; 4],
    pub suspension_velocity_mps: [f32; 4],
    /// Lateral, longitudinal and vertical g.
    pub g_forces: [f32; 3],
    /// Vertical acceleration of the body, m/s².
    pub vertical_accel_mps2: f32,
    /// The ground's slope under the car, along and across it.
    pub ground_slope: [f32; 2],
    /// The aero porpoising's phase and amplitude.
    pub porpoise: [f32; 2],
    /// Throttle, brake, steering and clutch as the car applied them (after
    /// the aids and the steering rate).
    pub controls: [f32; 4],
    pub gear: i8,
    pub engine_rpm: f32,
    pub turbo_spool: f32,
    /// The auto gearbox's hold and reverse timers, ticks.
    pub auto_shift_hold_ticks: u16,
    pub auto_reverse_ticks: u16,
    /// Distance along the lap, m; off the centerline, m (positive right);
    /// the lap; the centerline point the windowed search starts from.
    pub track_progress: f32,
    pub lateral_offset_m: f32,
    pub current_lap: u16,
    pub centerline_idx: Option<u32>,
    /// `FLAG_*` bits.
    pub flags: u8,
}

impl OwnCarSnapshot {
    /// `state` at `tick`, `input_seq` applied.
    pub fn of(state: &CarState, tick: u32, input_seq: Option<u32>) -> Self {
        let s = &state.suspension;
        let mut flags = 0;
        for (on, bit) in [
            (state.is_on_track, FLAG_ON_TRACK),
            (state.is_airborne, FLAG_AIRBORNE),
            (state.is_colliding, FLAG_COLLIDING),
            (state.drs_open, FLAG_DRS_OPEN),
            (state.wheels_off_track, FLAG_WHEELS_OFF_TRACK),
        ] {
            if on {
                flags |= bit;
            }
        }
        Self {
            tick,
            input_seq,
            pos: [state.pos_x, state.pos_y, state.pos_z],
            attitude: [state.yaw_rad, state.pitch_rad, state.roll_rad],
            vel: [state.vel_x, state.vel_y, state.vel_z],
            speed_mps: state.speed_mps,
            angular_vel: [
                state.angular_vel_yaw,
                state.angular_vel_pitch,
                state.angular_vel_roll,
            ],
            wheel_angular_vel: state.wheel_angular_vel,
            wheel_load_n: [
                state.weight_front_left_n,
                state.weight_front_right_n,
                state.weight_rear_left_n,
                state.weight_rear_right_n,
            ],
            suspension_travel_m: [
                s.front_left_travel_m,
                s.front_right_travel_m,
                s.rear_left_travel_m,
                s.rear_right_travel_m,
            ],
            suspension_velocity_mps: [
                s.front_left_velocity_mps,
                s.front_right_velocity_mps,
                s.rear_left_velocity_mps,
                s.rear_right_velocity_mps,
            ],
            g_forces: [
                state.g_forces.lateral_g,
                state.g_forces.longitudinal_g,
                state.g_forces.vertical_g,
            ],
            vertical_accel_mps2: state.vertical_accel_mps2,
            ground_slope: [state.ground_slope_f, state.ground_slope_y_f],
            porpoise: [state.porpoise_phase, state.porpoise_amp],
            controls: [
                state.throttle_input,
                state.brake_input,
                state.steering_input,
                state.clutch_input,
            ],
            gear: state.gear,
            engine_rpm: state.engine_rpm,
            turbo_spool: state.turbo_spool,
            auto_shift_hold_ticks: state.auto_shift_hold_ticks,
            auto_reverse_ticks: state.auto_reverse_ticks,
            track_progress: state.track_progress,
            lateral_offset_m: state.lateral_offset_m,
            current_lap: state.current_lap,
            centerline_idx: state.nearest_centerline_idx,
            flags,
        }
    }

    /// Lay the snapshot over `state`: every field it carries is set, the
    /// rest (tyres, fuel, hybrid, brakes, damage, timing) kept.
    pub fn apply_to(&self, state: &mut CarState) {
        [state.pos_x, state.pos_y, state.pos_z] = self.pos;
        [state.yaw_rad, state.pitch_rad, state.roll_rad] = self.attitude;
        [state.vel_x, state.vel_y, state.vel_z] = self.vel;
        state.speed_mps = self.speed_mps;
        [
            state.angular_vel_yaw,
            state.angular_vel_pitch,
            state.angular_vel_roll,
        ] = self.angular_vel;
        state.wheel_angular_vel = self.wheel_angular_vel;
        [
            state.weight_front_left_n,
            state.weight_front_right_n,
            state.weight_rear_left_n,
            state.weight_rear_right_n,
        ] = self.wheel_load_n;
        let s = &mut state.suspension;
        [
            s.front_left_travel_m,
            s.front_right_travel_m,
            s.rear_left_travel_m,
            s.rear_right_travel_m,
        ] = self.suspension_travel_m;
        [
            s.front_left_velocity_mps,
            s.front_right_velocity_mps,
            s.rear_left_velocity_mps,
            s.rear_right_velocity_mps,
        ] = self.suspension_velocity_mps;
        [
            state.g_forces.lateral_g,
            state.g_forces.longitudinal_g,
            state.g_forces.vertical_g,
        ] = self.g_forces;
        state.vertical_accel_mps2 = self.vertical_accel_mps2;
        [state.ground_slope_f, state.ground_slope_y_f] = self.ground_slope;
        [state.porpoise_phase, state.porpoise_amp] = self.porpoise;
        [
            state.throttle_input,
            state.brake_input,
            state.steering_input,
            state.clutch_input,
        ] = self.controls;
        state.gear = self.gear;
        state.engine_rpm = self.engine_rpm;
        state.turbo_spool = self.turbo_spool;
        state.auto_shift_hold_ticks = self.auto_shift_hold_ticks;
        state.auto_reverse_ticks = self.auto_reverse_ticks;
        state.track_progress = self.track_progress;
        state.lateral_offset_m = self.lateral_offset_m;
        state.current_lap = self.current_lap;
        state.nearest_centerline_idx = self.centerline_idx;
        let flag = |bit| self.flags & bit != 0;
        state.is_on_track = flag(FLAG_ON_TRACK);
        state.is_airborne = flag(FLAG_AIRBORNE);
        state.is_colliding = flag(FLAG_COLLIDING);
        state.drs_open = flag(FLAG_DRS_OPEN);
        state.wheels_off_track = flag(FLAG_WHEELS_OFF_TRACK);
    }
}

/// One slice of a full state blob. Positional on the wire. A client keeps
/// the slices of one `tick` in order from `index` 0 and has the blob when
/// `count` have come; a slice lost drops that blob (the next comes
/// [`STATE_BLOB_SECONDS`] later).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StateChunk {
    /// The tick the blob is the car at, and the newest input sequence
    /// applied by then.
    pub tick: u32,
    pub input_seq: Option<u32>,
    /// This slice's place, and how many make the blob.
    pub index: u16,
    pub count: u16,
    /// At most [`STATE_CHUNK_BYTES`] of `car_step::encode_state`.
    pub bytes: RecordBytes,
}

/// Feedback messages between the starts of two blobs at `tick_rate`
/// with telemetry every `telemetry_divisor` ticks.
pub fn state_period_messages(tick_rate: u16, telemetry_divisor: u64) -> u32 {
    let per_second = tick_rate as f32 / telemetry_divisor.max(1) as f32;
    ((STATE_BLOB_SECONDS * per_second).round() as u32).max(1)
}

/// One driver's feed: the input sequence the loop applied last, and the
/// blob going out.
#[derive(Debug, Clone, Default)]
pub struct OwnCarFeed {
    /// The newest `PlayerInput.seq` the loop has applied to the car.
    pub applied_seq: Option<u32>,
    blob: Vec<u8>,
    blob_tick: u32,
    blob_seq: Option<u32>,
    next_chunk: u16,
    chunk_count: u16,
    /// Messages to go before the next blob starts.
    wait: u32,
}

impl OwnCarFeed {
    /// What goes in this feedback message: the snapshot of `state` at
    /// `tick`, and the next slice of the blob, if one is going out. A blob
    /// starts on the first message, then every `period` messages.
    pub fn next(
        &mut self,
        state: &CarState,
        tick: u32,
        period: u32,
    ) -> (OwnCarSnapshot, Option<StateChunk>) {
        let snapshot = OwnCarSnapshot::of(state, tick, self.applied_seq);
        if self.next_chunk >= self.chunk_count {
            if self.wait > 0 {
                self.wait -= 1;
                return (snapshot, None);
            }
            let blob = crate::car_step::encode_state(state);
            let count = blob.len().div_ceil(STATE_CHUNK_BYTES);
            let Ok(count) = u16::try_from(count) else {
                // Never at today's sizes; a blob too big to slice is not sent.
                self.wait = period.saturating_sub(1);
                return (snapshot, None);
            };
            self.blob = blob;
            self.blob_tick = tick;
            self.blob_seq = self.applied_seq;
            self.next_chunk = 0;
            self.chunk_count = count;
            self.wait = period.saturating_sub(count as u32);
        }
        let index = self.next_chunk;
        let start = index as usize * STATE_CHUNK_BYTES;
        let end = (start + STATE_CHUNK_BYTES).min(self.blob.len());
        self.next_chunk += 1;
        let chunk = StateChunk {
            tick: self.blob_tick,
            input_seq: self.blob_seq,
            index,
            count: self.chunk_count,
            bytes: RecordBytes(self.blob[start..end].to_vec()),
        };
        (snapshot, Some(chunk))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::GridSlot;

    fn slot(x: f32) -> GridSlot {
        GridSlot {
            position: 1,
            x,
            y: -4.0,
            z: 1.0,
            yaw_rad: 0.5,
        }
    }

    fn car() -> CarState {
        let mut state = CarState::new(
            uuid::Uuid::from_u128(1),
            uuid::Uuid::from_u128(2),
            &slot(10.0),
        );
        state.vel_x = 30.0;
        state.speed_mps = 30.0;
        state.wheel_angular_vel = [90.0, 91.0, 92.0, 93.0];
        state.suspension.rear_left_travel_m = -0.01;
        state.gear = 3;
        state.engine_rpm = 8000.0;
        state.nearest_centerline_idx = Some(77);
        state.wheels_off_track = true;
        state.drs_open = true;
        state
    }

    #[test]
    fn a_snapshot_laid_over_another_state_gives_back_its_own() {
        let served = car();
        let snapshot = OwnCarSnapshot::of(&served, 99, Some(5));
        assert_eq!((snapshot.tick, snapshot.input_seq), (99, Some(5)));

        let mut predicted = CarState::new(served.player_id, served.car_config_id, &slot(0.0));
        predicted.fuel_liters = 42.0;
        snapshot.apply_to(&mut predicted);
        assert_eq!(OwnCarSnapshot::of(&predicted, 99, Some(5)), snapshot);
        assert_eq!(predicted.nearest_centerline_idx, Some(77));
        assert!(predicted.wheels_off_track && predicted.drs_open && predicted.is_on_track);
        assert_eq!(predicted.fuel_liters, 42.0, "the slow state is kept");
    }

    #[test]
    fn the_blob_goes_out_in_slices_then_waits_out_its_period() {
        let state = car();
        let blob = crate::car_step::encode_state(&state);
        let count = blob.len().div_ceil(STATE_CHUNK_BYTES) as u32;
        assert!(count > 1, "a blob of {} bytes is sliced", blob.len());
        let period = 30;
        let mut feed = OwnCarFeed {
            applied_seq: Some(7),
            ..Default::default()
        };

        let mut slices_at = Vec::new();
        let mut whole = Vec::new();
        for message in 0..(2 * period + 2) {
            let (snapshot, chunk) = feed.next(&state, 1000 + message, period);
            assert_eq!(snapshot.input_seq, Some(7));
            assert_eq!(snapshot.tick, 1000 + message);
            let Some(chunk) = chunk else { continue };
            slices_at.push(message);
            if chunk.index == 0 {
                whole.clear();
            }
            assert_eq!(chunk.count as u32, count);
            assert_eq!(chunk.input_seq, Some(7));
            assert!(chunk.bytes.0.len() <= STATE_CHUNK_BYTES);
            // Every slice of a blob names the tick its first went out at.
            assert_eq!(chunk.tick, 1000 + message - chunk.index as u32);
            whole.extend_from_slice(&chunk.bytes.0);
            if chunk.index as u32 + 1 == count {
                assert_eq!(whole, blob, "the slices make the blob");
            }
        }
        // A blob starts on the first message and every `period` after.
        let expected: Vec<u32> = (0..count)
            .chain(period..period + count)
            .chain(2 * period..2 * period + 2)
            .collect();
        assert_eq!(slices_at, expected);
        let decoded = crate::car_step::decode_state(&blob).unwrap();
        assert_eq!(crate::car_step::encode_state(&decoded), blob);
    }

    #[test]
    fn the_period_follows_the_rates() {
        assert_eq!(state_period_messages(420, 7), 30);
        assert_eq!(state_period_messages(240, 4), 30);
        assert_eq!(state_period_messages(60, 60), 1);
    }
}

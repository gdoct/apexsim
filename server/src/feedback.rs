//! Driver feedback: what a car's own driver should feel through the wheel or
//! pad. That covers the steering-column torque, how far each tyre is past its
//! grip, the surface under each wheel, suspension hits and contact with other
//! cars.
//!
//! Physics records one [`FeedbackTick`] per car per tick into
//! `CarState::feedback`. The game loop drains it into a [`DriverFeedback`]
//! message for the car's human driver on every telemetry tick, so nothing that
//! happened between two broadcasts is lost: the steering torque keeps every
//! tick's sample, and the transients are peak-held. A kerb strike or a
//! landing lasts a tick or two, and sampling it at the telemetry rate would
//! miss most of them.
//!
//! The values are device-agnostic on purpose. A wheel turns `steer_torque`
//! into a constant force, a pad turns slip and surface into rumble, and a
//! bass shaker or motion rig would read the same message.

use serde::{Deserialize, Serialize};

/// Steering-torque samples kept between two broadcasts. At the default 240 Hz
/// tick and 60 Hz telemetry that is 4 per message; beyond this cap the oldest
/// are dropped, which only a very slow telemetry rate would hit.
pub const MAX_STEER_SAMPLES: usize = 32;

/// Largest slip, as a multiple of the tyre's peak, that goes on the wire. A
/// locked wheel is about 12x its peak slip ratio; the cap only guards against
/// a degenerate tyre config producing infinities.
const MAX_NORMALIZED_SLIP: f32 = 50.0;

/// What is under one tyre, as far as feedback cares. Ordered by how much it
/// shakes the car, so peak-holding over an interval keeps the roughest.
#[repr(u8)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord)]
pub enum ContactSurface {
    #[default]
    Road = 0,
    /// Within the curb band past the road edge.
    Curb = 1,
    /// Past the curbs: grass, gravel, the verge.
    Off = 2,
}

/// One physics tick's contribution, per wheel in FL, FR, RL, RR order.
#[derive(Debug, Clone, Copy, Default)]
pub struct FeedbackTick {
    /// Steering-column torque as a fraction of the car's reference (see
    /// `physics::steering_column_torque`). Positive turns the wheel left.
    pub steer_torque: f32,
    /// Longitudinal slip as a multiple of the tyre's peak slip ratio; negative
    /// under braking. Beyond ±1 the tyre is past its grip.
    pub slip_ratio: [f32; 4],
    /// Slip angle as a multiple of the tyre's peak slip angle.
    pub slip_angle: [f32; 4],
    pub surface: [ContactSurface; 4],
    /// Suspension compression speed, m/s (positive = compressing).
    pub suspension_mps: [f32; 4],
    /// ABS is holding a wheel back from locking.
    pub abs_active: bool,
    /// Traction control is cutting drive to a wheel.
    pub tc_active: bool,
}

/// Collects [`FeedbackTick`]s between broadcasts. Runtime-only state on
/// `CarState`; it never feeds back into the simulation.
#[derive(Debug, Clone, Copy)]
pub struct FeedbackAccumulator {
    steer_torque: [f32; MAX_STEER_SAMPLES],
    steer_len: usize,
    slip_ratio: [f32; 4],
    slip_angle: [f32; 4],
    surface: [ContactSurface; 4],
    suspension_mps: [f32; 4],
    abs_active: bool,
    tc_active: bool,
    impact_mps: f32,
    steer_kick: f32,
}

impl Default for FeedbackAccumulator {
    fn default() -> Self {
        Self {
            steer_torque: [0.0; MAX_STEER_SAMPLES],
            steer_len: 0,
            slip_ratio: [0.0; 4],
            slip_angle: [0.0; 4],
            surface: [ContactSurface::Road; 4],
            suspension_mps: [0.0; 4],
            abs_active: false,
            tc_active: false,
            impact_mps: 0.0,
            steer_kick: 0.0,
        }
    }
}

/// Of two signed values, the one further from zero.
fn peak(held: f32, new: f32) -> f32 {
    if new.abs() > held.abs() {
        new
    } else {
        held
    }
}

fn finite_or_zero(value: f32) -> f32 {
    if value.is_finite() {
        value
    } else {
        0.0
    }
}

impl FeedbackAccumulator {
    pub fn record_tick(&mut self, tick: &FeedbackTick) {
        if self.steer_len == MAX_STEER_SAMPLES {
            self.steer_torque.copy_within(1.., 0);
            self.steer_len -= 1;
        }
        self.steer_torque[self.steer_len] = finite_or_zero(tick.steer_torque);
        self.steer_len += 1;

        for wheel in 0..4 {
            let clamp = |v: f32| finite_or_zero(v).clamp(-MAX_NORMALIZED_SLIP, MAX_NORMALIZED_SLIP);
            self.slip_ratio[wheel] = peak(self.slip_ratio[wheel], clamp(tick.slip_ratio[wheel]));
            self.slip_angle[wheel] = peak(self.slip_angle[wheel], clamp(tick.slip_angle[wheel]));
            self.surface[wheel] = self.surface[wheel].max(tick.surface[wheel]);
            self.suspension_mps[wheel] = peak(
                self.suspension_mps[wheel],
                finite_or_zero(tick.suspension_mps[wheel]),
            );
        }
        self.abs_active |= tick.abs_active;
        self.tc_active |= tick.tc_active;
    }

    /// A contact with another car, by the speed the two were closing at.
    pub fn record_impact(&mut self, closing_mps: f32) {
        self.impact_mps = self.impact_mps.max(finite_or_zero(closing_mps));
    }

    /// A jolt through the steering column from a hit, in the torque's own
    /// units and sign (see [`DriverFeedback::steer_kick`]). The hardest of an
    /// interval is kept, with its sign.
    pub fn record_steer_kick(&mut self, kick: f32) {
        self.steer_kick = peak(self.steer_kick, finite_or_zero(kick));
    }

    /// Everything since the last call, as the message for the driver. Starts
    /// the next interval empty.
    pub fn take(&mut self, server_tick: u32) -> DriverFeedback {
        let message = DriverFeedback {
            server_tick,
            steer_torque: self.steer_torque[..self.steer_len].to_vec(),
            slip_ratio: self.slip_ratio,
            slip_angle: self.slip_angle,
            surface: self.surface.map(|s| s as u8),
            suspension_mps: self.suspension_mps,
            abs_active: self.abs_active,
            tc_active: self.tc_active,
            impact_mps: self.impact_mps,
            steer_kick: self.steer_kick,
        };
        *self = Self::default();
        message
    }
}

/// `ServerMessage::DriverFeedback`: sent over UDP to one car's human driver
/// on every telemetry tick, positional encoding (`rmp_serde::to_vec`) like
/// `TelemetryCompact`. Field order is the wire contract. The Unreal client
/// reads it positionally (`ApexProtocolCodec.cpp`), and the golden bytes in
/// `network.rs` pin it.
///
/// Per-wheel arrays are FL, FR, RL, RR.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DriverFeedback {
    /// Tick of the newest sample.
    pub server_tick: u32,
    /// Steering-column torque for every physics tick since the previous
    /// message, oldest first. 1.0 is the car's reference torque: the front
    /// axle at its static grip limit. Downforce and load transfer take it
    /// past 1. Positive turns the wheel left, the server's steering sign.
    pub steer_torque: Vec<f32>,
    /// Longitudinal slip, multiples of the tyre's peak slip ratio (negative
    /// braking), largest magnitude over the interval. Past ±1 the tyre is
    /// beyond its grip: a locking or spinning wheel.
    pub slip_ratio: [f32; 4],
    /// Slip angle, multiples of the tyre's peak slip angle, largest
    /// magnitude over the interval. Past ±1 the tyre is sliding.
    pub slip_angle: [f32; 4],
    /// `ContactSurface` per wheel, the roughest over the interval: 0 road,
    /// 1 curb, 2 off track.
    pub surface: [u8; 4],
    /// Suspension compression speed, m/s, largest magnitude over the
    /// interval: bumps, kerb strikes, landings.
    pub suspension_mps: [f32; 4],
    pub abs_active: bool,
    pub tc_active: bool,
    /// Largest closing speed of any contact with another car over the
    /// interval, m/s; 0 when there was none.
    pub impact_mps: f32,
    /// The hardest jolt a hit put through the steering column over the
    /// interval, in `steer_torque`'s units and sign; 0 when there was none.
    /// A contact lasts a tick, so it would be lost among the torque samples:
    /// it comes separately and the client plays it as a decaying kick on top
    /// of the torque. Appended last, so an older client skips it.
    pub steer_kick: f32,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tick(steer: f32) -> FeedbackTick {
        FeedbackTick {
            steer_torque: steer,
            ..Default::default()
        }
    }

    #[test]
    fn keeps_every_steering_sample_in_order() {
        let mut acc = FeedbackAccumulator::default();
        for s in [0.1, 0.2, 0.3, 0.4] {
            acc.record_tick(&tick(s));
        }
        assert_eq!(acc.take(7).steer_torque, vec![0.1, 0.2, 0.3, 0.4]);
    }

    #[test]
    fn a_full_buffer_drops_the_oldest_samples() {
        let mut acc = FeedbackAccumulator::default();
        for i in 0..(MAX_STEER_SAMPLES + 3) {
            acc.record_tick(&tick(i as f32));
        }
        let samples = acc.take(0).steer_torque;
        assert_eq!(samples.len(), MAX_STEER_SAMPLES);
        assert_eq!(samples[0], 3.0);
        assert_eq!(*samples.last().unwrap(), (MAX_STEER_SAMPLES + 2) as f32);
    }

    #[test]
    fn transients_are_peak_held_with_their_sign() {
        let mut acc = FeedbackAccumulator::default();
        let mut a = tick(0.0);
        a.slip_ratio[0] = -3.0;
        a.suspension_mps[2] = 0.4;
        a.surface[1] = ContactSurface::Curb;
        a.abs_active = true;
        let mut b = tick(0.0);
        b.slip_ratio[0] = 1.5;
        b.suspension_mps[2] = -2.5;
        b.surface[1] = ContactSurface::Road;
        acc.record_tick(&a);
        acc.record_tick(&b);
        acc.record_impact(4.0);
        acc.record_impact(1.0);
        acc.record_steer_kick(0.4);
        acc.record_steer_kick(-1.2);
        acc.record_steer_kick(0.8);

        let msg = acc.take(99);
        assert_eq!(msg.server_tick, 99);
        assert_eq!(msg.slip_ratio[0], -3.0, "the larger slip wins, sign kept");
        assert_eq!(msg.suspension_mps[2], -2.5);
        assert_eq!(
            msg.surface[1],
            ContactSurface::Curb as u8,
            "a curb touched mid-interval still shows"
        );
        assert!(msg.abs_active && !msg.tc_active);
        assert_eq!(msg.impact_mps, 4.0);
        assert_eq!(msg.steer_kick, -1.2, "the hardest kick wins, sign kept");
    }

    #[test]
    fn take_starts_a_fresh_interval() {
        let mut acc = FeedbackAccumulator::default();
        let mut a = tick(0.5);
        a.slip_angle[3] = 2.0;
        acc.record_tick(&a);
        acc.record_impact(3.0);
        acc.record_steer_kick(1.0);
        acc.take(1);

        let msg = acc.take(2);
        assert!(msg.steer_torque.is_empty());
        assert_eq!(msg.slip_angle, [0.0; 4]);
        assert_eq!(msg.impact_mps, 0.0);
        assert_eq!(msg.steer_kick, 0.0);
    }

    #[test]
    fn non_finite_values_never_reach_the_wire() {
        let mut acc = FeedbackAccumulator::default();
        let mut a = tick(f32::NAN);
        a.slip_ratio[0] = f32::NEG_INFINITY;
        a.slip_angle[1] = 1e9;
        acc.record_tick(&a);
        let msg = acc.take(0);
        assert_eq!(msg.steer_torque, vec![0.0]);
        assert_eq!(msg.slip_ratio[0], 0.0);
        assert_eq!(msg.slip_angle[1], MAX_NORMALIZED_SLIP);
    }
}

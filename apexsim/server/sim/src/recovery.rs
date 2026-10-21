//! Recovering a stuck car (`ClientMessage::RecoverCar`).
//!
//! Walls stop cars, so a human can end up nose-in against a barrier with
//! no room to reverse out. A recovery puts the car back, at a cost:
//!
//! - **Back to track**: on the centerline at the car's own place on the lap
//!   (or the first spot behind it clear of other cars, never back over the
//!   line), pointing along the lap, in first. The lap in progress is struck.
//! - **Back to pits**: parked at the car's own box. When the hold is over
//!   the pit autopilot takes it: the crew services it (tyres, fuel, repairs)
//!   and drives it out, as after a stop. A towed car crosses no checkpoint,
//!   so the lap it was on counts only if it had passed them all.
//!
//! Either way the car is **held** where it was put for the hold's time
//! ([`TRACK_HOLD_S`], [`PITS_HOLD_S`]): stationary, out of every collision,
//! wake and AI pass, its input ignored. A car put back on the track then
//! waits until no car is closing on it ([`traffic_clear`]), for at most
//! [`RELEASE_WAIT_MAX_S`] more. Only a slow car may ask
//! ([`MAX_SPEED_MPS`]): a recovery is a way out of a wall, not out of a
//! spin. In a hotlap or qualifying session "back to pits" is the garage.

use crate::data::TrackPoint;
use serde::{Deserialize, Serialize};
use serde_repr::{Deserialize_repr, Serialize_repr};

/// Faster than this, m/s, a car may not ask for a recovery.
pub const MAX_SPEED_MPS: f32 = 5.0;
/// How long a car put back on the track is held, s: the time cost.
pub const TRACK_HOLD_S: f32 = 8.0;
/// How long a car towed to its box waits before the crew starts, s.
pub const PITS_HOLD_S: f32 = 30.0;
/// A spot on the track is free when no other car is this near it, m.
pub const CLEARANCE_M: f32 = 10.0;
/// Spots tried behind the car's own station, this far apart, m.
pub const STEP_BACK_M: f32 = 15.0;
pub const SPOT_TRIES: usize = 8;
/// A car is closing on the recovered one while it is within this many
/// seconds at its speed, or [`RELEASE_GAP_MIN_M`], behind it.
pub const RELEASE_GAP_S: f32 = 3.0;
pub const RELEASE_GAP_MIN_M: f32 = 30.0;
/// The longest a car held for traffic waits past its hold, s.
pub const RELEASE_WAIT_MAX_S: f32 = 15.0;

/// Where a driver asks for their car to be put.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum RecoverDestination {
    /// Back onto the track where the car is.
    #[default]
    Track = 0,
    /// To the car's pit box (the garage in a hotlap or qualifying session).
    Pits = 1,
}

/// A recovery under way on a car.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Recovery {
    pub destination: RecoverDestination,
    /// Hold left, s; at or below 0 the car waits only for traffic.
    pub left_s: f32,
    /// How long it has waited for traffic past the hold, s.
    pub waited_s: f32,
}

impl Recovery {
    pub fn new(destination: RecoverDestination) -> Self {
        Self {
            destination,
            left_s: match destination {
                RecoverDestination::Track => TRACK_HOLD_S,
                RecoverDestination::Pits => PITS_HOLD_S,
            },
            waited_s: 0.0,
        }
    }

    /// Seconds left for telemetry, in tenths: at least 1 while the car
    /// waits for traffic, so a client can tell a hold from none.
    pub fn deciseconds(&self) -> u16 {
        ((self.left_s.max(0.0) * 10.0).round() as u16).max(1)
    }
}

/// The station a car recovered to the track is put at: its own, or the
/// first of [`SPOT_TRIES`] spots [`STEP_BACK_M`] apart behind it whose pose
/// is [`CLEARANCE_M`] clear of every car in `others` (x, y). Never behind
/// the line, so the lap counter sees no crossing; the car's own station
/// when nothing is free.
pub fn track_station(centerline: &[TrackPoint], own_station_m: f32, others: &[(f32, f32)]) -> f32 {
    for k in 0..SPOT_TRIES {
        let station = own_station_m - k as f32 * STEP_BACK_M;
        if station < 0.0 {
            break;
        }
        let (x, y, _, _) = crate::physics::pose_at_station(centerline, station);
        if others
            .iter()
            .all(|(ox, oy)| (ox - x).hypot(oy - y) >= CLEARANCE_M)
        {
            return station;
        }
    }
    own_station_m
}

/// No car in `traffic` (track progress, speed) is closing on `station_m`
/// from behind: none within [`RELEASE_GAP_S`] at its speed, or
/// [`RELEASE_GAP_MIN_M`], of it on a lap of `track_length_m`.
pub fn traffic_clear(station_m: f32, track_length_m: f32, traffic: &[(f32, f32)]) -> bool {
    if track_length_m <= 0.0 {
        return true;
    }
    traffic.iter().all(|&(progress, speed)| {
        let behind = (station_m - progress).rem_euclid(track_length_m);
        behind > (speed * RELEASE_GAP_S).max(RELEASE_GAP_MIN_M)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn straight(length: f32) -> Vec<TrackPoint> {
        (0..=(length as usize / 5))
            .map(|i| TrackPoint {
                x: i as f32 * 5.0,
                distance_from_start_m: i as f32 * 5.0,
                ..Default::default()
            })
            .collect()
    }

    #[test]
    fn a_free_spot_is_the_cars_own() {
        let line = straight(1000.0);
        assert_eq!(track_station(&line, 500.0, &[(200.0, 0.0)]), 500.0);
    }

    #[test]
    fn a_taken_spot_steps_back_past_the_car_on_it() {
        let line = straight(1000.0);
        let station = track_station(&line, 500.0, &[(503.0, 0.0)]);
        assert_eq!(station, 500.0 - STEP_BACK_M);
    }

    #[test]
    fn never_steps_back_over_the_line() {
        let line = straight(1000.0);
        // Every spot from 20 m back is taken; the line is 20 m behind.
        let others: Vec<(f32, f32)> = (0..10).map(|i| (i as f32 * 5.0, 0.0)).collect();
        assert_eq!(track_station(&line, 20.0, &others), 20.0);
    }

    #[test]
    fn a_car_closing_from_behind_holds_the_release() {
        // 100 m behind at 50 m/s: 2 s away.
        assert!(!traffic_clear(500.0, 5000.0, &[(400.0, 50.0)]));
        // 200 m behind at 50 m/s: 4 s away.
        assert!(traffic_clear(500.0, 5000.0, &[(300.0, 50.0)]));
        // Ahead of the spot is no threat.
        assert!(traffic_clear(500.0, 5000.0, &[(520.0, 50.0)]));
        // A slow car close behind still is.
        assert!(!traffic_clear(500.0, 5000.0, &[(480.0, 1.0)]));
    }

    #[test]
    fn traffic_behind_across_the_line_counts() {
        // The car is 30 m past the line, one 60 m behind it on the last lap.
        assert!(!traffic_clear(30.0, 5000.0, &[(4970.0, 40.0)]));
    }

    #[test]
    fn telemetry_shows_a_traffic_wait() {
        let mut r = Recovery::new(RecoverDestination::Track);
        assert_eq!(r.deciseconds(), (TRACK_HOLD_S * 10.0) as u16);
        r.left_s = -2.0;
        assert_eq!(r.deciseconds(), 1);
    }
}

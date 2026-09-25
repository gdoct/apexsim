//! The drag reduction system: who may open the rear wing, where, and what
//! it does when open.
//!
//! The track file carries the zones (`TrackConfig::drs_zones`, three
//! stations each: detection, activation, end) and the car its `DrsSpec`
//! (what the open flap takes off the drag and the rear downforce; an F1
//! car gets one by default, nothing else does). Everything here is the
//! rule between the two, run by the game session once per tick before the
//! physics:
//!
//! - Crossing a zone's **detection** line *arms* that zone for the car —
//!   in a race, only when a car is within [`DRS_GAP_S`] ahead of it on the
//!   road (any car, lapped or not, as the FIA counts it); in practice,
//!   qualifying and a hotlap the zone is always armed.
//! - Between the zone's **activation** and **end** an armed car is
//!   *allowed* to open the flap (`CarState::drs_allowed`), and the flap is
//!   **open** while the driver holds the button and is off the brake
//!   (`physics::update_car_3d`: [`DRS_BRAKE_CLOSE`]); a lift alone keeps
//!   it open, as the real one does.
//! - Past the end the zone disarms itself.
//! - Nothing opens in the rain, on the first lap of a race, or in the
//!   garage.
//!
//! The AI asks for the flap whenever it is allowed, and telemetry carries
//! the two states as bits 3 and 4 of `lap_flags` so the HUD can show a
//! light. The racing line's speed profile takes the drag reduction into
//! account inside the zones (`racing_line`), so the AI's braking points on
//! a DRS straight are the flap-open ones.

use std::collections::{BTreeMap, HashMap};

use crate::data::{
    CarConfig, CarConfigId, CarState, DrsZone, GameMode, PlayerId, TrackConfig, Weather,
};

/// The car ahead must be within this many seconds at the detection line for
/// the zone to arm in a race.
pub const DRS_GAP_S: f32 = 1.0;
/// The brake pedal shuts the flap at this input.
pub const DRS_BRAKE_CLOSE: f32 = 0.1;
/// In a race the flap stays shut until this lap.
pub const DRS_RACE_FROM_LAP: u16 = 2;
/// A car further ahead than this is not "ahead" for the gap: the lap's
/// own length would otherwise find the car itself round the other way.
const GAP_REACH_M: f32 = 400.0;

/// `lap_flags` bit 3: the car may open the flap here.
pub const LAP_FLAG_DRS_ALLOWED: u8 = 8;
/// `lap_flags` bit 4: the flap is open.
pub const LAP_FLAG_DRS_OPEN: u8 = 16;

/// Whether `station` lies inside `[start, end)` along a lap of `total`,
/// the span wrapping through the start line when `end < start`.
pub fn inside(station: f32, start: f32, end: f32, total: f32) -> bool {
    let s = station.rem_euclid(total);
    let (a, b) = (start.rem_euclid(total), end.rem_euclid(total));
    if a <= b {
        (a..b).contains(&s)
    } else {
        s >= a || s < b
    }
}

/// Whether moving forward from `from` to `to` (stations on a lap of
/// `total`) crossed `line`. A move of more than half a lap in one tick is
/// not forward motion but a respawn, and crosses nothing.
pub fn crossed(from: f32, to: f32, line: f32, total: f32) -> bool {
    let moved = (to - from).rem_euclid(total);
    if moved <= 0.0 || moved > total * 0.5 {
        return false;
    }
    let ahead = (line - from).rem_euclid(total);
    ahead > 0.0 && ahead <= moved
}

/// Which game modes race under the gap rule. Practice, qualifying and the
/// hotlap open the flap in every zone.
pub fn race_rules(mode: GameMode) -> bool {
    matches!(mode, GameMode::Race | GameMode::Countdown)
}

/// Seconds to the nearest car ahead of `me` on the road, or `None` when
/// nobody is within [`GAP_REACH_M`].
fn gap_ahead_s(me: &CarState, field: &[(PlayerId, f32)], total: f32) -> Option<f32> {
    let speed = me.speed_mps.max(1.0);
    field
        .iter()
        .filter(|(id, _)| *id != me.player_id)
        .map(|(_, progress)| (progress - me.track_progress).rem_euclid(total))
        .filter(|d| *d > 0.0 && *d <= GAP_REACH_M)
        .map(|d| d / speed)
        .reduce(f32::min)
}

/// Run the rule over every car for this tick. `mode` and `weather` are the
/// session's; the cars' `track_progress` is where the last tick left them.
pub fn update(
    participants: &mut BTreeMap<PlayerId, CarState>,
    car_configs: &HashMap<CarConfigId, CarConfig>,
    track: &TrackConfig,
    mode: GameMode,
    weather: Weather,
) {
    let zones: &[DrsZone] = &track.drs_zones;
    if zones.is_empty() {
        return;
    }
    let total = crate::laps::track_length_m(track);
    if total <= 0.0 {
        return;
    }
    let wet = matches!(weather, Weather::LightRain | Weather::HeavyRain);
    let race = race_rules(mode);
    let field: Vec<(PlayerId, f32)> = participants
        .values()
        .filter(|s| !s.in_garage)
        .map(|s| (s.player_id, s.track_progress))
        .collect();

    for state in participants.values_mut() {
        let has_drs = car_configs
            .get(&state.car_config_id)
            .is_some_and(|c| c.drs.is_some());
        let from = state.drs_last_progress;
        let to = state.track_progress;
        state.drs_last_progress = to;
        if !has_drs {
            state.drs_armed = 0;
            state.drs_allowed = false;
            state.drs_open = false;
            continue;
        }
        let enabled = !wet && !state.in_garage && (!race || state.current_lap >= DRS_RACE_FROM_LAP);

        for (i, zone) in zones.iter().enumerate().take(32) {
            let bit = 1u32 << i;
            if crossed(from, to, zone.detection_m, total) {
                let earned = enabled
                    && (!race || gap_ahead_s(state, &field, total).is_some_and(|g| g <= DRS_GAP_S));
                if earned {
                    state.drs_armed |= bit;
                } else {
                    state.drs_armed &= !bit;
                }
            }
            if state.drs_armed & bit != 0 && crossed(from, to, zone.end_m, total) {
                state.drs_armed &= !bit;
            }
        }

        state.drs_allowed = enabled
            && zones.iter().enumerate().take(32).any(|(i, zone)| {
                state.drs_armed & (1 << i) != 0 && inside(to, zone.start_m, zone.end_m, total)
            });
        if !state.drs_allowed {
            state.drs_open = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::DrsSpec;

    fn track(total: f32) -> TrackConfig {
        let mut t = TrackConfig::default();
        // A ring of `total` metres, one point per 10 m, so the lap length
        // the rules use is what the zones were written against.
        let n = (total / 10.0) as usize;
        t.centerline = (0..n)
            .map(|i| {
                let a = i as f32 / n as f32 * std::f32::consts::TAU;
                let r = total / std::f32::consts::TAU;
                crate::data::TrackPoint {
                    x: r * a.cos(),
                    y: r * a.sin(),
                    heading_rad: a + std::f32::consts::FRAC_PI_2,
                    distance_from_start_m: i as f32 * 10.0,
                    ..Default::default()
                }
            })
            .collect();
        t.drs_zones = vec![DrsZone {
            detection_m: 100.0,
            start_m: 200.0,
            end_m: 500.0,
        }];
        t
    }

    fn cars() -> (HashMap<CarConfigId, CarConfig>, CarConfigId) {
        let config = CarConfig {
            drs: Some(DrsSpec::F1),
            ..Default::default()
        };
        let id = config.id;
        let mut m = HashMap::new();
        m.insert(id, config);
        (m, id)
    }

    fn car(id: CarConfigId, progress: f32, speed: f32, lap: u16) -> CarState {
        let slot = crate::data::GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        let mut s = CarState::new(PlayerId::new_v4(), id, &slot);
        s.track_progress = progress;
        s.drs_last_progress = progress;
        s.speed_mps = speed;
        s.current_lap = lap;
        s
    }

    fn step(participants: &mut BTreeMap<PlayerId, CarState>, id: &PlayerId, to: f32) {
        participants.get_mut(id).unwrap().track_progress = to;
    }

    #[test]
    fn spans_wrap_and_crossings_are_forward_only() {
        assert!(inside(250.0, 200.0, 500.0, 1000.0));
        assert!(!inside(500.0, 200.0, 500.0, 1000.0));
        assert!(inside(950.0, 900.0, 100.0, 1000.0));
        assert!(inside(50.0, 900.0, 100.0, 1000.0));
        assert!(!inside(500.0, 900.0, 100.0, 1000.0));
        assert!(crossed(90.0, 110.0, 100.0, 1000.0));
        assert!(crossed(990.0, 10.0, 0.0, 1000.0));
        assert!(
            !crossed(110.0, 90.0, 100.0, 1000.0),
            "reversing crosses nothing"
        );
        assert!(
            !crossed(100.0, 100.0, 100.0, 1000.0),
            "standing still crosses nothing"
        );
        assert!(
            !crossed(0.0, 700.0, 100.0, 1000.0),
            "a respawn is not a lap"
        );
    }

    #[test]
    fn a_lone_car_in_a_race_never_earns_the_zone() {
        let track = track(1000.0);
        let (configs, id) = cars();
        let mut field = BTreeMap::new();
        let me = car(id, 90.0, 50.0, 3);
        let my_id = me.player_id;
        field.insert(my_id, me);
        step(&mut field, &my_id, 110.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        step(&mut field, &my_id, 250.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        assert!(!field[&my_id].drs_allowed);
    }

    #[test]
    fn within_a_second_at_detection_the_zone_opens_and_shuts_at_its_end() {
        let track = track(1000.0);
        let (configs, id) = cars();
        let mut field = BTreeMap::new();
        let me = car(id, 90.0, 50.0, 3);
        let my_id = me.player_id;
        // 40 m ahead at 50 m/s: 0.8 s.
        let ahead = car(id, 130.0, 50.0, 3);
        field.insert(my_id, me);
        field.insert(ahead.player_id, ahead);
        step(&mut field, &my_id, 110.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        assert!(!field[&my_id].drs_allowed, "not before the activation line");
        step(&mut field, &my_id, 250.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        assert!(field[&my_id].drs_allowed, "allowed in the zone");
        step(&mut field, &my_id, 520.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        assert!(!field[&my_id].drs_allowed, "shut past the end");
        assert_eq!(field[&my_id].drs_armed, 0);
    }

    #[test]
    fn the_gap_rule_needs_the_car_ahead_within_a_second() {
        let track = track(1000.0);
        let (configs, id) = cars();
        let mut field = BTreeMap::new();
        let me = car(id, 90.0, 50.0, 3);
        let my_id = me.player_id;
        // 80 m ahead at 50 m/s: 1.6 s.
        let ahead = car(id, 170.0, 50.0, 3);
        field.insert(my_id, me);
        field.insert(ahead.player_id, ahead);
        step(&mut field, &my_id, 110.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        step(&mut field, &my_id, 250.0);
        update(&mut field, &configs, &track, GameMode::Race, Weather::Sunny);
        assert!(!field[&my_id].drs_allowed);
    }

    #[test]
    fn practice_opens_every_zone_and_rain_or_the_first_lap_none() {
        let track = track(1000.0);
        let (configs, id) = cars();
        for (mode, weather, lap, want) in [
            (GameMode::FreePractice, Weather::Sunny, 1, true),
            (GameMode::Hotlap, Weather::Sunny, 1, true),
            (GameMode::FreePractice, Weather::HeavyRain, 3, false),
            (GameMode::Race, Weather::Sunny, 1, false),
        ] {
            let mut field = BTreeMap::new();
            let me = car(id, 90.0, 50.0, lap);
            let my_id = me.player_id;
            let ahead = car(id, 130.0, 50.0, lap);
            field.insert(my_id, me);
            field.insert(ahead.player_id, ahead);
            step(&mut field, &my_id, 110.0);
            update(&mut field, &configs, &track, mode, weather);
            step(&mut field, &my_id, 250.0);
            update(&mut field, &configs, &track, mode, weather);
            assert_eq!(
                field[&my_id].drs_allowed, want,
                "{mode:?} {weather:?} lap {lap}"
            );
        }
    }

    #[test]
    fn a_car_without_a_wing_never_opens_one() {
        let track = track(1000.0);
        let config = CarConfig {
            drs: None,
            ..Default::default()
        };
        let id = config.id;
        let mut configs = HashMap::new();
        configs.insert(id, config);
        let mut field = BTreeMap::new();
        let me = car(id, 90.0, 50.0, 3);
        let my_id = me.player_id;
        field.insert(my_id, me);
        step(&mut field, &my_id, 110.0);
        update(
            &mut field,
            &configs,
            &track,
            GameMode::Hotlap,
            Weather::Sunny,
        );
        step(&mut field, &my_id, 250.0);
        update(
            &mut field,
            &configs,
            &track,
            GameMode::Hotlap,
            Weather::Sunny,
        );
        assert!(!field[&my_id].drs_allowed);
    }
}

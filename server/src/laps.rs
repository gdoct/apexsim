//! Lap timing: sectors, track limits and the bests a lap is measured against.
//!
//! The lap *counter* lives in [`crate::physics::update_track_progress_3d`]
//! (it is tied to the anti-shortcut checkpoints); this module owns everything
//! a stopwatch would: which sector the car is in, what each sector took, and
//! whether the lap is still legal.
//!
//! **Track limits.** A lap is invalidated when all four wheels are off the
//! track — past the curb band, not merely past the white line, since the
//! curbs are what `curbs.rs` already calls track. One tick of it is noise
//! (a wheel skimming a kerb edge at 240 Hz), so the car has to be fully off
//! for [`TRACK_LIMITS_SECONDS`] before the lap is struck. The lap still
//! completes and is still timed — it just cannot become a best.
//!
//! **Sectors.** Three per lap, split at stations along the centerline: the
//! track file's own `sectors` when it has them, otherwise even thirds. The
//! boundaries are sent to the client once (`ServerMessage::TrackSectors`) so
//! it can tell which sector a car is in from the telemetry it already gets;
//! the splits themselves are timed here and delivered as events, because a
//! client timing them off 60 Hz snapshots would be tens of milliseconds out.

use crate::data::{CarState, TrackConfig};

/// Sectors per lap. Fixed: the HUD has three bars, and a track file supplies
/// at most [`SECTOR_COUNT`] - 1 boundaries.
pub const SECTOR_COUNT: usize = 3;

/// How long every wheel has to be off the track before the lap is struck.
pub const TRACK_LIMITS_SECONDS: f32 = 0.2;

/// Per-car lap timing. Runtime-only state, rebuilt from the grid on every
/// countdown; never serialized (a replay carries the times themselves).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LapTiming {
    /// Sector the car is in, 0-based.
    pub sector: u8,
    /// Tick the current sector started on.
    pub sector_start_tick: u32,
    /// Splits of the lap in progress, milliseconds; 0 where not yet set.
    pub splits_ms: [u32; SECTOR_COUNT],
    /// The lap in progress broke track limits.
    pub invalid: bool,
    /// The lap just completed was struck. False before the first one, so a
    /// car that has not finished a lap reports nothing against itself.
    pub last_invalid: bool,
    /// Splits of the completed lap, milliseconds.
    pub last_splits_ms: [u32; SECTOR_COUNT],
    /// Best legal lap this session, milliseconds.
    pub best_lap_ms: Option<u32>,
    /// Splits of that best lap.
    pub best_lap_splits_ms: [u32; SECTOR_COUNT],
    /// Best legal time in each sector this session, whatever lap it came from.
    pub best_splits_ms: [Option<u32>; SECTOR_COUNT],
    /// Consecutive ticks with all four wheels off the track.
    pub off_track_ticks: u16,
}

impl LapTiming {
    /// Start a fresh lap at `tick`: the car is at the line, sector 1, clean.
    pub fn start_lap(&mut self, tick: u32) {
        self.sector = 0;
        self.sector_start_tick = tick;
        self.splits_ms = [0; SECTOR_COUNT];
        self.invalid = false;
        self.off_track_ticks = 0;
    }

    /// Start a lap already under way at `station_m` (a car placed past the
    /// line, or one that joins mid-session): the sector is where it stands,
    /// and the sectors it never drove stay unset.
    pub fn start_lap_at(&mut self, tick: u32, track: &TrackConfig, station_m: f32) {
        self.start_lap(tick);
        self.sector = sector_of(track, station_m);
    }

    /// The lap flags carried in telemetry: bit 0 the lap in progress is
    /// struck, bit 1 the last completed lap was.
    pub fn flags(&self) -> u8 {
        let mut flags = 0u8;
        if self.invalid {
            flags |= 1;
        }
        if self.last_invalid {
            flags |= 2;
        }
        flags
    }
}

/// What a car did as it crossed a timing line, handed to the game loop to
/// broadcast and to measure against the stored records.
#[derive(Debug, Clone, PartialEq)]
pub struct LapEvent {
    /// The lap the sector belongs to (the lap just finished, at a lap end).
    pub lap: u16,
    /// Sector just completed, 0-based.
    pub sector: u8,
    /// What that sector took, milliseconds.
    pub sector_time_ms: u32,
    /// Set when this sector closed the lap.
    pub lap_time_ms: Option<u32>,
    /// Splits of the lap, at a lap end.
    pub splits_ms: [u32; SECTOR_COUNT],
    /// The lap was inside track limits.
    pub valid: bool,
    /// A legal lap or sector faster than anything this car did before.
    pub personal_best_lap: bool,
    pub personal_best_sector: bool,
}

/// Where sector `idx` (0-based) begins, in metres from the start line.
/// Sector 0 always begins at the line.
pub fn sector_start_m(track: &TrackConfig, idx: usize) -> f32 {
    let length = track_length_m(track);
    if idx == 0 || length <= 0.0 {
        return 0.0;
    }
    match track.sectors.get(idx - 1) {
        Some(&station) => station.clamp(0.0, length),
        None => length * idx as f32 / SECTOR_COUNT as f32,
    }
}

/// Every sector boundary past the line, ascending — what the client is told
/// so it can place a car in a sector from its station alone.
pub fn sector_boundaries_m(track: &TrackConfig) -> Vec<f32> {
    (1..SECTOR_COUNT)
        .map(|i| sector_start_m(track, i))
        .collect()
}

/// The sector a car at `station_m` is driving, 0-based.
pub fn sector_of(track: &TrackConfig, station_m: f32) -> u8 {
    let mut sector = 0u8;
    for idx in 1..SECTOR_COUNT {
        if station_m >= sector_start_m(track, idx) {
            sector = idx as u8;
        }
    }
    sector
}

/// Lap length in metres (the last centerline station).
pub fn track_length_m(track: &TrackConfig) -> f32 {
    track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(0.0)
}

/// Sector boundary stations resolved from the track file, or an empty vec
/// when it names none (even thirds are then used).
pub fn sectors_from_stations(mut stations: Vec<f32>, track_length: f32) -> Vec<f32> {
    stations.retain(|s| s.is_finite() && *s > 0.0 && *s < track_length);
    stations.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    stations.dedup();
    stations.truncate(SECTOR_COUNT - 1);
    if stations.len() == SECTOR_COUNT - 1 {
        stations
    } else {
        Vec::new()
    }
}

/// Count a tick against track limits. Called from physics with whether all
/// four wheels are off the track this tick.
pub fn note_track_limits(state: &mut CarState, tick_rate_hz: u16) {
    if state.wheels_off_track {
        state.laps.off_track_ticks = state.laps.off_track_ticks.saturating_add(1);
        let threshold = (TRACK_LIMITS_SECONDS * tick_rate_hz as f32).max(1.0) as u16;
        if state.laps.off_track_ticks >= threshold {
            state.laps.invalid = true;
        }
    } else {
        state.laps.off_track_ticks = 0;
    }
}

/// Close the sector the car is in because it crossed into `new_sector`.
/// Returns the event for the sector just completed.
pub fn close_sector(
    state: &mut CarState,
    new_sector: u8,
    tick: u32,
    tick_rate_hz: u16,
) -> LapEvent {
    let done = state.laps.sector as usize;
    let time_ms = ticks_to_ms(
        tick.saturating_sub(state.laps.sector_start_tick),
        tick_rate_hz,
    );
    state.laps.splits_ms[done.min(SECTOR_COUNT - 1)] = time_ms;
    state.laps.sector = new_sector;
    state.laps.sector_start_tick = tick;

    let personal_best_sector = !state.laps.invalid
        && state.laps.best_splits_ms[done.min(SECTOR_COUNT - 1)].is_none_or(|best| time_ms < best);
    if personal_best_sector {
        state.laps.best_splits_ms[done.min(SECTOR_COUNT - 1)] = Some(time_ms);
    }

    LapEvent {
        lap: state.current_lap,
        sector: done as u8,
        sector_time_ms: time_ms,
        lap_time_ms: None,
        splits_ms: state.laps.splits_ms,
        valid: !state.laps.invalid,
        personal_best_lap: false,
        personal_best_sector,
    }
}

/// Close the final sector and the lap. `lap_time_ms` is the lap as the lap
/// counter measured it, so the splits and the lap agree with the clock the
/// driver sees. Returns the event; the caller starts the next lap.
pub fn close_lap(
    state: &mut CarState,
    lap: u16,
    lap_time_ms: u32,
    tick: u32,
    tick_rate_hz: u16,
) -> LapEvent {
    let last = SECTOR_COUNT - 1;
    // The final split is what the other sectors leave of the lap, so the
    // three always add up to the lap time exactly.
    let earlier: u32 = state.laps.splits_ms[..last].iter().sum();
    let mut time_ms = ticks_to_ms(
        tick.saturating_sub(state.laps.sector_start_tick),
        tick_rate_hz,
    );
    if state.laps.splits_ms[..last].iter().all(|&s| s > 0) {
        time_ms = lap_time_ms.saturating_sub(earlier);
    }
    state.laps.splits_ms[last] = time_ms;

    let valid = !state.laps.invalid && state.laps.splits_ms.iter().all(|&s| s > 0);
    let personal_best_sector =
        valid && state.laps.best_splits_ms[last].is_none_or(|best| time_ms < best);
    if personal_best_sector {
        state.laps.best_splits_ms[last] = Some(time_ms);
    }

    let personal_best_lap = valid && state.laps.best_lap_ms.is_none_or(|best| lap_time_ms < best);
    if personal_best_lap {
        state.laps.best_lap_ms = Some(lap_time_ms);
        state.laps.best_lap_splits_ms = state.laps.splits_ms;
    }

    let event = LapEvent {
        lap,
        sector: last as u8,
        sector_time_ms: time_ms,
        lap_time_ms: Some(lap_time_ms),
        splits_ms: state.laps.splits_ms,
        valid,
        personal_best_lap,
        personal_best_sector,
    };

    state.laps.last_invalid = !valid;
    state.laps.last_splits_ms = state.laps.splits_ms;
    state.laps.start_lap(tick);
    event
}

/// Ticks as milliseconds at the session's rate.
pub fn ticks_to_ms(ticks: u32, tick_rate_hz: u16) -> u32 {
    ((ticks as f32 * 1000.0) / tick_rate_hz.max(1) as f32) as u32
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::{CarState, GridSlot, TrackConfig, TrackPoint};

    fn straight(length: f32) -> TrackConfig {
        TrackConfig {
            centerline: (0..=((length / 10.0) as usize))
                .map(|i| TrackPoint {
                    x: i as f32 * 10.0,
                    distance_from_start_m: i as f32 * 10.0,
                    ..Default::default()
                })
                .collect(),
            ..Default::default()
        }
    }

    fn car() -> CarState {
        CarState::new(
            uuid::Uuid::new_v4(),
            uuid::Uuid::new_v4(),
            &GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        )
    }

    #[test]
    fn sectors_default_to_even_thirds() {
        let track = straight(900.0);
        assert_eq!(sector_start_m(&track, 0), 0.0);
        assert!((sector_start_m(&track, 1) - 300.0).abs() < 0.001);
        assert!((sector_start_m(&track, 2) - 600.0).abs() < 0.001);
        assert_eq!(sector_of(&track, 0.0), 0);
        assert_eq!(sector_of(&track, 299.0), 0);
        assert_eq!(sector_of(&track, 300.0), 1);
        assert_eq!(sector_of(&track, 899.0), 2);
    }

    #[test]
    fn track_file_sectors_win() {
        let mut track = straight(900.0);
        track.sectors = vec![100.0, 200.0];
        assert!((sector_start_m(&track, 1) - 100.0).abs() < 0.001);
        assert_eq!(sector_of(&track, 150.0), 1);
        assert_eq!(sector_of(&track, 250.0), 2);
        assert_eq!(sector_boundaries_m(&track), vec![100.0, 200.0]);
    }

    #[test]
    fn file_sectors_need_every_boundary() {
        assert!(sectors_from_stations(vec![100.0], 900.0).is_empty());
        assert!(sectors_from_stations(vec![0.0, 950.0], 900.0).is_empty());
        assert_eq!(
            sectors_from_stations(vec![200.0, 100.0, 100.0, 400.0], 900.0),
            vec![100.0, 200.0]
        );
    }

    #[test]
    fn splits_always_add_up_to_the_lap() {
        let mut state = car();
        state.current_lap = 1;
        state.laps.start_lap(0);

        // 240 Hz: 20 s, then 25 s, then the lap closes at 70 s.
        let e1 = close_sector(&mut state, 1, 20 * 240, 240);
        assert_eq!(e1.sector_time_ms, 20_000);
        let e2 = close_sector(&mut state, 2, 45 * 240, 240);
        assert_eq!(e2.sector_time_ms, 25_000);
        let e3 = close_lap(&mut state, 1, 70_000, 70 * 240, 240);
        assert_eq!(e3.lap_time_ms, Some(70_000));
        assert_eq!(e3.splits_ms.iter().sum::<u32>(), 70_000);
        assert!(e3.valid);
        assert!(e3.personal_best_lap);
    }

    #[test]
    fn a_lap_with_a_missed_sector_is_not_valid() {
        let mut state = car();
        state.current_lap = 1;
        state.laps.start_lap(0);
        state.laps.sector = 2; // joined mid-lap: sectors 1 and 2 never timed
        let event = close_lap(&mut state, 1, 70_000, 70 * 240, 240);
        assert!(!event.valid);
        assert_eq!(state.laps.best_lap_ms, None);
    }

    #[test]
    fn four_wheels_off_for_long_enough_strikes_the_lap() {
        let mut state = car();
        state.laps.start_lap(0);
        state.wheels_off_track = true;
        for _ in 0..47 {
            note_track_limits(&mut state, 240);
        }
        assert!(!state.laps.invalid, "a brush past the kerb is not a cut");
        note_track_limits(&mut state, 240);
        assert!(state.laps.invalid);

        // Back on the road the lap stays struck until the line.
        state.wheels_off_track = false;
        note_track_limits(&mut state, 240);
        assert!(state.laps.invalid);
        assert_eq!(state.laps.off_track_ticks, 0);
    }

    #[test]
    fn a_struck_lap_never_becomes_a_best() {
        let mut state = car();
        state.current_lap = 1;
        state.laps.start_lap(0);
        close_sector(&mut state, 1, 20 * 240, 240);
        close_sector(&mut state, 2, 45 * 240, 240);
        state.laps.invalid = true;
        let event = close_lap(&mut state, 1, 70_000, 70 * 240, 240);
        assert!(!event.valid);
        assert!(!event.personal_best_lap);
        assert_eq!(state.laps.best_lap_ms, None);
        assert!(state.laps.last_invalid);
        assert!(!state.laps.invalid, "the next lap starts clean");
    }
}

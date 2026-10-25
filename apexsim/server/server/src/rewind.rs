//! Rewind in offline practice: a ring of whole-session snapshots, taken by
//! the game loop between ticks (`GameSession::snapshot`, outside the
//! simulation), and the `Rewind` that restores one of them.
//!
//! A session offers it when it is driven alone: a `Practice` session (the
//! kind the client creates for single-player) with one human among its cars,
//! who asks. A snapshot is taken every [`SNAPSHOT_SECONDS`] of session time
//! while it races in a mode with cars on the move; the ring holds
//! [`DEPTH_SECONDS`] of them, every one of the last [`FINE_SECONDS`] and
//! one every [`COARSE_SECONDS`] before (`keeps`), and starts again whenever the mode or the cars
//! change, the session stops racing, or a weekend phase begins. A rewind
//! restores the snapshot nearest the time asked for, drops the ring's newer
//! ones (a future that no longer happens), strikes every lap in progress
//! (`GameSession::rewind_to`) and holds the cars still for [`HOLD_MS`]
//! before the session runs on; the recording is cut there (`game_loop`).

use crate::data::{GameMode, PlayerId, SessionKind, SessionState};
use crate::game_session::{GameSession, SessionSnapshot};
use std::collections::VecDeque;

/// Session time between two snapshots, s.
pub const SNAPSHOT_SECONDS: u32 = 1;

/// How far back the ring reaches, s.
pub const DEPTH_SECONDS: u32 = 60;

/// The newest span the ring holds every snapshot of, s: a rewind of a
/// few seconds lands where it was asked.
pub const FINE_SECONDS: u32 = 10;

/// Further back, the ring holds one snapshot this often, s: the pause
/// menu's 10 s steps land on one, and a long circuit's ring stays a third
/// of its size (a snapshot is 4 MB at the Nordschleife).
pub const COARSE_SECONDS: u32 = 5;

/// How long the cars stand still after a rewind before the session runs
/// on, ms: the driver's hands back on the wheel.
pub const HOLD_MS: u32 = 3000;

/// The one human who drives a session alone (`SessionKind::Practice`, one
/// human car): the driver a rewind is offered to. `None` for any other
/// session.
pub fn sole_driver(session: &GameSession) -> Option<PlayerId> {
    if session.session.session_kind != SessionKind::Practice {
        return None;
    }
    let mut humans = session
        .session
        .participants
        .keys()
        .filter(|id| !session.session.ai_player_ids.contains(id));
    match (humans.next(), humans.next()) {
        (Some(only), None) => Some(*only),
        _ => None,
    }
}

/// The session keeps a ring now: driven alone, racing, in a mode whose cars
/// move under the physics.
pub fn takes_snapshots(session: &GameSession) -> bool {
    sole_driver(session).is_some()
        && session.session.state == SessionState::Racing
        && matches!(
            session.session.game_mode,
            GameMode::FreePractice
                | GameMode::Hotlap
                | GameMode::Qualification
                | GameMode::Practice
                | GameMode::Race
        )
}

/// Whether the ring still holds the snapshot taken at session tick `taken`
/// at tick `now`: the last [`FINE_SECONDS`] all, then one every
/// [`COARSE_SECONDS`] of session time back to [`DEPTH_SECONDS`].
fn keeps(now: u32, taken: u32, rate_hz: u32) -> bool {
    let age = now.saturating_sub(taken);
    age <= DEPTH_SECONDS * rate_hz
        && (age <= FINE_SECONDS * rate_hz || taken.is_multiple_of(COARSE_SECONDS * rate_hz))
}

/// A rewind done: the session tick rewound to and from.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Rewound {
    pub tick: u32,
    pub from_tick: u32,
}

/// One session's rewind state, kept by the server beside it.
#[derive(Default)]
pub struct Rewind {
    /// Oldest first.
    ring: VecDeque<SessionSnapshot>,
    /// Loop ticks the session stays still for.
    hold_ticks: u32,
    /// The recording was cut by a rewind: a new one starts as the session
    /// runs on.
    restart_recording: bool,
}

impl Rewind {
    /// After the session's tick: a snapshot when one is due.
    pub fn after_tick(&mut self, session: &mut GameSession) {
        let tick = session.session.current_tick;
        let every = SNAPSHOT_SECONDS * u32::from(session.tick_rate_hz()).max(1);
        if !tick.is_multiple_of(every) {
            return;
        }
        if let Some(newest) = self.ring.back() {
            let same_cars = newest
                .participants()
                .eq(session.session.participants.keys());
            if newest.game_mode() != session.session.game_mode || !same_cars {
                self.ring.clear();
            } else if newest.tick() >= tick {
                return;
            }
        }
        self.ring.push_back(session.snapshot());
        let rate = u32::from(session.tick_rate_hz()).max(1);
        self.ring.retain(|s| keeps(tick, s.tick(), rate));
    }

    /// Whether there is a snapshot to rewind to.
    pub fn can_rewind(&self) -> bool {
        !self.ring.is_empty()
    }

    /// Rewind `session` `seconds`: to the snapshot nearest that far back
    /// (the older of two as near), newer ones dropped, then held. `None`
    /// with nothing to rewind to.
    pub fn rewind(&mut self, session: &mut GameSession, seconds: u16) -> Option<Rewound> {
        let rate = u32::from(session.tick_rate_hz()).max(1);
        let from_tick = session.session.current_tick;
        let target = from_tick.saturating_sub(u32::from(seconds) * rate);
        let index = self
            .ring
            .iter()
            .enumerate()
            .min_by_key(|(_, s)| s.tick().abs_diff(target))
            .map(|(i, _)| i)?;
        self.ring.truncate(index + 1);
        let snapshot = self.ring.back()?;
        session.rewind_to(snapshot);
        self.hold_ticks = HOLD_MS * rate / 1000;
        Some(Rewound {
            tick: snapshot.tick(),
            from_tick,
        })
    }

    /// One loop tick of the hold after a rewind: true while the session
    /// stands still (not ticked, not recorded; telemetry goes on).
    pub fn hold(&mut self) -> bool {
        if self.hold_ticks == 0 {
            return false;
        }
        self.hold_ticks -= 1;
        true
    }

    /// The rewind cut the session's recording: a new one starts once the
    /// hold is over.
    pub fn restart_recording(&mut self) {
        self.restart_recording = true;
    }

    /// The recording was cut by a rewind and starts again now.
    pub fn take_restart_recording(&mut self) -> bool {
        std::mem::take(&mut self.restart_recording)
    }

    /// Snapshots in the ring.
    pub fn len(&self) -> usize {
        self.ring.len()
    }

    pub fn is_empty(&self) -> bool {
        self.ring.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_ring_thins_past_its_fine_span() {
        let rate = 420;
        let now = 120 * rate;
        let kept: Vec<u32> = (0..=120)
            .map(|s| s * rate)
            .filter(|&tick| keeps(now, tick, rate))
            .map(|tick| tick / rate)
            .collect();
        let mut want: Vec<u32> = (60..110).step_by(5).collect();
        want.extend(110..=120);
        assert_eq!(kept, want);
    }
}

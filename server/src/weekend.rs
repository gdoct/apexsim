//! A race weekend: optional timed practice, one to three timed qualifying
//! rounds run as a knockout, then the race, all in one session. The host
//! moves from one phase to the next (`ClientMessage::AdvanceWeekend`);
//! nothing advances on its own. This module holds the format, the knockout
//! arithmetic and the weekend's progress; `GameSession` runs the phases
//! (`advance_weekend`, `settle_weekend`).

use crate::data::{
    clamp_qualifying_seconds, PlayerId, MAX_QUALIFYING_SECONDS, MIN_QUALIFYING_SECONDS,
};
use serde::{Deserialize, Serialize};

/// The most qualifying rounds a weekend has.
pub const MAX_ROUNDS: usize = 3;
/// A round's length when the host names none.
pub const DEFAULT_ROUND_SECONDS: u32 = 900;

/// What a host asks for (`CreateSession.weekend`), echoed clamped in
/// `SessionJoined.Weekend`. Keys are snake_case and both always present.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WeekendFormat {
    /// Timed practice before qualifying, s; 0 for none.
    pub practice_seconds: u32,
    /// Each qualifying round's length, s, Q1 first.
    pub qualifying_seconds: Vec<u32>,
}

impl WeekendFormat {
    /// Practice 0 (none) or held in the qualifying range; one to
    /// [`MAX_ROUNDS`] rounds (extra ones dropped, none becomes one of
    /// [`DEFAULT_ROUND_SECONDS`]), each held in the qualifying range
    /// (`MIN_QUALIFYING_SECONDS` .. `MAX_QUALIFYING_SECONDS`).
    pub fn clamp(self) -> Self {
        let practice_seconds = clamp_qualifying_seconds(Some(self.practice_seconds)).unwrap_or(0);
        let mut rounds: Vec<u32> = self
            .qualifying_seconds
            .into_iter()
            .take(MAX_ROUNDS)
            .map(|s| s.clamp(MIN_QUALIFYING_SECONDS, MAX_QUALIFYING_SECONDS))
            .collect();
        if rounds.is_empty() {
            rounds.push(DEFAULT_ROUND_SECONDS);
        }
        Self {
            practice_seconds,
            qualifying_seconds: rounds,
        }
    }

    /// How many qualifying rounds.
    pub fn rounds(&self) -> usize {
        self.qualifying_seconds.len()
    }
}

/// How many cars take part in each qualifying round, Q1 first. With two
/// rounds or more the final has `ceil(field / 2)`; the cars between the
/// field and the final are knocked out evenly over the earlier rounds, any
/// remainder in the earliest. One round is the whole field. Never below 1.
/// The F1 format: 20 cars, 3 rounds -> 20, 15, 10.
pub fn knockout_sizes(field: usize, rounds: usize) -> Vec<usize> {
    let field = field.max(1);
    let rounds = rounds.max(1);
    if rounds == 1 {
        return vec![field];
    }
    let final_size = field.div_ceil(2);
    let out = field - final_size;
    let cuts = rounds - 1;
    let mut sizes = vec![field];
    let mut size = field;
    for k in 0..cuts {
        let cut = out / cuts + usize::from(k < out % cuts);
        size = (size - cut).max(1);
        sizes.push(size);
    }
    sizes
}

/// Where a weekend stands (`WeekendState.phase`).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum WeekendPhase {
    /// Not started: the session's lobby.
    #[default]
    Lobby = 0,
    /// Timed practice (`GameMode::Practice`).
    Practice = 1,
    /// A qualifying round (`GameMode::Qualification`).
    Qualifying = 2,
    /// The race, from its countdown on.
    Race = 3,
}

/// A car knocked out of qualifying, with the race grid slot it earned.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Eliminated {
    pub player_id: PlayerId,
    /// The round (1-based) it went out in.
    pub round: u8,
    /// 1-based race grid slot.
    pub grid_position: u8,
    /// Its best legal lap in that round, ms; 0 for none.
    pub lap_time_ms: u32,
    /// A driver who joined after Q1 began ([`WeekendProgress::enter_late`]):
    /// never in a round, at the back of the grid.
    pub late: bool,
}

/// A weekend's progress, kept by its `GameSession`.
#[derive(Debug, Clone, Default)]
pub struct WeekendProgress {
    pub phase: WeekendPhase,
    /// The qualifying round running or last run, 1-based; 0 outside
    /// qualifying.
    pub round: u8,
    /// `knockout_sizes` of the field seated when Q1 began.
    pub sizes: Vec<usize>,
    /// Every car knocked out so far, in grid order.
    pub eliminated: Vec<Eliminated>,
    /// The final round's order with each car's lap (0 for none), once it
    /// has been closed: the front of the grid.
    pub final_order: Vec<(PlayerId, u32)>,
    /// The current round has been ranked (its eliminations or the final
    /// order are in).
    pub round_closed: bool,
    /// Q1's ranking once closed, every car with its lap (0 for none): what
    /// a qualifying cut-off is measured on.
    pub first_round: Vec<(PlayerId, u32)>,
}

impl WeekendProgress {
    /// How many cars go through from the round running now: 0 in the final
    /// round and outside qualifying.
    pub fn advancing(&self) -> usize {
        if self.phase != WeekendPhase::Qualifying || self.round == 0 {
            return 0;
        }
        self.sizes.get(self.round as usize).copied().unwrap_or(0)
    }

    /// The round a car was knocked out in, if it was.
    pub fn eliminated_in(&self, player_id: &PlayerId) -> Option<u8> {
        self.eliminated_entry(player_id).map(|e| e.round)
    }

    /// A knocked-out car's entry, if it is one.
    pub fn eliminated_entry(&self, player_id: &PlayerId) -> Option<&Eliminated> {
        self.eliminated.iter().find(|e| e.player_id == *player_id)
    }

    /// A driver joins after Q1 began, between rounds or once the final has
    /// been run: no part in the rounds left, a grid slot behind every car
    /// that has one (the Q1 field, every earlier late entry), so it counts
    /// as knocked out in the round just run with no time.
    pub fn enter_late(&mut self, player_id: PlayerId) {
        if self.eliminated_in(&player_id).is_some() {
            return;
        }
        let last = self
            .eliminated
            .iter()
            .map(|e| e.grid_position as usize)
            .chain(self.sizes.first().copied())
            .chain(std::iter::once(self.final_order.len()))
            .max()
            .unwrap_or(0);
        self.eliminated.push(Eliminated {
            player_id,
            round: self.round.max(1),
            grid_position: (last + 1).min(u8::MAX as usize) as u8,
            lap_time_ms: 0,
            late: true,
        });
    }

    /// The race grid the weekend set, front first: the final round's order,
    /// then every knocked-out car by its slot. `None` until the final round
    /// has been ranked.
    pub fn grid(&self) -> Option<Vec<PlayerId>> {
        if self.final_order.is_empty() {
            return None;
        }
        Some(
            self.final_order
                .iter()
                .map(|(id, _)| *id)
                .chain(self.eliminated.iter().map(|e| e.player_id))
                .collect(),
        )
    }

    /// Rank a round that has ended. `ranking` is its participants, best
    /// first (no time behind every time, then seating order), each with its
    /// lap (0 for none); `rounds` how many the weekend has. A round before
    /// the final knocks out the cars beyond the next round's size, which
    /// take grid slots `next + 1 ..` in that order; the final sets the front
    /// of the grid.
    pub fn close_round(&mut self, ranking: &[(PlayerId, u32)], rounds: usize) {
        let round = self.round as usize;
        if round == 1 {
            self.first_round = ranking.to_vec();
        }
        if round < rounds {
            let keep = self.sizes.get(round).copied().unwrap_or(ranking.len());
            for (i, (id, lap_time_ms)) in ranking.iter().enumerate().skip(keep) {
                self.eliminated.push(Eliminated {
                    player_id: *id,
                    round: self.round,
                    grid_position: (i + 1).min(u8::MAX as usize) as u8,
                    lap_time_ms: *lap_time_ms,
                    late: false,
                });
            }
            self.eliminated.sort_by_key(|e| e.grid_position);
        } else {
            self.final_order = ranking.to_vec();
        }
        self.round_closed = true;
    }
}

/// The refusal a knocked-out car gets for going out again.
pub fn eliminated_reason(entry: &Eliminated) -> &'static str {
    if entry.late {
        return "Joined after qualifying began: starts from the back";
    }
    match entry.round {
        1 => "Eliminated in Q1",
        2 => "Eliminated in Q2",
        _ => "Eliminated in qualifying",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    #[test]
    fn knockout_sizes_halve_the_field_by_the_final() {
        assert_eq!(knockout_sizes(20, 3), vec![20, 15, 10]);
        assert_eq!(knockout_sizes(4, 3), vec![4, 3, 2]);
        assert_eq!(knockout_sizes(22, 3), vec![22, 16, 11]);
        assert_eq!(knockout_sizes(20, 2), vec![20, 10]);
        assert_eq!(knockout_sizes(5, 2), vec![5, 3]);
        assert_eq!(knockout_sizes(3, 3), vec![3, 2, 2]);
        assert_eq!(knockout_sizes(1, 3), vec![1, 1, 1]);
        assert_eq!(knockout_sizes(2, 3), vec![2, 1, 1]);
        assert_eq!(knockout_sizes(12, 1), vec![12]);
        assert_eq!(knockout_sizes(0, 1), vec![1]);
        assert_eq!(knockout_sizes(0, 3), vec![1, 1, 1]);
    }

    #[test]
    fn a_format_is_clamped() {
        let format = WeekendFormat {
            practice_seconds: 5,
            qualifying_seconds: vec![10, 900, 99_999, 600],
        }
        .clamp();
        assert_eq!(format.practice_seconds, 60);
        assert_eq!(format.qualifying_seconds, vec![60, 900, 7200]);
        let none = WeekendFormat {
            practice_seconds: 0,
            qualifying_seconds: vec![],
        }
        .clamp();
        assert_eq!(none.practice_seconds, 0);
        assert_eq!(none.qualifying_seconds, vec![DEFAULT_ROUND_SECONDS]);
        let zero_round = WeekendFormat {
            practice_seconds: 3600,
            qualifying_seconds: vec![0],
        }
        .clamp();
        assert_eq!(zero_round.practice_seconds, 3600);
        assert_eq!(zero_round.qualifying_seconds, vec![60]);
    }

    #[test]
    fn closing_rounds_builds_the_grid() {
        let ids: Vec<PlayerId> = (1..=4).map(Uuid::from_u128).collect();
        let mut weekend = WeekendProgress {
            phase: WeekendPhase::Qualifying,
            round: 1,
            sizes: knockout_sizes(4, 2),
            ..Default::default()
        };
        assert_eq!(weekend.advancing(), 2);
        weekend.close_round(
            &[
                (ids[2], 80_000),
                (ids[0], 81_000),
                (ids[1], 82_000),
                (ids[3], 0),
            ],
            2,
        );
        assert_eq!(weekend.eliminated.len(), 2);
        assert_eq!(weekend.eliminated[0].player_id, ids[1]);
        assert_eq!(weekend.eliminated[0].grid_position, 3);
        assert_eq!(weekend.eliminated[1].player_id, ids[3]);
        assert_eq!(weekend.eliminated[1].grid_position, 4);
        assert_eq!(weekend.eliminated[1].lap_time_ms, 0);
        assert_eq!(weekend.eliminated_in(&ids[3]), Some(1));
        assert_eq!(weekend.grid(), None);

        weekend.round = 2;
        weekend.round_closed = false;
        assert_eq!(weekend.advancing(), 0, "the final");
        weekend.close_round(&[(ids[0], 79_000), (ids[2], 79_500)], 2);
        assert_eq!(weekend.grid(), Some(vec![ids[0], ids[2], ids[1], ids[3]]));
    }
}

//! The race's incident list for stewards: contacts between cars, passes
//! under a yellow, track-limit offences, unsafe rejoins and the penalties
//! race control gave or a steward took back, each with the tick and the lap
//! it happened on.
//!
//! The list is filled from what the race's tick already finds (the
//! collision pass's touching pairs, race control's yellow passes, the lap
//! timing's track limits, `GameSession::penalise`); it reads nothing of its
//! own and changes nothing the simulation reads, so a race with it runs
//! exactly as one without. The server writes it beside the race's results
//! and the admin dashboard lists it (docs/server/sessions.md, "Incidents").

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::data::PlayerId;
use crate::race_control::{Offence, Penalty, PenaltyKind};

/// A contact under this closing speed does no damage
/// (`damage::IMPACT_THRESHOLD_MPS`): a touch.
pub const CONTACT_TOUCH_MPS: f32 = crate::damage::IMPACT_THRESHOLD_MPS;
/// A contact closing at this or more is a heavy one.
pub const CONTACT_HEAVY_MPS: f32 = 8.0;
/// Two cars apart for this long and touching again are a new contact; less,
/// and it is the same one going on (cars rubbing come apart and back
/// together from tick to tick).
pub const CONTACT_GAP_S: f32 = 1.0;
/// A car back on the road after every wheel was off for
/// `laps::TRACK_LIMITS_SECONDS` rejoined unsafely when a car behind it on
/// the lap closes on it faster than this...
pub const REJOIN_CLOSING_MPS: f32 = 8.0;
/// ... and would reach it within this, s.
pub const REJOIN_REACH_S: f32 = 2.0;

/// Of the cars behind a car that rejoins at lap distance `at_m` going
/// `speed_mps` (each car behind as its lap distance and speed, its id
/// first), the one it rejoined in front of unsafely: the one that would
/// reach it soonest within [`REJOIN_REACH_S`], closing faster than
/// [`REJOIN_CLOSING_MPS`]; with how fast it closed.
pub fn unsafe_rejoin(
    at_m: f32,
    speed_mps: f32,
    behind: impl IntoIterator<Item = (PlayerId, f32, f32)>,
) -> Option<(PlayerId, f32)> {
    behind
        .into_iter()
        .filter_map(|(id, run_m, speed)| {
            let gap = at_m - run_m;
            let closing = speed - speed_mps;
            (gap > 0.0 && closing > REJOIN_CLOSING_MPS && gap / closing < REJOIN_REACH_S)
                .then_some((id, closing, gap / closing))
        })
        .min_by(|a, b| a.2.total_cmp(&b.2))
        .map(|(id, closing, _)| (id, closing))
}

/// Two cars the collision pass found touching this tick
/// (`physics::check_collisions_refs`).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CarContact {
    pub a: PlayerId,
    pub b: PlayerId,
    /// How fast they were closing along the contact's normal, m/s.
    pub closing_mps: f32,
}

/// How hard a contact was, by its closing speed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Severity {
    /// Under [`CONTACT_TOUCH_MPS`]: no damage.
    Touch,
    /// Under [`CONTACT_HEAVY_MPS`].
    Light,
    Heavy,
}

impl Severity {
    pub fn of(closing_mps: f32) -> Severity {
        if closing_mps < CONTACT_TOUCH_MPS {
            Severity::Touch
        } else if closing_mps < CONTACT_HEAVY_MPS {
            Severity::Light
        } else {
            Severity::Heavy
        }
    }
}

/// What happened.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum IncidentKind {
    /// The car and `other` touched; `closing_mps` is the hardest the
    /// contact closed over its length.
    Contact {
        other: PlayerId,
        closing_mps: f32,
        severity: Severity,
    },
    /// The car passed `passed` in a yellow sector (or under a safety car),
    /// as race control saw it; penalised or not by the rules.
    YellowPass { passed: PlayerId },
    /// The car's lap was struck: every wheel off the track for
    /// `laps::TRACK_LIMITS_SECONDS`, or (`cut`) the line reached without
    /// every checkpoint.
    TrackLimits { cut: bool },
    /// Race control gave the car a penalty.
    Penalty {
        offence: String,
        penalty: String,
        seconds: u16,
    },
    /// A steward took a penalty back (`GameSession::clear_penalty`).
    PenaltyCleared { offence: String, penalty: String },
    /// The car came back onto the road from a trip off it (every wheel off
    /// for `laps::TRACK_LIMITS_SECONDS`) in front of `other`, closing on it
    /// at `closing_mps` and due to reach it within [`REJOIN_REACH_S`]
    /// ([`unsafe_rejoin`]).
    UnsafeRejoin { other: PlayerId, closing_mps: f32 },
}

/// One incident.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Incident {
    /// The session tick it happened on (a contact: the tick it began).
    pub tick: u32,
    /// The lap `car` was on.
    pub lap: u16,
    pub car: PlayerId,
    #[serde(flatten)]
    pub kind: IncidentKind,
}

/// Every offence, in code order.
pub const OFFENCES: [Offence; 6] = [
    Offence::JumpStart,
    Offence::YellowPass,
    Offence::PitExitRed,
    Offence::Steward,
    Offence::TrackLimits,
    Offence::UnsafeRejoin,
];

/// The offence an [`offence_name`] names.
pub fn offence_named(name: &str) -> Option<Offence> {
    OFFENCES.into_iter().find(|o| offence_name(*o) == name)
}

/// An offence's name in the list.
pub fn offence_name(offence: Offence) -> &'static str {
    match offence {
        Offence::JumpStart => "jump_start",
        Offence::YellowPass => "yellow_pass",
        Offence::PitExitRed => "pit_exit_red",
        Offence::Steward => "steward",
        Offence::TrackLimits => "track_limits",
        Offence::UnsafeRejoin => "unsafe_rejoin",
    }
}

/// A penalty kind's name in the list.
pub fn penalty_name(kind: PenaltyKind) -> &'static str {
    match kind {
        PenaltyKind::Time => "time",
        PenaltyKind::DriveThrough => "drive_through",
        PenaltyKind::StopGo => "stop_go",
    }
}

/// The race's incidents, in the order they happened. Emptied as the field
/// lines up for a race.
#[derive(Debug, Clone, Default)]
pub struct IncidentLog {
    pub incidents: Vec<Incident>,
    /// The session tick the server's replay of this race starts on, set by
    /// the server as it starts recording (nothing in the sim reads it): an
    /// incident's place in the replay is its tick less this.
    pub replay_start_tick: Option<u32>,
    /// Pairs in contact (lower id first): the incident it is and the last
    /// tick they touched.
    touching: BTreeMap<(PlayerId, PlayerId), (usize, u32)>,
}

impl IncidentLog {
    pub fn clear(&mut self) {
        self.incidents.clear();
        self.touching.clear();
        self.replay_start_tick = None;
    }

    pub fn push(&mut self, incident: Incident) {
        self.incidents.push(incident);
    }

    /// This tick's touching pairs: a pair not touching within
    /// [`CONTACT_GAP_S`] (`gap_ticks`) before is a new contact, on the lap
    /// `lap_of` gives its first car; one still touching keeps its hardest
    /// closing speed.
    pub fn note_contacts(
        &mut self,
        contacts: &[CarContact],
        tick: u32,
        gap_ticks: u32,
        lap_of: impl Fn(&PlayerId) -> u16,
    ) {
        self.touching
            .retain(|_, (_, last)| tick.saturating_sub(*last) <= gap_ticks);
        for contact in contacts {
            let key = if contact.a <= contact.b {
                (contact.a, contact.b)
            } else {
                (contact.b, contact.a)
            };
            if let Some((index, last)) = self.touching.get_mut(&key) {
                *last = tick;
                if let Some(Incident {
                    kind:
                        IncidentKind::Contact {
                            closing_mps,
                            severity,
                            ..
                        },
                    ..
                }) = self.incidents.get_mut(*index)
                {
                    if contact.closing_mps > *closing_mps {
                        *closing_mps = contact.closing_mps;
                        *severity = Severity::of(contact.closing_mps);
                    }
                }
                continue;
            }
            self.touching.insert(key, (self.incidents.len(), tick));
            self.incidents.push(Incident {
                tick,
                lap: lap_of(&key.0),
                car: key.0,
                kind: IncidentKind::Contact {
                    other: key.1,
                    closing_mps: contact.closing_mps,
                    severity: Severity::of(contact.closing_mps),
                },
            });
        }
    }

    /// A penalty given: the incident for it.
    pub fn note_penalty(&mut self, car: PlayerId, penalty: &Penalty) {
        self.incidents.push(Incident {
            tick: penalty.tick,
            lap: penalty.lap,
            car,
            kind: IncidentKind::Penalty {
                offence: offence_name(penalty.offence).to_string(),
                penalty: penalty_name(penalty.kind).to_string(),
                seconds: penalty.seconds,
            },
        });
    }
}

#[cfg(test)]
mod tests {

    /// Every offence comes back from its name, as its code
    /// (`PenaltyData::reason`): a steward's decision and track limits went
    /// out as a jump start.
    #[test]
    fn every_offence_is_found_by_its_name() {
        for (code, offence) in OFFENCES.into_iter().enumerate() {
            assert_eq!(offence as usize, code);
            assert_eq!(offence_named(offence_name(offence)), Some(offence));
        }
        assert_eq!(offence_named("nonsense"), None);
    }
    use super::*;

    fn id(n: u128) -> PlayerId {
        uuid::Uuid::from_u128(n)
    }

    #[test]
    fn a_contact_is_one_incident_while_it_lasts_and_keeps_its_hardest_hit() {
        let mut log = IncidentLog::default();
        let touch = |mps| CarContact {
            a: id(2),
            b: id(1),
            closing_mps: mps,
        };
        log.note_contacts(&[touch(1.0)], 100, 420, |_| 3);
        log.note_contacts(&[touch(9.0)], 101, 420, |_| 3);
        log.note_contacts(&[], 102, 420, |_| 3);
        log.note_contacts(&[touch(0.5)], 300, 420, |_| 3);
        assert_eq!(log.incidents.len(), 1, "{:?}", log.incidents);
        let incident = &log.incidents[0];
        assert_eq!((incident.tick, incident.lap, incident.car), (100, 3, id(1)));
        assert_eq!(
            incident.kind,
            IncidentKind::Contact {
                other: id(2),
                closing_mps: 9.0,
                severity: Severity::Heavy
            }
        );
        // Apart for longer than the gap: the next touch is a new contact.
        log.note_contacts(&[touch(3.0)], 1000, 420, |_| 4);
        assert_eq!(log.incidents.len(), 2);
        assert_eq!(log.incidents[1].lap, 4);
    }

    #[test]
    fn an_incident_is_written_flat_with_its_kind() {
        let incident = Incident {
            tick: 7,
            lap: 2,
            car: id(1),
            kind: IncidentKind::YellowPass { passed: id(2) },
        };
        let json = serde_json::to_value(&incident).unwrap();
        assert_eq!(json["kind"], "yellow_pass");
        assert_eq!(json["lap"], 2);
        let back: Incident = serde_json::from_value(json).unwrap();
        assert_eq!(back, incident);
    }
}

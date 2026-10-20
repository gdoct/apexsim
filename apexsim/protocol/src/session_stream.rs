//! A session as spectator-stream records, tick by tick (docs/game/spectator.md).
//!
//! [`SessionStream`] is the one producer the offline render tool, the server's
//! own replay recorder and the live spectators all draw on: it turns a
//! [`GameSession`] into events, roster changes, frames, the slow state and
//! the road, with the cadence the stream format promises, and nothing else
//! decides when a record is due. The consumers differ only in where the
//! bodies go: a file's blocks, a TCP message, a UDP datagram.
//!
//! Nothing here reads a clock: the records follow the session's own tick, so
//! a seeded render writes the same bytes twice.

use std::collections::{BTreeSet, HashMap};

use crate::data::*;
use crate::game_session::{GameSession, PitService, SessionLapEvent};
use crate::spectator::{
    BroadcastEncoder, CarRow, Record, StreamGhost, StreamHeader, StreamPath, StreamRoster,
    EVENT_FINISH, EVENT_LAP_TIMING, EVENT_PIT_SERVICE, EVENT_PIT_STOP, EVENT_RETIRED,
    EVENT_SESSION_STATE, RECORD_EVENT, RECORD_FRAME, RECORD_GHOST, RECORD_ROAD, RECORD_ROSTER,
    RECORD_STATE, STATE_EVERY_S,
};

/// The most timing and pit events a stream remembers for newcomers: an
/// endurance race of twenty cars is well under it.
const MAX_HISTORY_EVENTS: usize = 200_000;

/// One record a tick produced, ready to be framed into a file or sent.
#[derive(Debug, Clone, PartialEq)]
pub struct StreamRecord {
    pub tick: u32,
    /// `spectator::RECORD_*`: frames go out as datagrams, the rest reliably.
    pub kind: u8,
    pub body: Vec<u8>,
}

/// What one [`SessionStream::tick`] produced.
#[derive(Debug, Default)]
pub struct StreamTick {
    pub records: Vec<StreamRecord>,
    /// The cars' rows, when this tick was a frame tick (the render tool
    /// scores a race on them).
    pub rows: Option<Vec<CarRow>>,
}

/// What a viewer who joins a stream part-way needs that no frame holds
/// (the same rule `showcase::catch_up_bodies` applies to a file): the timing
/// and pit events so far, the road as last laid, the latest slow state and
/// a watched hotlap's ghost.
#[derive(Default)]
struct History {
    events: Vec<Vec<u8>>,
    session_state: Option<Vec<u8>>,
    /// Road slices keyed by their first cell and length, oldest first: a
    /// slice replaces only one of the same cells.
    roads: Vec<((u32, usize), Vec<u8>)>,
    state: Option<Vec<u8>>,
    ghost: Option<Vec<u8>>,
}

impl History {
    fn note(&mut self, kind: u8, body: &[u8]) {
        match kind {
            // [4, epoch (5 bytes), tick (5 bytes), kind, payload]
            RECORD_EVENT => match body.get(12).copied() {
                Some(EVENT_SESSION_STATE) => self.session_state = Some(body.to_vec()),
                Some(
                    EVENT_LAP_TIMING | EVENT_PIT_STOP | EVENT_PIT_SERVICE | EVENT_FINISH
                    | EVENT_RETIRED,
                ) if self.events.len() < MAX_HISTORY_EVENTS => self.events.push(body.to_vec()),
                _ => {}
            },
            RECORD_STATE => self.state = Some(body.to_vec()),
            RECORD_GHOST => self.ghost = Some(body.to_vec()),
            RECORD_ROAD => {
                if let Ok(Record::Road(road)) = Record::decode(body) {
                    let key = (road.first_cell, road.rows.len());
                    self.roads.retain(|(k, _)| *k != key);
                    self.roads.push((key, body.to_vec()));
                }
            }
            _ => {}
        }
    }
}

pub struct SessionStream {
    pub encoder: BroadcastEncoder,
    record_every: u32,
    state_every: u32,
    road_every: u32,
    roster: StreamRoster,
    road_burst_done: bool,
    history: History,
    /// The watched hotlap's ghost revision last written
    /// (`GameSession::hotlap_watch_ghost`).
    ghost_revision: Option<u32>,
}

impl SessionStream {
    /// A stream of `session` at about `frame_hz` frames a second, epoch 0.
    pub fn new(session: &GameSession, frame_hz: u16, names: &HashMap<PlayerId, String>) -> Self {
        let tick_rate = session.tick_rate_hz().max(1);
        let mut encoder = BroadcastEncoder::new(0);
        let roster = StreamRoster::from_session_named(session, 0, 0, names);
        // The state the stream opens in is not an event.
        encoder.observe(session, &[]);
        Self {
            encoder,
            record_every: crate::render::frame_spacing(tick_rate, frame_hz),
            state_every: ((STATE_EVERY_S * tick_rate as f32).round() as u32).max(1),
            road_every: ((crate::road_state::ROAD_SEND_SECONDS * tick_rate as f32).round() as u32)
                .max(1),
            roster,
            road_burst_done: false,
            history: History::default(),
            ghost_revision: None,
        }
    }

    /// The YAML stem the header names the track by.
    pub fn track_stem(session: &GameSession) -> String {
        session
            .track_config
            .source_path
            .as_deref()
            .and_then(|p| std::path::Path::new(p).file_stem())
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    /// Everything a viewer joining now is sent, in order, before the records
    /// of the ticks to come: the preamble (header, roster, path, sector
    /// lines), the timing and pit events so far, the road as last laid, the
    /// latest slow state and a watched hotlap's ghost.
    pub fn catch_up(&self, session: &GameSession) -> Vec<Vec<u8>> {
        let mut bodies = vec![
            self.header(session, &Self::track_stem(session)).encode(),
            self.roster.encode(),
            self.path(session).encode(),
            self.encoder.sectors(session).encode(),
        ];
        bodies.extend(self.history.events.iter().cloned());
        bodies.extend(self.history.session_state.iter().cloned());
        bodies.extend(self.history.roads.iter().map(|(_, body)| body.clone()));
        bodies.extend(self.history.state.iter().cloned());
        bodies.extend(self.history.ghost.iter().cloned());
        bodies
    }

    /// Ticks between two frames.
    pub fn record_every(&self) -> u32 {
        self.record_every
    }

    /// Frames a second, as the header says it.
    pub fn frame_rate(&self, session: &GameSession) -> u16 {
        crate::render::frames_per_second(session.tick_rate_hz(), self.record_every)
    }

    /// The header of the stream as it begins; the content's last tick and a
    /// render's seed are the caller's to fill in.
    pub fn header(&self, session: &GameSession, track_stem: &str) -> StreamHeader {
        let mut header = self.encoder.header(session, track_stem);
        header.frame_rate = self.frame_rate(session);
        header
    }

    /// The roster as it stands (the last one written).
    pub fn roster(&self) -> &StreamRoster {
        &self.roster
    }

    pub fn path(&self, session: &GameSession) -> StreamPath {
        self.encoder.path(session)
    }

    /// Everything one tick of `session` adds to the stream. Call once per
    /// simulated tick, with the timing lines crossed and the pit stops
    /// started in it (drained by the caller). `roster_changed` says the
    /// session's membership or liveries moved (`GameSession::roster_is_dirty`)
    /// and the roster is to be looked at again, with `names` for the humans.
    pub fn tick(
        &mut self,
        session: &GameSession,
        laps: &[SessionLapEvent],
        pit_starts: &[(PlayerId, PitService)],
        names: &HashMap<PlayerId, String>,
        roster_changed: bool,
    ) -> StreamTick {
        let tick = session.session.current_tick;
        let mut out = StreamTick::default();
        let history = &mut self.history;
        let mut push = |kind: u8, body: Vec<u8>| {
            history.note(kind, &body);
            out.records.push(StreamRecord { tick, kind, body });
        };

        for event in self.encoder.observe(session, laps) {
            push(RECORD_EVENT, event.encode());
        }
        for event in self.encoder.pit_services(session, pit_starts) {
            push(RECORD_EVENT, event.encode());
        }
        if roster_changed {
            let next = StreamRoster::from_session_named(
                session,
                self.encoder.epoch,
                self.encoder.roster_revision.wrapping_add(1),
                names,
            );
            if next.entries != self.roster.entries {
                self.encoder.roster_revision = next.revision;
                push(RECORD_ROSTER, next.encode());
                self.roster = next;
            }
        }
        // A watched hotlap's ghost, each time a faster lap replaces it (and
        // the one an earlier watch left, at once).
        if let Some((revision, lap)) = session.hotlap_watch_ghost() {
            if self.ghost_revision != Some(revision) {
                self.ghost_revision = Some(revision);
                let car = session
                    .session
                    .ai_player_ids
                    .first()
                    .and_then(|id| session.session.participants.get(id))
                    .map(|c| c.car_config_id)
                    .unwrap_or_default();
                let ghost = StreamGhost::from_lap(
                    self.encoder.epoch,
                    tick,
                    session.session.track_config_id,
                    car,
                    lap,
                );
                push(RECORD_GHOST, ghost.encode());
            }
        }
        // The slow state: the sky, a timed race's clock, every car's tyres
        // and tank, a few times a second.
        if tick.is_multiple_of(self.state_every) {
            push(RECORD_STATE, self.encoder.state(session).encode());
        }
        let frame_tick = tick.is_multiple_of(self.record_every);
        if frame_tick {
            let rows = BroadcastEncoder::rows(session);
            for body in self.encoder.frame_of(session, &rows) {
                push(RECORD_FRAME, body);
            }
            out.rows = Some(rows);
        }
        // The road, round-robin over the lap: the next slice every couple of
        // seconds, so a stream carries the track rubbering in and drying
        // just as a live client sees it.
        let cells = session
            .track_config
            .road_state
            .as_ref()
            .map_or(0, |r| r.cell_count());
        if cells > 0 {
            if !self.road_burst_done && frame_tick {
                // The whole lap once, with the first frame: a viewer opens on
                // the road as the host set it, not on a lap filling in.
                self.road_burst_done = true;
                let mut first = 0;
                while first < cells {
                    let count = crate::road_state::ROAD_CELLS_PER_JOIN.min(cells - first);
                    if let Some(road) = self.encoder.road(session, first, count) {
                        push(RECORD_ROAD, road.encode());
                    }
                    first += count;
                }
            } else if tick.is_multiple_of(self.road_every) {
                let first = ((tick / self.road_every) as usize
                    * crate::road_state::ROAD_CELLS_PER_SEND)
                    % cells;
                if let Some(road) =
                    self.encoder
                        .road(session, first, crate::road_state::ROAD_CELLS_PER_SEND)
                {
                    push(RECORD_ROAD, road.encode());
                }
            }
        }
        out
    }
}

/// Who is watching which session as a stream (`SpectateSession`), and who
/// has not yet been sent the catch-up.
#[derive(Debug, Default)]
pub struct LiveWatchers {
    watching: HashMap<PlayerId, SessionId>,
    newcomers: BTreeSet<PlayerId>,
}

impl LiveWatchers {
    /// Put a player on a session's stream, leaving whichever they watched.
    pub fn join(&mut self, player: PlayerId, session: SessionId) {
        self.leave(&player);
        self.watching.insert(player, session);
        self.newcomers.insert(player);
    }

    /// Take a player off their stream. True when they were on one.
    pub fn leave(&mut self, player: &PlayerId) -> bool {
        self.newcomers.remove(player);
        self.watching.remove(player).is_some()
    }

    pub fn watching(&self, player: &PlayerId) -> Option<SessionId> {
        self.watching.get(player).copied()
    }

    pub fn is_empty(&self) -> bool {
        self.watching.is_empty()
    }

    pub fn has_viewers(&self, session: &SessionId) -> bool {
        self.watching.values().any(|s| s == session)
    }

    /// Everyone on a session's stream who has had the catch-up.
    pub fn viewers(&self, session: &SessionId) -> Vec<PlayerId> {
        let mut viewers: Vec<PlayerId> = self
            .watching
            .iter()
            .filter(|(player, s)| *s == session && !self.newcomers.contains(player))
            .map(|(player, _)| *player)
            .collect();
        viewers.sort();
        viewers
    }

    /// The viewers of a session who have yet to be sent the catch-up, now
    /// counted as sent.
    pub fn take_newcomers(&mut self, session: &SessionId) -> Vec<PlayerId> {
        let new: Vec<PlayerId> = self
            .newcomers
            .iter()
            .filter(|p| self.watching.get(p) == Some(session))
            .copied()
            .collect();
        for player in &new {
            self.newcomers.remove(player);
        }
        new
    }

    /// The sessions somebody watches.
    pub fn watched_sessions(&self) -> Vec<SessionId> {
        let mut sessions: Vec<SessionId> = self.watching.values().copied().collect();
        sessions.sort();
        sessions.dedup();
        sessions
    }

    /// A session is gone: its viewers are watching nothing.
    pub fn forget_session(&mut self, session: &SessionId) -> Vec<PlayerId> {
        let gone: Vec<PlayerId> = self
            .watching
            .iter()
            .filter(|(_, s)| *s == session)
            .map(|(p, _)| *p)
            .collect();
        for player in &gone {
            self.leave(player);
        }
        gone
    }
}

//! Race events: a weekend that outlives its session and the server
//! (docs/server/sessions.md "Events across restarts").
//!
//! An event is a hosted weekend (`admin::hosted::HostedSpec`) with a name,
//! an entry list of drivers by their proved driver id (`driver_keys`), each
//! with the car and livery they race for the whole event, the phase it is
//! at (practice, qualifying, race, done) and each finished phase's
//! results. It is kept in `<records>/events/<id>.json`, its id the slug of
//! its name, so the same names make the same files.
//!
//! The server runs one session for an event while it is open
//! (`Event::opened`): the event runner ([`run_once`], every second) opens
//! it when it has none (at startup that is the resume), records each phase
//! as it finishes, and an event resumed in a fresh session starts at its
//! next phase: the phases before are skipped (`GameSession::resume_weekend`)
//! and the race grids from the stored qualifying (`@driver:` and `@ai:`
//! references, `GameSession::set_grid_order`). A phase that was running
//! when the server stopped is run again from its start.
//!
//! Nothing here runs in the simulation: the runner reads the session's
//! state under the server lock and hands the resume data in.

use crate::admin::hosted::{self, HostedSessions, HostedSpec};
use crate::data::*;
use crate::game_loop::GameLoopCtx;
use crate::game_session::GameSession;
use crate::weekend::WeekendPhase;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tracing::{info, warn};

/// The event files' folder under `[records] dir`.
pub const EVENTS_DIR: &str = "events";
/// The file layout's version (`Event::version`).
pub const EVENT_FILE_VERSION: u32 = 1;
/// The longest event name kept.
pub const MAX_EVENT_NAME: usize = 60;

/// Where an event stands: the phase it runs next (or is running).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum EventPhase {
    Practice,
    Qualifying,
    Race,
    /// The race is over and its result kept.
    Done,
}

/// A driver entered in an event: known by their proved driver id, racing
/// the car and livery they entered with in every session of it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EventEntry {
    pub driver_id: DriverId,
    /// The name they last joined under (display only).
    pub name: String,
    pub car_id: CarConfigId,
    #[serde(default)]
    pub livery: u8,
}

/// One line of a finished phase's results.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ResultLine {
    /// 1-based: practice by best lap, qualifying the grid it set, the race
    /// its classification.
    pub position: u16,
    pub name: String,
    /// A human's proved driver id.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub driver_id: Option<DriverId>,
    /// An AI car's place in the session's seating order, 1-based (what
    /// `@ai:N` names).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ai: Option<u8>,
    pub car_id: CarConfigId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub best_lap_ms: Option<u32>,
    #[serde(default)]
    pub laps: u16,
    /// The race only: from the green to the flag.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub race_time_ms: Option<u32>,
    /// The race only: finished, running or retired.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub status: Option<String>,
    /// Qualifying only: the round the car went out in.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub knocked_out_in: Option<u8>,
}

impl ResultLine {
    /// The grid reference that names this line's driver in a later session.
    pub fn grid_ref(&self) -> String {
        match (self.driver_id, self.ai) {
            (Some(id), _) => crate::grid_order::driver_ref(id),
            (None, Some(n)) => crate::grid_order::ai_ref(n as usize),
            _ => self.name.clone(),
        }
    }
}

/// A finished phase.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PhaseResult {
    pub phase: EventPhase,
    /// When it was recorded, Unix s (display only).
    #[serde(default)]
    pub finished_at: u64,
    pub lines: Vec<ResultLine>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Event {
    #[serde(default)]
    pub version: u32,
    /// The slug of its name ([`slug`]); the file's stem.
    pub id: String,
    pub name: String,
    /// The weekend: track, car class, AI, lengths, weather, rules.
    pub spec: HostedSpec,
    /// A proved driver who joins and is not entered yet is entered with
    /// the car and livery they join with, up to `spec.player_slots`;
    /// closed, only the entered drivers may drive.
    #[serde(default = "default_true")]
    pub entries_open: bool,
    #[serde(default)]
    pub entries: Vec<EventEntry>,
    pub phase: EventPhase,
    /// The server keeps a session open for it (and opens one again after a
    /// restart) until it is done or closed.
    #[serde(default)]
    pub opened: bool,
    #[serde(default)]
    pub results: Vec<PhaseResult>,
    /// Unix s (display only).
    #[serde(default)]
    pub created_at: u64,
    /// What went wrong opening its session last, if anything.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_error: Option<String>,
}

fn default_true() -> bool {
    true
}

impl Event {
    pub fn new(id: String, name: String, spec: HostedSpec, entries_open: bool, now: u64) -> Self {
        let phase = first_phase(&spec);
        Self {
            version: EVENT_FILE_VERSION,
            id,
            name,
            spec,
            entries_open,
            entries: Vec::new(),
            phase,
            opened: false,
            results: Vec::new(),
            created_at: now,
            last_error: None,
        }
    }

    /// The phase after `phase` for this event's format.
    fn after(&self, phase: EventPhase) -> EventPhase {
        match phase {
            EventPhase::Practice => EventPhase::Qualifying,
            EventPhase::Qualifying => EventPhase::Race,
            _ => EventPhase::Done,
        }
    }

    pub fn result(&self, phase: EventPhase) -> Option<&PhaseResult> {
        self.results.iter().find(|r| r.phase == phase)
    }

    pub fn entry(&self, driver_id: DriverId) -> Option<&EventEntry> {
        self.entries.iter().find(|e| e.driver_id == driver_id)
    }

    /// The race's start order from the stored qualifying, as grid
    /// references; empty without one.
    pub fn race_grid_refs(&self) -> Vec<String> {
        self.result(EventPhase::Qualifying)
            .map(|r| r.lines.iter().map(ResultLine::grid_ref).collect())
            .unwrap_or_default()
    }
}

/// The first phase of an event on `spec`: practice when the weekend has
/// it, else qualifying when it is a weekend, else the race.
pub fn first_phase(spec: &HostedSpec) -> EventPhase {
    match &spec.weekend {
        Some(w) if w.practice_seconds > 0 => EventPhase::Practice,
        Some(_) => EventPhase::Qualifying,
        None => EventPhase::Race,
    }
}

/// An event's id from its name: lower case ASCII letters and digits, runs
/// of anything else one `-`; "event" for a name with none.
pub fn slug(name: &str) -> String {
    let mut out = String::new();
    for c in name.chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c.to_ascii_lowercase());
        } else if !out.is_empty() && !out.ends_with('-') {
            out.push('-');
        }
    }
    let out = out
        .trim_end_matches('-')
        .chars()
        .take(48)
        .collect::<String>();
    if out.is_empty() {
        "event".into()
    } else {
        out
    }
}

/// The phase a session has just finished, as the event counts phases, with
/// its results; `None` while it runs or between other states.
pub fn finished_phase(gs: &GameSession) -> Option<(EventPhase, Vec<ResultLine>)> {
    if gs.session.state != SessionState::Finished {
        return None;
    }
    let progress = gs.weekend_progress();
    let format = gs.session.weekend.as_ref();
    let phase = match format {
        Some(format) => match progress.phase {
            WeekendPhase::Practice => EventPhase::Practice,
            WeekendPhase::Qualifying
                if progress.round_closed && progress.round as usize >= format.rounds() =>
            {
                EventPhase::Qualifying
            }
            WeekendPhase::Race if gs.session.game_mode == GameMode::Race => EventPhase::Race,
            _ => return None,
        },
        None if gs.session.game_mode == GameMode::Race => EventPhase::Race,
        None => return None,
    };
    let line = |position: usize, id: &PlayerId| -> Option<ResultLine> {
        let state = gs.session.participants.get(id)?;
        Some(ResultLine {
            position: (position + 1).min(u16::MAX as usize) as u16,
            name: gs.driver_name(id),
            driver_id: gs.driver_id(id),
            ai: gs
                .session
                .ai_player_ids
                .iter()
                .position(|a| a == id)
                .map(|i| (i + 1).min(u8::MAX as usize) as u8),
            car_id: state.car_config_id,
            best_lap_ms: state.best_lap_time_ms.filter(|ms| *ms > 0),
            laps: gs.laps_completed(id),
            race_time_ms: None,
            status: None,
            knocked_out_in: None,
        })
    };
    let lines = match phase {
        EventPhase::Practice => {
            let mut cars: Vec<&CarState> = gs.session.participants.values().collect();
            cars.sort_by_key(|s| {
                (
                    s.best_lap_time_ms.filter(|ms| *ms > 0).unwrap_or(u32::MAX),
                    s.grid_position,
                    s.player_id,
                )
            });
            cars.iter()
                .enumerate()
                .filter_map(|(i, s)| line(i, &s.player_id))
                .collect()
        }
        EventPhase::Qualifying => {
            let front = progress.final_order.iter().map(|(id, ms)| (*id, *ms, None));
            let out = progress
                .eliminated
                .iter()
                .map(|e| (e.player_id, e.lap_time_ms, Some(e.round)));
            front
                .chain(out)
                .filter(|(id, _, _)| gs.session.participants.contains_key(id))
                .enumerate()
                .filter_map(|(i, (id, ms, round))| {
                    let mut l = line(i, &id)?;
                    l.best_lap_ms = Some(ms).filter(|ms| *ms > 0);
                    l.knocked_out_in = round;
                    Some(l)
                })
                .collect()
        }
        _ => gs
            .classification(&std::collections::HashMap::new())
            .into_iter()
            .enumerate()
            .filter_map(|(i, c)| {
                let mut l = line(i, &c.player_id)?;
                l.name = c.name;
                l.laps = c.laps;
                l.best_lap_ms = c.best_lap_ms;
                l.race_time_ms = c.race_time_ms;
                l.status = Some(c.status.to_string());
                Some(l)
            })
            .collect(),
    };
    Some((phase, lines))
}

/// What an event says to a driver joining its session.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Admission {
    /// The session is no event's.
    NotAnEvent,
    /// Seat them in this car and livery (their entry's).
    Seat { car_id: CarConfigId, livery: u8 },
    /// Refused, with the reason.
    Refused(&'static str),
}

#[derive(Default)]
struct Inner {
    events: BTreeMap<String, Event>,
    /// Each open event's session in this process.
    sessions: BTreeMap<String, SessionId>,
}

/// Every event, kept one file each in `dir` (none: in memory only).
#[derive(Default)]
pub struct EventStore {
    dir: Option<PathBuf>,
    inner: Mutex<Inner>,
    /// Held while a session is opened for an event, so the runner and the
    /// dashboard never open two for one.
    opening: tokio::sync::Mutex<()>,
}

impl EventStore {
    pub fn in_memory() -> Self {
        Self::default()
    }

    /// The events in `dir` (`<records>/events`); a file that does not read
    /// is logged and left out (and left on disk).
    pub fn open(dir: impl Into<PathBuf>) -> Self {
        let dir = dir.into();
        let mut events = BTreeMap::new();
        if let Ok(read) = std::fs::read_dir(&dir) {
            let mut paths: Vec<PathBuf> = read
                .filter_map(|e| e.ok().map(|e| e.path()))
                .filter(|p| p.extension().is_some_and(|x| x == "json"))
                .collect();
            paths.sort();
            for path in paths {
                match std::fs::read(&path)
                    .map_err(|e| e.to_string())
                    .and_then(|b| serde_json::from_slice::<Event>(&b).map_err(|e| e.to_string()))
                {
                    Ok(event) => {
                        events.insert(event.id.clone(), event);
                    }
                    Err(e) => warn!("Ignoring unreadable event {}: {}", path.display(), e),
                }
            }
        }
        if !events.is_empty() {
            info!("Loaded {} event(s) from {}", events.len(), dir.display());
        }
        Self {
            dir: Some(dir),
            inner: Mutex::new(Inner {
                events,
                sessions: BTreeMap::new(),
            }),
            opening: tokio::sync::Mutex::new(()),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn save(&self, event: &Event) {
        let Some(dir) = &self.dir else {
            return;
        };
        let path = dir.join(format!("{}.json", event.id));
        let written = serde_json::to_vec_pretty(event)
            .map_err(std::io::Error::other)
            .and_then(|bytes| {
                std::fs::create_dir_all(dir)?;
                let tmp = path.with_extension("json.tmp");
                std::fs::write(&tmp, bytes)?;
                std::fs::rename(&tmp, &path)
            });
        if let Err(e) = written {
            warn!("Cannot write the event {}: {}", path.display(), e);
        }
    }

    /// Every event, by id.
    pub fn list(&self) -> Vec<Event> {
        self.lock().events.values().cloned().collect()
    }

    pub fn get(&self, id: &str) -> Option<Event> {
        self.lock().events.get(id).cloned()
    }

    /// A new event; its id is its name's slug, with `-2`, `-3`... after it
    /// while that is taken.
    pub fn create(&self, name: &str, mut spec: HostedSpec, entries_open: bool, now: u64) -> Event {
        let name: String = name.trim().chars().take(MAX_EVENT_NAME).collect();
        if spec.name.trim().is_empty() {
            spec.name = name.clone();
        }
        let mut inner = self.lock();
        let base = slug(&name);
        let id = (1..)
            .map(|n| {
                if n == 1 {
                    base.clone()
                } else {
                    format!("{base}-{n}")
                }
            })
            .find(|id| !inner.events.contains_key(id))
            .unwrap_or(base);
        let event = Event::new(id.clone(), name, spec, entries_open, now);
        inner.events.insert(id, event.clone());
        drop(inner);
        self.save(&event);
        event
    }

    /// Change an event and write it; the changed event.
    pub fn update(&self, id: &str, f: impl FnOnce(&mut Event)) -> Option<Event> {
        let mut inner = self.lock();
        let event = inner.events.get_mut(id)?;
        f(event);
        let event = event.clone();
        drop(inner);
        self.save(&event);
        Some(event)
    }

    /// Forget an event and delete its file.
    pub fn remove(&self, id: &str) -> Option<Event> {
        let mut inner = self.lock();
        inner.sessions.remove(id);
        let gone = inner.events.remove(id)?;
        drop(inner);
        if let Some(dir) = &self.dir {
            let _ = std::fs::remove_file(dir.join(format!("{id}.json")));
        }
        Some(gone)
    }

    /// The session open for an event, in this process.
    pub fn session_of(&self, id: &str) -> Option<SessionId> {
        self.lock().sessions.get(id).copied()
    }

    /// The event a session is open for.
    pub fn event_of_session(&self, session: SessionId) -> Option<String> {
        self.lock()
            .sessions
            .iter()
            .find(|(_, s)| **s == session)
            .map(|(id, _)| id.clone())
    }

    fn bind_session(&self, id: &str, session: Option<SessionId>) {
        let mut inner = self.lock();
        match session {
            Some(s) => inner.sessions.insert(id.to_string(), s),
            None => inner.sessions.remove(id),
        };
    }

    /// Whether a driver may drive in `session` and in what. An event takes
    /// drivers only by a proved driver id (`proved`: the connection proved
    /// its key, `driver_keys`): an entered one drives their entry's car and
    /// livery; one not entered is entered with `car` and `livery` while
    /// entries are open and seats are left.
    pub fn admit(
        &self,
        session: SessionId,
        driver_id: Option<DriverId>,
        proved: bool,
        name: &str,
        car: Option<CarConfigId>,
        livery: u8,
    ) -> Admission {
        let Some(id) = self.event_of_session(session) else {
            return Admission::NotAnEvent;
        };
        let Some(driver_id) = driver_id.filter(|_| proved) else {
            return Admission::Refused("This event takes drivers by their proved driver id");
        };
        let mut inner = self.lock();
        let Some(event) = inner.events.get_mut(&id) else {
            return Admission::NotAnEvent;
        };
        let admission =
            if let Some(entry) = event.entries.iter_mut().find(|e| e.driver_id == driver_id) {
                entry.name = name.chars().take(40).collect();
                Admission::Seat {
                    car_id: entry.car_id,
                    livery: entry.livery,
                }
            } else if !event.entries_open {
                return Admission::Refused("Not entered in this event");
            } else if event.entries.len() >= event.spec.player_slots.max(1) as usize {
                return Admission::Refused("This event's entry list is full");
            } else if let Some(car_id) = car {
                event.entries.push(EventEntry {
                    driver_id,
                    name: name.chars().take(40).collect(),
                    car_id,
                    livery,
                });
                info!("{} entered event {} ({})", name, event.id, driver_id);
                Admission::Seat { car_id, livery }
            } else {
                return Admission::Refused("Must select a car before joining session");
            };
        let event = event.clone();
        drop(inner);
        self.save(&event);
        admission
    }

    /// Record the phase `gs` (the session open for an event) has just
    /// finished, when it is the one the event is at: its results kept, the
    /// event on to its next phase. True when something was recorded.
    pub fn note_finished(&self, session: SessionId, gs: &GameSession, now: u64) -> bool {
        let Some(id) = self.event_of_session(session) else {
            return false;
        };
        let Some((phase, lines)) = finished_phase(gs) else {
            return false;
        };
        if self.lock().events.get(&id).is_none_or(|e| e.phase != phase) {
            return false;
        }
        let mut recorded = false;
        self.update(&id, |event| {
            if event.phase != phase {
                return;
            }
            event.results.retain(|r| r.phase != phase);
            event.results.push(PhaseResult {
                phase,
                finished_at: now,
                lines,
            });
            event.phase = event.after(phase);
            if event.phase == EventPhase::Done {
                event.opened = false;
            }
            recorded = true;
            info!("Event {} finished its {:?}", event.id, phase);
        });
        recorded
    }
}

/// Open the session for an event, at its phase: a fresh hosted session on
/// the event's spec, the phases before skipped and the race gridded from
/// the stored qualifying.
pub(crate) async fn open_session(
    ctx: &GameLoopCtx,
    hosted: &HostedSessions,
    store: &EventStore,
    id: &str,
) -> Result<SessionId, String> {
    let _opening = store.opening.lock().await;
    let Some(event) = store.get(id) else {
        return Err("no such event".into());
    };
    if event.phase == EventPhase::Done {
        return Err("the event is over".into());
    }
    if let Some(s) = store.session_of(id) {
        if ctx.state.read().await.sessions.contains_key(&s) {
            return Ok(s);
        }
    }
    let session = hosted::open(ctx, hosted, event.spec.clone()).await?;
    // Bound at once: a join is admitted by the event from here on.
    {
        let mut st = ctx.state.write().await;
        if let Some(gs) = st.sessions.get_mut(&session) {
            match event.phase {
                EventPhase::Qualifying => gs.resume_weekend(WeekendPhase::Qualifying),
                EventPhase::Race => {
                    gs.resume_weekend(WeekendPhase::Race);
                    let refs = event.race_grid_refs();
                    if !refs.is_empty() {
                        gs.set_grid_order(refs);
                    }
                }
                _ => {}
            }
        }
    }
    store.bind_session(id, Some(session));
    info!(
        "Event {} open in session {} at its {:?}",
        id, session, event.phase
    );
    Ok(session)
}

/// One pass: record what the events' sessions have finished, forget the
/// sessions that have gone, and open a session for every open event that
/// has none (after a restart, every one).
pub(crate) async fn run_once(
    ctx: &GameLoopCtx,
    hosted: &HostedSessions,
    store: &EventStore,
    now: u64,
) {
    for event in store.list() {
        if let Some(session) = store.session_of(&event.id) {
            let st = ctx.state.read().await;
            match st.sessions.get(&session) {
                Some(gs) => {
                    store.note_finished(session, gs, now);
                    continue;
                }
                None => {
                    drop(st);
                    store.bind_session(&event.id, None);
                }
            }
        }
        let event = store.get(&event.id).unwrap_or(event);
        if !event.opened || event.phase == EventPhase::Done {
            continue;
        }
        let error = open_session(ctx, hosted, store, &event.id).await.err();
        if error.is_some() || event.last_error.is_some() {
            if let Some(why) = &error {
                warn!("Event {} could not open its session: {}", event.id, why);
            }
            if error != event.last_error {
                store.update(&event.id, |e| e.last_error = error);
            }
        }
    }
}

/// Run [`run_once`] every second while the server is healthy.
pub(crate) fn spawn(
    store: Arc<EventStore>,
    ctx: Arc<GameLoopCtx>,
    hosted: Arc<HostedSessions>,
    health: crate::health::HealthState,
) {
    tokio::spawn(async move {
        let mut tick = tokio::time::interval(Duration::from_secs(1));
        loop {
            tick.tick().await;
            if !*health.is_healthy.read().await {
                return;
            }
            run_once(&ctx, &hosted, &store, crate::admin::schedule::unix_now()).await;
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec(weekend: Option<crate::weekend::WeekendFormat>) -> HostedSpec {
        serde_json::from_value(serde_json::json!({
            "track_id": uuid::Uuid::from_u128(1),
            "car_id": uuid::Uuid::from_u128(2),
            "player_slots": 2,
        }))
        .map(|mut s: HostedSpec| {
            s.weekend = weekend;
            s
        })
        .unwrap()
    }

    #[test]
    fn ids_are_the_names_slugs_and_unique() {
        assert_eq!(slug("Sunday Cup: Round 3!"), "sunday-cup-round-3");
        assert_eq!(slug("  ***  "), "event");
        let store = EventStore::in_memory();
        assert_eq!(
            store.create("Sunday Cup", spec(None), true, 0).id,
            "sunday-cup"
        );
        assert_eq!(
            store.create("Sunday cup", spec(None), true, 0).id,
            "sunday-cup-2"
        );
    }

    #[test]
    fn the_first_phase_follows_the_format() {
        let weekend = |practice| {
            Some(crate::weekend::WeekendFormat {
                practice_seconds: practice,
                qualifying_seconds: vec![600],
                ..Default::default()
            })
        };
        assert_eq!(first_phase(&spec(weekend(600))), EventPhase::Practice);
        assert_eq!(first_phase(&spec(weekend(0))), EventPhase::Qualifying);
        assert_eq!(first_phase(&spec(None)), EventPhase::Race);
    }

    #[test]
    fn an_event_round_trips_its_file_and_entries_follow_the_rules() {
        let dir = tempfile::tempdir().unwrap();
        let store = EventStore::open(dir.path());
        let event = store.create("Cup", spec(None), true, 7);
        let session = uuid::Uuid::from_u128(9);
        store.bind_session(&event.id, Some(session));
        let car = uuid::Uuid::from_u128(2);
        let (a, b, c) = (
            uuid::Uuid::from_u128(11),
            uuid::Uuid::from_u128(12),
            uuid::Uuid::from_u128(13),
        );
        assert_eq!(
            store.admit(session, Some(a), false, "A", Some(car), 0),
            Admission::Refused("This event takes drivers by their proved driver id")
        );
        assert_eq!(
            store.admit(session, Some(a), true, "A", Some(car), 3),
            Admission::Seat {
                car_id: car,
                livery: 3
            }
        );
        // Entered: the entry's car and livery, whatever they picked now.
        let other = uuid::Uuid::from_u128(5);
        assert_eq!(
            store.admit(session, Some(a), true, "A2", Some(other), 1),
            Admission::Seat {
                car_id: car,
                livery: 3
            }
        );
        assert!(matches!(
            store.admit(session, Some(b), true, "B", Some(car), 0),
            Admission::Seat { .. }
        ));
        assert_eq!(
            store.admit(session, Some(c), true, "C", Some(car), 0),
            Admission::Refused("This event's entry list is full")
        );
        assert_eq!(
            store.admit(uuid::Uuid::from_u128(99), None, false, "X", None, 0),
            Admission::NotAnEvent
        );

        let again = EventStore::open(dir.path());
        let loaded = again.get("cup").unwrap();
        assert_eq!(loaded.entries.len(), 2);
        assert_eq!(loaded.entries[0].name, "A2");
        assert_eq!(loaded.created_at, 7);
        assert_eq!(again.session_of("cup"), None, "sessions are this process's");
    }

    #[test]
    fn the_race_grids_by_the_stored_qualifying() {
        let mut event = Event::new("e".into(), "E".into(), spec(None), true, 0);
        let line = |position, driver_id, ai| ResultLine {
            position,
            name: "N".into(),
            driver_id,
            ai,
            car_id: uuid::Uuid::from_u128(2),
            best_lap_ms: None,
            laps: 0,
            race_time_ms: None,
            status: None,
            knocked_out_in: None,
        };
        let a = uuid::Uuid::from_u128(11);
        event.results.push(PhaseResult {
            phase: EventPhase::Qualifying,
            finished_at: 0,
            lines: vec![
                line(1, None, Some(2)),
                line(2, Some(a), None),
                line(3, None, None),
            ],
        });
        assert_eq!(
            event.race_grid_refs(),
            vec!["@ai:2".to_string(), format!("@driver:{a}"), "N".into()]
        );
    }
}

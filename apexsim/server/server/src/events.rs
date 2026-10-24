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
/// The weekend's sheets beside an event's file: `<id>.sheets.json` and
/// `<id>.sheets.csv`.
pub const SHEETS_STEM: &str = "sheets";

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

impl EventPhase {
    /// The phase as `SessionJoined.Event.Phase` numbers it (as
    /// `WeekendState.Phase` does, the event's end after the race).
    pub fn wire(self) -> u8 {
        match self {
            EventPhase::Practice => 1,
            EventPhase::Qualifying => 2,
            EventPhase::Race => 3,
            EventPhase::Done => 4,
        }
    }
}

/// When an event's phases start (`Event::schedule`): wall-clock times,
/// Unix s, read by the event runner (never by the simulation). A phase with
/// a time opens its session `open_minutes` before it (the runner opens the
/// event), starts at it whoever is there, and the phase before it waits for
/// it; a phase without one is started from the dashboard, as an event
/// without a schedule is.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EventSchedule {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub practice_at: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub qualifying_at: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub race_at: Option<u64>,
    /// How long before a phase's time its session opens, minutes.
    #[serde(default = "default_open_minutes")]
    pub open_minutes: u32,
    /// The pause between two knockout rounds, s.
    #[serde(default = "default_round_break_s")]
    pub round_break_s: u32,
}

fn default_open_minutes() -> u32 {
    15
}

fn default_round_break_s() -> u32 {
    120
}

impl Default for EventSchedule {
    fn default() -> Self {
        Self {
            practice_at: None,
            qualifying_at: None,
            race_at: None,
            open_minutes: default_open_minutes(),
            round_break_s: default_round_break_s(),
        }
    }
}

impl EventSchedule {
    /// No phase has a time.
    pub fn is_empty(&self) -> bool {
        self.practice_at.is_none() && self.qualifying_at.is_none() && self.race_at.is_none()
    }

    /// When `phase` starts, if it has a time.
    pub fn at(&self, phase: EventPhase) -> Option<u64> {
        match phase {
            EventPhase::Practice => self.practice_at,
            EventPhase::Qualifying => self.qualifying_at,
            EventPhase::Race => self.race_at,
            EventPhase::Done => None,
        }
    }

    /// When `phase`'s session opens, if it has a time.
    pub fn opens_at(&self, phase: EventPhase) -> Option<u64> {
        self.at(phase)
            .map(|at| at.saturating_sub(u64::from(self.open_minutes) * 60))
    }

    /// The times in phase order, never earlier than the one before, and an
    /// opening of at most a day: what the dashboard's edit keeps.
    pub fn clamp(mut self) -> Self {
        let mut last = 0u64;
        for at in [
            &mut self.practice_at,
            &mut self.qualifying_at,
            &mut self.race_at,
        ]
        .into_iter()
        .flatten()
        {
            *at = (*at).max(last);
            last = *at;
        }
        self.open_minutes = self.open_minutes.min(24 * 60);
        self.round_break_s = self.round_break_s.clamp(10, 3600);
        self
    }
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

/// An AI driver entered in an event: the same id, name, level and car in
/// every session of it ([`EventStore::ai_field`]).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EventAi {
    /// From the event's id and the driver's place ([`ai_id`]): the session
    /// seats the field in the order of these.
    pub id: PlayerId,
    pub name: String,
    /// `AiDriverProfile::skill_level`; the rest of the profile follows it.
    pub skill: u8,
    pub car_id: CarConfigId,
    /// A multi-class field's class; empty for one class.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub class: String,
}

impl EventAi {
    fn profile(&self) -> crate::ai_driver::AiDriverProfile {
        let mut p = crate::ai_driver::AiDriverProfile::new(self.name.clone(), self.skill);
        p.id = self.id;
        p.preferred_car_id = Some(self.car_id);
        p.class = self.class.clone();
        p
    }
}

/// The `n`th AI entry's id in event `event`: an FNV-1a hash of both, so the
/// same event makes the same ids on every server.
pub fn ai_id(event: &str, n: usize) -> PlayerId {
    let hash = |seed: u64| {
        let mut h = seed;
        for b in event.bytes().chain(n.to_le_bytes()) {
            h ^= u64::from(b);
            h = h.wrapping_mul(0x0000_0100_0000_01b3);
        }
        h
    };
    PlayerId::from_u64_pair(hash(0xcbf2_9ce4_8422_2325), hash(0x8422_2325_cbf2_9ce4))
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
    /// An AI entry's id (`EventAi::id`), the same in every session.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ai_id: Option<PlayerId>,
    pub car_id: CarConfigId,
    /// The car's name as the server knows it (the sheets' export).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub car_name: String,
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
    /// The session it was run in: its recordings are this phase's replays
    /// ([`phase_replays`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session: Option<SessionId>,
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
    /// The AI field, fixed when its first session opens.
    #[serde(default)]
    pub ai: Vec<EventAi>,
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
    /// When its phases start; empty, the dashboard starts each.
    #[serde(default, skip_serializing_if = "EventSchedule::is_empty")]
    pub schedule: EventSchedule,
    /// Closed on the dashboard: its schedule does not open it again until
    /// it is opened by hand.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub held: bool,
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
            ai: Vec::new(),
            phase,
            opened: false,
            results: Vec::new(),
            created_at: now,
            last_error: None,
            schedule: EventSchedule::default(),
            held: false,
        }
    }

    /// When the phase it is at starts, by its schedule; `None` for a phase
    /// with no time, and once it is done.
    pub fn next_start(&self) -> Option<u64> {
        self.schedule.at(self.phase)
    }

    /// Whether its schedule has it opened at `now`: its phase has a time,
    /// that phase's session opens by now, and it was not closed by hand.
    pub fn due_to_open(&self, now: u64) -> bool {
        !self.held
            && self.phase != EventPhase::Done
            && self
                .schedule
                .opens_at(self.phase)
                .is_some_and(|at| now >= at)
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
    /// references; empty without one. An AI entry is named by its place in
    /// the field the session seats (`@ai:N`: the entries in the order of
    /// their ids).
    pub fn race_grid_refs(&self) -> Vec<String> {
        let mut seated: Vec<PlayerId> = self.ai.iter().map(|a| a.id).collect();
        seated.sort();
        let reference =
            |l: &ResultLine| match l.ai_id.and_then(|id| seated.iter().position(|s| *s == id)) {
                Some(n) => crate::grid_order::ai_ref(n + 1),
                None => l.grid_ref(),
            };
        self.result(EventPhase::Qualifying)
            .map(|r| r.lines.iter().map(reference).collect())
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
            ai_id: gs.is_ai_player(id).then_some(*id),
            car_id: state.car_config_id,
            car_name: gs
                .car_configs
                .get(&state.car_config_id)
                .map(|c| c.name.clone())
                .unwrap_or_default(),
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

impl EventPhase {
    /// Its name in the sheets: practice, qualifying, race.
    pub fn name(self) -> &'static str {
        match self {
            EventPhase::Practice => "practice",
            EventPhase::Qualifying => "qualifying",
            EventPhase::Race => "race",
            EventPhase::Done => "done",
        }
    }
}

/// The weekend's sheets as one record: practice times, the combined
/// qualifying sheet and the race classification, each phase as it was
/// kept (`<id>.sheets.json`).
pub fn sheets_json(event: &Event) -> serde_json::Value {
    serde_json::json!({
        "id": event.id,
        "name": event.name,
        "track_id": event.spec.track_id,
        "phase": event.phase,
        "phases": event.results,
    })
}

/// The sheets as CSV, a line per car per phase (`<id>.sheets.csv`).
pub fn sheets_csv(event: &Event) -> String {
    let mut out = String::from(
        "phase,position,name,driver_id,ai_id,car,laps,best_lap_ms,race_time_ms,status,knocked_out_in\n",
    );
    let quoted = |s: &str| format!("\"{}\"", s.replace('"', "\"\""));
    let opt = |v: Option<u32>| v.map_or(String::new(), |v| v.to_string());
    for result in &event.results {
        for l in &result.lines {
            out.push_str(&format!(
                "{},{},{},{},{},{},{},{},{},{},{}\n",
                result.phase.name(),
                l.position,
                quoted(&l.name),
                l.driver_id.map(|d| d.to_string()).unwrap_or_default(),
                l.ai_id.map(|d| d.to_string()).unwrap_or_default(),
                quoted(&l.car_name),
                l.laps,
                opt(l.best_lap_ms),
                opt(l.race_time_ms),
                l.status.as_deref().unwrap_or(""),
                l.knocked_out_in.map(|r| r.to_string()).unwrap_or_default(),
            ));
        }
    }
    out
}

/// Each kept phase's replays, in `event.results` order: the recordings
/// (`replay_<session>_<unix>.apxs`, `stream_recorder`) of the session it
/// was run in, written after the phase before it in that session was kept
/// and no later than a minute after it was. A qualifying phase has one a
/// round. Names only, newest last; empty without a session or a recording.
pub fn phase_replays(event: &Event, replay_dir: &std::path::Path) -> Vec<Vec<String>> {
    let files: Vec<(SessionId, u64, String)> = std::fs::read_dir(replay_dir)
        .map(|read| {
            read.filter_map(|e| e.ok())
                .filter_map(|e| {
                    let name = e.file_name().to_string_lossy().into_owned();
                    let stem = name.strip_prefix("replay_")?.strip_suffix(".apxs")?;
                    let (session, unix) = stem.rsplit_once('_')?;
                    Some((session.parse().ok()?, unix.parse().ok()?, name.clone()))
                })
                .collect()
        })
        .unwrap_or_default();
    let mut out = Vec::with_capacity(event.results.len());
    for (i, result) in event.results.iter().enumerate() {
        let Some(session) = result.session else {
            out.push(Vec::new());
            continue;
        };
        // The phase before it in the same session ends its window.
        let after = event.results[..i]
            .iter()
            .filter(|r| r.session == Some(session))
            .map(|r| r.finished_at + 2)
            .max()
            .unwrap_or(0);
        let mut mine: Vec<(u64, String)> = files
            .iter()
            .filter(|(s, t, _)| *s == session && *t > after && *t <= result.finished_at + 60)
            .map(|(_, t, n)| (*t, n.clone()))
            .collect();
        mine.sort();
        out.push(mine.into_iter().map(|(_, n)| n).collect());
    }
    out
}

/// An event's sheets as `EventSheets` sends them (`RequestEventSheets`),
/// with each phase's replays from `replay_dir`.
pub fn sheets_data(
    event: &Event,
    replay_dir: Option<&std::path::Path>,
) -> crate::network::EventSheetsData {
    use crate::network::{EventSheetData, EventSheetLine, EventSheetsData};
    let replays = replay_dir
        .map(|dir| phase_replays(event, dir))
        .unwrap_or_default();
    EventSheetsData {
        id: event.id.clone(),
        name: event.name.clone(),
        track_id: event.spec.track_id,
        phase: event.phase.wire(),
        phases: event
            .results
            .iter()
            .enumerate()
            .map(|(i, r)| EventSheetData {
                phase: r.phase.wire(),
                finished_at: r.finished_at,
                lines: r
                    .lines
                    .iter()
                    .map(|l| EventSheetLine {
                        position: l.position,
                        name: l.name.clone(),
                        car_id: l.car_id,
                        ai: l.driver_id.is_none(),
                        laps: l.laps,
                        best_lap_ms: l.best_lap_ms.unwrap_or(0),
                        race_time_ms: l.race_time_ms.unwrap_or(0),
                        status: l.status.clone().unwrap_or_default(),
                        knocked_out_in: l.knocked_out_in.unwrap_or(0),
                    })
                    .collect(),
                replays: replays.get(i).cloned().unwrap_or_default(),
            })
            .collect(),
    }
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
    /// When a scheduled event's session was first seen between two
    /// knockout rounds (the next one starts `round_break_s` later).
    round_breaks: BTreeMap<String, u64>,
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
                // `<id>.json` only: an event's other files share its stem.
                .filter(|p| {
                    p.file_stem()
                        .is_some_and(|s| !s.to_string_lossy().contains('.'))
                })
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
                ..Default::default()
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
        if event.results.is_empty() {
            return;
        }
        // The weekend's sheets beside it, as the race results are kept.
        let sheets = serde_json::to_vec_pretty(&sheets_json(event))
            .map_err(std::io::Error::other)
            .and_then(|json| {
                std::fs::write(dir.join(format!("{}.{SHEETS_STEM}.json", event.id)), json)?;
                std::fs::write(
                    dir.join(format!("{}.{SHEETS_STEM}.csv", event.id)),
                    sheets_csv(event),
                )
            });
        if let Err(e) = sheets {
            warn!("Cannot write the sheets of event {}: {}", event.id, e);
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
            for ext in ["json", "csv"] {
                let _ = std::fs::remove_file(dir.join(format!("{id}.{SHEETS_STEM}.{ext}")));
            }
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

    /// The event a session is open for, as `SessionJoined.Event` names it.
    pub fn joined_event(&self, session: SessionId) -> Option<crate::network::SessionEventData> {
        let id = self.event_of_session(session)?;
        let event = self.get(&id)?;
        Some(crate::network::SessionEventData {
            id: event.id,
            name: event.name,
            phase: event.phase.wire(),
        })
    }

    /// Every event as the lobby lists it (`LobbyState.Events`), by id.
    pub fn lobby_events(&self) -> Vec<crate::network::LobbyEventData> {
        let inner = self.lock();
        inner
            .events
            .values()
            .map(|e| crate::network::LobbyEventData {
                id: e.id.clone(),
                name: e.name.clone(),
                phase: e.phase.wire(),
                track_id: e.spec.track_id,
                starts_at: e.next_start().unwrap_or(0),
                session_id: inner
                    .sessions
                    .get(&e.id)
                    .map(|s| s.to_string())
                    .unwrap_or_default(),
                entries: e.entries.len().min(u8::MAX as usize) as u8,
                slots: e.spec.player_slots,
            })
            .collect()
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

    /// The event's AI field: as stored, or, the first time, the field the
    /// session `gs` was dealt from the spec (names, levels, cars, classes in
    /// seating order) under the event's own ids, stored.
    pub fn ai_field(&self, id: &str, gs: &GameSession) -> Vec<EventAi> {
        let Some(event) = self.get(id) else {
            return Vec::new();
        };
        if !event.ai.is_empty() || gs.session.ai_player_ids.is_empty() {
            return event.ai;
        }
        let field: Vec<EventAi> = gs
            .session
            .ai_player_ids
            .iter()
            .enumerate()
            .filter_map(|(n, p)| {
                let profile = gs.get_ai_profile(p)?;
                Some(EventAi {
                    id: ai_id(id, n),
                    name: profile.name.clone(),
                    skill: profile.skill_level,
                    car_id: gs.session.participants.get(p)?.car_config_id,
                    class: profile.class.clone(),
                })
            })
            .collect();
        self.update(id, |e| e.ai = field.clone());
        info!("Event {} fixed its AI field ({} drivers)", id, field.len());
        field
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
                session: Some(session),
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
    store.bind_session(id, Some(session));
    {
        let mut st = ctx.state.write().await;
        if let Some(gs) = st.sessions.get_mut(&session) {
            let field = store.ai_field(id, gs);
            gs.replace_ai_field(field.iter().map(EventAi::profile).collect());
            gs.on_schedule = !event.schedule.is_empty();
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
    info!(
        "Event {} open in session {} at its {:?}",
        id, session, event.phase
    );
    Ok(session)
}

/// One pass: record what the events' sessions have finished, run their
/// schedules, forget the sessions that have gone, and open a session for
/// every open event that has none (after a restart, every one; a scheduled
/// event once its phase's session is due to open).
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
                    drop(st);
                    run_schedule(ctx, store, &event.id, session, now).await;
                    continue;
                }
                None => {
                    drop(st);
                    store.bind_session(&event.id, None);
                }
            }
        }
        let mut event = store.get(&event.id).unwrap_or(event);
        if !event.opened && event.due_to_open(now) {
            info!("Event {} opens on its schedule", event.id);
            event = store
                .update(&event.id, |e| {
                    e.opened = true;
                    e.last_error = None;
                })
                .unwrap_or(event);
        }
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

/// What a scheduled event's session does next at `now`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScheduleStep {
    /// Nothing yet.
    Wait,
    /// Start the session in its lobby (its weekend's next phase, or the
    /// race's countdown).
    Start,
    /// Move the weekend on to its next phase or knockout round.
    Advance,
}

/// The schedule's call for an event's session: `in_lobby` (the session has
/// not started), `between_phases` (a weekend waiting to be moved on) and
/// `between_rounds` (that wait is between two knockout rounds, first seen
/// at `break_since`).
pub fn schedule_step(
    event: &Event,
    in_lobby: bool,
    between_phases: bool,
    between_rounds: bool,
    break_since: u64,
    now: u64,
) -> ScheduleStep {
    if event.schedule.is_empty() {
        return ScheduleStep::Wait;
    }
    if between_phases && between_rounds {
        // The knockout runs on after a short break once its first round
        // has started on time.
        return if now >= break_since + u64::from(event.schedule.round_break_s) {
            ScheduleStep::Advance
        } else {
            ScheduleStep::Wait
        };
    }
    let Some(at) = event.next_start() else {
        return ScheduleStep::Wait;
    };
    match (in_lobby, between_phases) {
        _ if now < at => ScheduleStep::Wait,
        (true, _) => ScheduleStep::Start,
        (false, true) => ScheduleStep::Advance,
        _ => ScheduleStep::Wait,
    }
}

/// Run a scheduled event's session on its times: started at its phase's
/// time from the lobby, moved on to a later phase at that phase's time,
/// and from one knockout round to the next after the round break.
async fn run_schedule(
    ctx: &GameLoopCtx,
    store: &EventStore,
    id: &str,
    session: SessionId,
    now: u64,
) {
    let Some(event) = store.get(id) else {
        return;
    };
    if event.schedule.is_empty() {
        return;
    }
    let (in_lobby, between_phases, between_rounds) = {
        let mut st = ctx.state.write().await;
        let Some(gs) = st.sessions.get_mut(&session) else {
            return;
        };
        // A schedule set after the session opened.
        gs.on_schedule = true;
        let progress = gs.weekend_progress();
        let rounds = gs.session.weekend.as_ref().map_or(1, |w| w.rounds());
        (
            gs.session.state == SessionState::Lobby,
            gs.awaits_weekend_advance(),
            progress.phase == WeekendPhase::Qualifying && (progress.round as usize) < rounds,
        )
    };
    let break_since = {
        let mut inner = store.lock();
        if between_phases && between_rounds {
            *inner.round_breaks.entry(id.to_string()).or_insert(now)
        } else {
            inner.round_breaks.remove(id);
            now
        }
    };
    let step = schedule_step(
        &event,
        in_lobby,
        between_phases,
        between_rounds,
        break_since,
        now,
    );
    let done = match step {
        ScheduleStep::Wait => return,
        ScheduleStep::Start => hosted::start(ctx, session).await,
        ScheduleStep::Advance => hosted::advance(ctx, session).await,
    };
    match done {
        Ok(phase) => {
            store.lock().round_breaks.remove(id);
            info!(
                "Event {} on its schedule: {:?} into its {}",
                id, step, phase
            );
        }
        Err(why) => warn!("Event {} could not keep its schedule: {}", id, why),
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
    fn a_schedule_starts_each_phase_at_its_time_and_runs_the_knockout_on() {
        let mut event = Event::new("e".into(), "E".into(), spec(None), true, 0);
        assert_eq!(
            schedule_step(&event, true, false, false, 0, 1_000_000),
            ScheduleStep::Wait,
            "no schedule: the dashboard starts it"
        );
        assert!(!event.due_to_open(u64::MAX));
        event.schedule = EventSchedule {
            race_at: Some(10_000),
            open_minutes: 10,
            ..Default::default()
        };
        assert!(!event.due_to_open(9_399));
        assert!(event.due_to_open(9_400), "open ten minutes before");
        event.held = true;
        assert!(!event.due_to_open(9_400), "closed by hand: held");
        assert_eq!(
            schedule_step(&event, true, false, false, 0, 9_999),
            ScheduleStep::Wait
        );
        assert_eq!(
            schedule_step(&event, true, false, false, 0, 10_000),
            ScheduleStep::Start
        );
        assert_eq!(
            schedule_step(&event, false, false, false, 0, 10_500),
            ScheduleStep::Wait,
            "under way"
        );
        // Between practice and qualifying: the event is at qualifying.
        event.phase = EventPhase::Qualifying;
        event.schedule.qualifying_at = Some(20_000);
        assert_eq!(
            schedule_step(&event, false, true, false, 0, 19_000),
            ScheduleStep::Wait
        );
        assert_eq!(
            schedule_step(&event, false, true, false, 0, 20_000),
            ScheduleStep::Advance
        );
        // Between two knockout rounds: after the break.
        let brk = u64::from(event.schedule.round_break_s);
        assert_eq!(
            schedule_step(&event, false, true, true, 21_000, 21_000 + brk - 1),
            ScheduleStep::Wait
        );
        assert_eq!(
            schedule_step(&event, false, true, true, 21_000, 21_000 + brk),
            ScheduleStep::Advance
        );
    }

    #[test]
    fn a_schedule_keeps_its_phases_in_order() {
        let s = EventSchedule {
            practice_at: Some(500),
            qualifying_at: Some(100),
            race_at: Some(400),
            open_minutes: 100_000,
            round_break_s: 0,
        }
        .clamp();
        assert_eq!(
            (s.practice_at, s.qualifying_at, s.race_at),
            (Some(500), Some(500), Some(500))
        );
        assert_eq!((s.open_minutes, s.round_break_s), (24 * 60, 10));
    }

    #[test]
    fn each_phase_finds_the_replays_of_its_own_session_and_window() {
        let dir = tempfile::tempdir().unwrap();
        let (one, two) = (uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2));
        for name in [
            format!("replay_{one}_1000.apxs"),
            format!("replay_{one}_1700.apxs"),
            format!("replay_{one}_1900.apxs"),
            format!("replay_{one}_1000.meta.json"),
            format!("replay_{two}_5000.apxs"),
            "notes.txt".to_string(),
        ] {
            std::fs::write(dir.path().join(name), b"x").unwrap();
        }
        let mut event = Event::new("e".into(), "E".into(), spec(None), true, 0);
        let kept = |phase, finished_at, session| PhaseResult {
            phase,
            finished_at,
            lines: Vec::new(),
            session,
        };
        event.results = vec![
            kept(EventPhase::Practice, 1001, Some(one)),
            kept(EventPhase::Qualifying, 1900, Some(one)),
            kept(EventPhase::Race, 5000, Some(two)),
        ];
        let replays = phase_replays(&event, dir.path());
        assert_eq!(replays[0], vec![format!("replay_{one}_1000.apxs")]);
        assert_eq!(
            replays[1],
            vec![
                format!("replay_{one}_1700.apxs"),
                format!("replay_{one}_1900.apxs")
            ],
            "a replay a round"
        );
        assert_eq!(replays[2], vec![format!("replay_{two}_5000.apxs")]);
        event.results[2].session = None;
        assert!(phase_replays(&event, dir.path())[2].is_empty());
    }

    #[test]
    fn the_sheets_are_one_record_and_a_csv() {
        let mut event = Event::new("cup".into(), "Cup".into(), spec(None), true, 0);
        event.results.push(PhaseResult {
            phase: EventPhase::Race,
            finished_at: 9,
            lines: vec![ResultLine {
                position: 1,
                name: "Ada \"Quick\"".into(),
                driver_id: Some(uuid::Uuid::from_u128(11)),
                ai: None,
                ai_id: None,
                car_id: uuid::Uuid::from_u128(2),
                car_name: "GT".into(),
                best_lap_ms: Some(80_000),
                laps: 3,
                race_time_ms: Some(250_000),
                status: Some("finished".into()),
                knocked_out_in: None,
            }],
            session: None,
        });
        let csv = sheets_csv(&event);
        let line = csv.lines().nth(1).unwrap();
        assert_eq!(
            line,
            format!(
                "race,1,\"Ada \"\"Quick\"\"\",{},,\"GT\",3,80000,250000,finished,",
                uuid::Uuid::from_u128(11)
            )
        );
        let json = sheets_json(&event);
        assert_eq!(json["phases"][0]["lines"][0]["car_name"], "GT");
        let wire = sheets_data(&event, None);
        assert_eq!(wire.phases[0].lines[0].race_time_ms, 250_000);
        assert!(!wire.phases[0].lines[0].ai);
    }

    #[test]
    fn the_race_grids_by_the_stored_qualifying() {
        let mut event = Event::new("e".into(), "E".into(), spec(None), true, 0);
        let line = |position, driver_id, ai| ResultLine {
            position,
            name: "N".into(),
            driver_id,
            ai,
            ai_id: None,
            car_id: uuid::Uuid::from_u128(2),
            car_name: String::new(),
            best_lap_ms: None,
            laps: 0,
            race_time_ms: None,
            status: None,
            knocked_out_in: None,
        };
        let a = uuid::Uuid::from_u128(11);
        event.results.push(PhaseResult {
            phase: EventPhase::Qualifying,
            session: None,
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

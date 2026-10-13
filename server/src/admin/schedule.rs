//! The dashboard's schedule: hosted sessions (`super::hosted`) opened in
//! the lobby ahead of a set time and started at it, once, daily or weekly.
//!
//! Each entry opens its session `open_minutes` before `start_at`, so players
//! can join and set up, and starts it at `start_at` (a weekend's first
//! phase, else the race's countdown). Once started, the entry moves on to
//! its next time (a one-off is done), and while that session runs a weekend
//! is moved on as each phase ends (`auto_advance`). An occurrence the
//! server was not running for, more than [`MISSED_GRACE_S`] late, is
//! skipped and noted.
//!
//! Times are Unix seconds, so a daily entry keeps to UTC across a change
//! of the clocks. Kept in `[admin] schedule_file`; this runs on the wall
//! clock, outside the simulation.

use super::hosted::{self, HostedSessions, HostedSpec};
use crate::data::SessionId;
use crate::game_loop::GameLoopCtx;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tracing::{info, warn};
use uuid::Uuid;

/// How late an occurrence may still open: past this it was missed.
pub const MISSED_GRACE_S: u64 = 10 * 60;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Repeat {
    #[default]
    Once,
    Daily,
    Weekly,
}

impl Repeat {
    fn period_s(self) -> Option<u64> {
        match self {
            Repeat::Once => None,
            Repeat::Daily => Some(86_400),
            Repeat::Weekly => Some(7 * 86_400),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ScheduleEntry {
    #[serde(default = "Uuid::new_v4")]
    pub id: Uuid,
    pub spec: HostedSpec,
    /// When the session starts, Unix s.
    pub start_at: u64,
    /// How long before the start it opens in the lobby, minutes.
    #[serde(default = "default_open_minutes")]
    pub open_minutes: u32,
    #[serde(default)]
    pub repeat: Repeat,
    #[serde(default = "default_true")]
    pub enabled: bool,
    /// Move a weekend on to its next phase as each one ends.
    #[serde(default = "default_true")]
    pub auto_advance: bool,
    /// The session opened for the coming start, while it waits for it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub open_session: Option<SessionId>,
    /// The session the last start began, while it runs.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub running_session: Option<SessionId>,
    /// When the last occurrence started, Unix s.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_started: Option<u64>,
    /// What went wrong with the last occurrence.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_error: Option<String>,
}

fn default_open_minutes() -> u32 {
    10
}

fn default_true() -> bool {
    true
}

impl ScheduleEntry {
    /// When the session opens, Unix s.
    pub fn opens_at(&self) -> u64 {
        self.start_at
            .saturating_sub(u64::from(self.open_minutes) * 60)
    }

    /// The next start after this one, or the entry is done.
    fn move_on(&mut self, now: u64) {
        match self.repeat.period_s() {
            Some(period) => {
                while self.start_at <= now {
                    self.start_at += period;
                }
            }
            None => self.enabled = false,
        }
    }
}

pub fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

pub struct Schedule {
    path: Option<PathBuf>,
    entries: Mutex<Vec<ScheduleEntry>>,
}

impl Schedule {
    /// The schedule kept in `path` (none when it is absent or unreadable,
    /// logged); empty `path` keeps it in memory only.
    pub fn open(path: &str) -> Self {
        let path = (!path.trim().is_empty()).then(|| PathBuf::from(path));
        let entries = path
            .as_ref()
            .and_then(|p| std::fs::read(p).ok().map(|bytes| (p, bytes)))
            .and_then(|(p, bytes)| match serde_json::from_slice(&bytes) {
                Ok(entries) => Some(entries),
                Err(e) => {
                    warn!(
                        "Schedule {} is not readable ({}); starting empty",
                        p.display(),
                        e
                    );
                    None
                }
            })
            .unwrap_or_default();
        Self {
            path,
            entries: Mutex::new(entries),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Vec<ScheduleEntry>> {
        self.entries.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn entries(&self) -> Vec<ScheduleEntry> {
        let mut out = self.lock().clone();
        out.sort_by_key(|e| (!e.enabled, e.start_at));
        out
    }

    fn save(&self, entries: &[ScheduleEntry]) {
        let Some(path) = &self.path else {
            return;
        };
        let written = serde_json::to_vec_pretty(entries)
            .map_err(std::io::Error::other)
            .and_then(|bytes| {
                if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
                    std::fs::create_dir_all(dir)?;
                }
                let tmp = path.with_extension("json.tmp");
                std::fs::write(&tmp, bytes)?;
                std::fs::rename(&tmp, path)
            });
        if let Err(e) = written {
            warn!("Cannot write the schedule {}: {}", path.display(), e);
        }
    }

    /// Add an entry, or replace the one with its id (keeping what the
    /// scheduler knows of its sessions).
    pub fn upsert(&self, mut entry: ScheduleEntry) -> ScheduleEntry {
        let mut entries = self.lock();
        if let Some(old) = entries.iter_mut().find(|e| e.id == entry.id) {
            entry.open_session = old.open_session;
            entry.running_session = old.running_session;
            entry.last_started = old.last_started;
            *old = entry.clone();
        } else {
            entry.open_session = None;
            entry.running_session = None;
            entry.last_started = None;
            entries.push(entry.clone());
        }
        entry.last_error = None;
        self.save(&entries);
        entry
    }

    pub fn remove(&self, id: Uuid) -> Option<ScheduleEntry> {
        let mut entries = self.lock();
        let at = entries.iter().position(|e| e.id == id)?;
        let gone = entries.remove(at);
        self.save(&entries);
        Some(gone)
    }

    fn update(&self, id: Uuid, f: impl FnOnce(&mut ScheduleEntry)) {
        let mut entries = self.lock();
        if let Some(entry) = entries.iter_mut().find(|e| e.id == id) {
            f(entry);
            self.save(&entries);
        }
    }

    /// One pass over the schedule at `now`: open what is due to open,
    /// start what is due to start, move weekends on.
    pub(crate) async fn run_once(&self, ctx: &GameLoopCtx, hosted: &HostedSessions, now: u64) {
        let live = {
            let st = ctx.state.read().await;
            move |id: &SessionId| st.sessions.contains_key(id)
        };
        // Sessions that have gone (finished, or ended on the dashboard).
        let mut gone_open = Vec::new();
        let mut gone_running = Vec::new();
        for e in self.entries() {
            if e.open_session.is_some_and(|s| !live(&s)) {
                gone_open.push(e.id);
            }
            if e.running_session.is_some_and(|s| !live(&s)) {
                gone_running.push(e.id);
            }
        }
        drop(live);
        for id in gone_running {
            self.update(id, |e| e.running_session = None);
        }
        for id in gone_open {
            // Ended before its start: that occurrence is cancelled.
            self.update(id, |e| {
                e.open_session = None;
                e.last_error = Some("ended before it started".into());
                e.move_on(now);
            });
        }

        for e in self.entries() {
            if let Some(running) = e.running_session {
                if e.auto_advance && hosted::awaits_advance(ctx, running).await {
                    if let Err(why) = hosted::advance(ctx, running).await {
                        warn!("Schedule could not move {} on: {}", running, why);
                    }
                }
            }
            if !e.enabled {
                continue;
            }
            if e.open_session.is_none() && now >= e.opens_at() {
                if now > e.start_at + MISSED_GRACE_S {
                    info!(
                        "Scheduled session {} missed its start; skipping it",
                        e.spec.name
                    );
                    self.update(e.id, |e| {
                        e.last_error = Some("missed: the server was not running".into());
                        e.move_on(now);
                    });
                    continue;
                }
                match hosted::open(ctx, hosted, e.spec.clone()).await {
                    Ok(session) => self.update(e.id, |e| {
                        e.open_session = Some(session);
                        e.last_error = None;
                    }),
                    Err(why) => {
                        warn!("Scheduled session {} could not open: {}", e.spec.name, why);
                        self.update(e.id, |e| {
                            e.last_error = Some(why);
                            e.move_on(now);
                        });
                    }
                }
                continue;
            }
            if let Some(session) = e.open_session.filter(|_| now >= e.start_at) {
                let started = hosted::start(ctx, session).await;
                self.update(e.id, |e| {
                    e.open_session = None;
                    match started {
                        Ok(_) => {
                            e.running_session = Some(session);
                            e.last_started = Some(now);
                            e.last_error = None;
                        }
                        Err(why) => e.last_error = Some(why),
                    }
                    e.move_on(now);
                });
            }
        }
    }
}

/// Run the schedule once a second for the life of the server.
pub(crate) fn spawn(schedule: Arc<Schedule>, ctx: Arc<GameLoopCtx>, hosted: Arc<HostedSessions>) {
    tokio::spawn(async move {
        let mut every = tokio::time::interval(Duration::from_secs(1));
        every.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            every.tick().await;
            schedule.run_once(&ctx, &hosted, unix_now()).await;
            let st = ctx.state.read().await;
            hosted.retain(|id| st.sessions.contains_key(id));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(start_at: u64, repeat: Repeat) -> ScheduleEntry {
        ScheduleEntry {
            id: Uuid::from_u128(1),
            spec: HostedSpec {
                name: "Cup".into(),
                track_id: Uuid::nil(),
                car_id: Uuid::nil(),
                player_slots: 4,
                ai_count: 0,
                ai_skill: None,
                lap_limit: 3,
                race_seconds: None,
                weekend: None,
                conditions: Default::default(),
                damage: Default::default(),
                allowed_assists: Default::default(),
            },
            start_at,
            open_minutes: 10,
            repeat,
            enabled: true,
            auto_advance: true,
            open_session: None,
            running_session: None,
            last_started: None,
            last_error: None,
        }
    }

    #[test]
    fn a_repeating_entry_moves_past_now_and_a_one_off_is_done() {
        let mut daily = entry(1_000, Repeat::Daily);
        daily.move_on(1_000 + 3 * 86_400);
        assert_eq!(daily.start_at, 1_000 + 4 * 86_400);
        assert!(daily.enabled);

        let mut once = entry(1_000, Repeat::Once);
        once.move_on(1_000);
        assert!(!once.enabled);
    }

    #[test]
    fn it_opens_ahead_of_its_start() {
        assert_eq!(entry(10_000, Repeat::Once).opens_at(), 10_000 - 600);
    }

    #[test]
    fn the_file_round_trips_and_an_edit_keeps_the_running_session() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("schedule.json");
        let schedule = Schedule::open(&path.to_string_lossy());
        let mut e = entry(5_000, Repeat::Weekly);
        schedule.upsert(e.clone());
        schedule.update(e.id, |e| e.running_session = Some(Uuid::from_u128(9)));

        e.open_minutes = 30;
        schedule.upsert(e.clone());
        let reread = Schedule::open(&path.to_string_lossy()).entries();
        assert_eq!(reread.len(), 1);
        assert_eq!(reread[0].open_minutes, 30);
        assert_eq!(reread[0].running_session, Some(Uuid::from_u128(9)));
        assert!(schedule.remove(e.id).is_some());
        assert!(Schedule::open(&path.to_string_lossy()).entries().is_empty());
    }
}

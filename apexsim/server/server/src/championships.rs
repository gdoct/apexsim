//! League championships online: a series of race events whose races score
//! points (docs/server/sessions.md "Championships").
//!
//! A championship names its rounds (race events, `crate::events`, by id, in
//! order) and a points table. Its standings are worked out from the rounds'
//! kept race results whenever they are asked for: a human by their proved
//! driver id (the same driver across every event and every restart), an AI
//! driver by name (an event's AI ids are its own). Kept one file each in
//! `<records>/championships/<id>.json`; nothing here runs in the
//! simulation.

use crate::data::DriverId;
use crate::events::{Event, EventPhase};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::Mutex;
use tracing::{info, warn};

/// The championship files' folder under `[records] dir`.
pub const CHAMPIONSHIPS_DIR: &str = "championships";
pub const CHAMPIONSHIP_FILE_VERSION: u32 = 1;
/// Points for 1st to 10th when a championship names none.
pub const DEFAULT_POINTS: [u16; 10] = [25, 18, 15, 12, 10, 8, 6, 4, 2, 1];
/// Most rounds a championship takes.
pub const MAX_ROUNDS: usize = 30;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Championship {
    #[serde(default)]
    pub version: u32,
    /// The slug of its name (`events::slug`), the file's stem.
    pub id: String,
    pub name: String,
    /// Its rounds: race events by id, in the order they count.
    #[serde(default)]
    pub events: Vec<String>,
    /// Points by finishing place, 1st first; a place past the table scores
    /// none.
    #[serde(default = "default_points")]
    pub points: Vec<u16>,
    /// Unix s (display only).
    #[serde(default)]
    pub created_at: u64,
}

fn default_points() -> Vec<u16> {
    DEFAULT_POINTS.to_vec()
}

/// Who a standing is: a human by their proved driver id, an AI driver by
/// name.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Entrant {
    Driver(DriverId),
    Ai(String),
}

/// A driver's place in a championship.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Standing {
    pub entrant: Entrant,
    /// The name they last raced under.
    pub name: String,
    pub points: u32,
    pub wins: u16,
    /// Their finishing place in each round, in round order; 0 for a round
    /// not raced (or not run yet). A car that retired keeps its place in
    /// the classification and scores nothing.
    pub results: Vec<u16>,
}

/// A round as the standings see it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Round {
    pub event_id: String,
    /// The event's name; empty for an event that no longer exists.
    pub name: String,
    /// The event's phase; `None` for one that no longer exists.
    pub phase: Option<EventPhase>,
    /// Its race has been run and kept.
    pub raced: bool,
}

/// The rounds and the standings, best first.
pub fn standings(champ: &Championship, events: &[Event]) -> (Vec<Round>, Vec<Standing>) {
    let rounds: Vec<Round> = champ
        .events
        .iter()
        .map(|id| {
            let event = events.iter().find(|e| &e.id == id);
            Round {
                event_id: id.clone(),
                name: event.map(|e| e.name.clone()).unwrap_or_default(),
                phase: event.map(|e| e.phase),
                raced: event.is_some_and(|e| e.result(EventPhase::Race).is_some()),
            }
        })
        .collect();
    let mut table: BTreeMap<Entrant, Standing> = BTreeMap::new();
    for (n, id) in champ.events.iter().enumerate() {
        let Some(race) = events
            .iter()
            .find(|e| &e.id == id)
            .and_then(|e| e.result(EventPhase::Race))
        else {
            continue;
        };
        for line in &race.lines {
            let entrant = match line.driver_id {
                Some(id) => Entrant::Driver(id),
                None => Entrant::Ai(line.name.clone()),
            };
            let standing = table.entry(entrant.clone()).or_insert_with(|| Standing {
                entrant,
                name: line.name.clone(),
                points: 0,
                wins: 0,
                results: vec![0; champ.events.len()],
            });
            standing.name = line.name.clone();
            standing.results[n] = line.position;
            let retired = line.status.as_deref() == Some("retired");
            if !retired {
                let place = line.position as usize;
                if place >= 1 {
                    standing.points += u32::from(champ.points.get(place - 1).copied().unwrap_or(0));
                }
                if line.position == 1 {
                    standing.wins += 1;
                }
            }
        }
    }
    let mut out: Vec<Standing> = table.into_values().collect();
    // Points, then wins, then the better places in turn (count-back), then
    // the name.
    let places = |s: &Standing| {
        let mut p: Vec<u16> = s.results.iter().copied().filter(|p| *p > 0).collect();
        p.sort();
        p
    };
    out.sort_by(|a, b| {
        b.points
            .cmp(&a.points)
            .then(b.wins.cmp(&a.wins))
            .then_with(|| places(a).cmp(&places(b)))
            .then_with(|| a.name.cmp(&b.name))
    });
    (rounds, out)
}

/// Every championship, one file each in `dir` (none: in memory only).
#[derive(Default)]
pub struct ChampionshipStore {
    dir: Option<PathBuf>,
    inner: Mutex<BTreeMap<String, Championship>>,
}

impl ChampionshipStore {
    pub fn in_memory() -> Self {
        Self::default()
    }

    /// The championships in `dir`; a file that does not read is logged and
    /// left alone.
    pub fn open(dir: impl Into<PathBuf>) -> Self {
        let dir = dir.into();
        let mut all = BTreeMap::new();
        if let Ok(read) = std::fs::read_dir(&dir) {
            for path in read
                .filter_map(|e| e.ok().map(|e| e.path()))
                .filter(|p| p.extension().is_some_and(|x| x == "json"))
            {
                match std::fs::read(&path)
                    .map_err(|e| e.to_string())
                    .and_then(|b| {
                        serde_json::from_slice::<Championship>(&b).map_err(|e| e.to_string())
                    }) {
                    Ok(c) => {
                        all.insert(c.id.clone(), c);
                    }
                    Err(e) => warn!("Ignoring unreadable championship {}: {}", path.display(), e),
                }
            }
        }
        if !all.is_empty() {
            info!(
                "Loaded {} championship(s) from {}",
                all.len(),
                dir.display()
            );
        }
        Self {
            dir: Some(dir),
            inner: Mutex::new(all),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, BTreeMap<String, Championship>> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn save(&self, c: &Championship) {
        let Some(dir) = &self.dir else {
            return;
        };
        let path = dir.join(format!("{}.json", c.id));
        let written = serde_json::to_vec_pretty(c)
            .map_err(std::io::Error::other)
            .and_then(|bytes| {
                std::fs::create_dir_all(dir)?;
                let tmp = path.with_extension("json.tmp");
                std::fs::write(&tmp, bytes)?;
                std::fs::rename(&tmp, &path)
            });
        if let Err(e) = written {
            warn!("Cannot write the championship {}: {}", path.display(), e);
        }
    }

    pub fn list(&self) -> Vec<Championship> {
        self.lock().values().cloned().collect()
    }

    pub fn get(&self, id: &str) -> Option<Championship> {
        self.lock().get(id).cloned()
    }

    /// A new championship over `events` (in round order, at most
    /// [`MAX_ROUNDS`], each once); its id is its name's slug, `-2`, `-3`...
    /// after it while that is taken. An empty points table is the default.
    pub fn create(
        &self,
        name: &str,
        events: Vec<String>,
        points: Vec<u16>,
        now: u64,
    ) -> Championship {
        let name: String = name
            .trim()
            .chars()
            .take(crate::events::MAX_EVENT_NAME)
            .collect();
        let mut all = self.lock();
        let base = crate::events::slug(&name);
        let id = (1..)
            .map(|n| {
                if n == 1 {
                    base.clone()
                } else {
                    format!("{base}-{n}")
                }
            })
            .find(|id| !all.contains_key(id))
            .unwrap_or(base);
        let c = Championship {
            version: CHAMPIONSHIP_FILE_VERSION,
            id: id.clone(),
            name,
            events: clean_rounds(events),
            points: clean_points(points),
            created_at: now,
        };
        all.insert(id, c.clone());
        drop(all);
        self.save(&c);
        c
    }

    /// Change a championship's rounds or points and write it.
    pub fn update(
        &self,
        id: &str,
        events: Option<Vec<String>>,
        points: Option<Vec<u16>>,
    ) -> Option<Championship> {
        let mut all = self.lock();
        let c = all.get_mut(id)?;
        if let Some(events) = events {
            c.events = clean_rounds(events);
        }
        if let Some(points) = points {
            c.points = clean_points(points);
        }
        let c = c.clone();
        drop(all);
        self.save(&c);
        Some(c)
    }

    pub fn remove(&self, id: &str) -> Option<Championship> {
        let gone = self.lock().remove(id)?;
        if let Some(dir) = &self.dir {
            let _ = std::fs::remove_file(dir.join(format!("{id}.json")));
        }
        Some(gone)
    }

    /// Every championship with its rounds and standings, as
    /// `Championships` sends them.
    pub fn wire(&self, events: &[Event]) -> crate::network::ChampionshipsData {
        use crate::network::{ChampionshipData, ChampionshipRoundData, ChampionshipStandingData};
        crate::network::ChampionshipsData {
            championships: self
                .list()
                .iter()
                .map(|c| {
                    let (rounds, table) = standings(c, events);
                    ChampionshipData {
                        id: c.id.clone(),
                        name: c.name.clone(),
                        rounds: rounds
                            .into_iter()
                            .map(|r| ChampionshipRoundData {
                                event_id: r.event_id,
                                name: r.name,
                                phase: r.phase.map_or(0, EventPhase::wire),
                                raced: r.raced,
                            })
                            .collect(),
                        standings: table
                            .into_iter()
                            .enumerate()
                            .map(|(i, s)| ChampionshipStandingData {
                                position: (i + 1).min(u16::MAX as usize) as u16,
                                name: s.name,
                                ai: matches!(s.entrant, Entrant::Ai(_)),
                                points: s.points,
                                wins: s.wins,
                                results: s.results,
                            })
                            .collect(),
                    }
                })
                .collect(),
        }
    }
}

fn clean_rounds(events: Vec<String>) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for e in events {
        let e = e.trim().to_string();
        if !e.is_empty() && !out.contains(&e) && out.len() < MAX_ROUNDS {
            out.push(e);
        }
    }
    out
}

fn clean_points(points: Vec<u16>) -> Vec<u16> {
    if points.is_empty() {
        default_points()
    } else {
        points.into_iter().take(40).map(|p| p.min(1000)).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::{PhaseResult, ResultLine};

    fn event(id: &str, lines: &[(&str, Option<u128>, &str)]) -> Event {
        let spec: crate::admin::hosted::HostedSpec = serde_json::from_value(serde_json::json!({
            "track_id": uuid::Uuid::from_u128(1),
            "car_id": uuid::Uuid::from_u128(2),
        }))
        .unwrap();
        let mut e = Event::new(id.into(), id.to_uppercase(), spec, true, 0);
        e.results.push(PhaseResult {
            phase: EventPhase::Race,
            finished_at: 0,
            session: None,
            lines: lines
                .iter()
                .enumerate()
                .map(|(i, (name, driver, status))| ResultLine {
                    position: i as u16 + 1,
                    name: (*name).into(),
                    driver_id: driver.map(uuid::Uuid::from_u128),
                    ai: driver.is_none().then_some(1),
                    ai_id: None,
                    car_id: uuid::Uuid::from_u128(2),
                    car_name: String::new(),
                    best_lap_ms: None,
                    laps: 3,
                    race_time_ms: None,
                    status: Some((*status).into()),
                    knocked_out_in: None,
                })
                .collect(),
        });
        e.phase = EventPhase::Done;
        e
    }

    #[test]
    fn points_add_up_by_driver_id_across_rounds() {
        let r1 = event(
            "r1",
            &[
                ("Ada", Some(1), "finished"),
                ("Bot", None, "finished"),
                ("Bea", Some(2), "retired"),
            ],
        );
        // Ada races the second round under another name: the same id.
        let r2 = event(
            "r2",
            &[
                ("Bea", Some(2), "finished"),
                ("Ada R.", Some(1), "finished"),
                ("Bot", None, "finished"),
            ],
        );
        let champ = Championship {
            version: 1,
            id: "series".into(),
            name: "Series".into(),
            events: vec!["r1".into(), "r2".into(), "r3".into()],
            points: vec![10, 6, 4],
            created_at: 0,
        };
        let (rounds, table) = standings(&champ, &[r1, r2]);
        assert_eq!(rounds.len(), 3);
        assert!(rounds[0].raced && rounds[1].raced && !rounds[2].raced);
        assert_eq!(rounds[2].phase, None, "a round not made yet");
        let row = |n: usize| (table[n].name.as_str(), table[n].points, table[n].wins);
        assert_eq!(row(0), ("Ada R.", 16, 1), "10 + 6, under the latest name");
        assert_eq!(row(1), ("Bea", 10, 1), "a retirement scores nothing");
        assert_eq!(row(2), ("Bot", 10, 0), "an AI by name; behind on wins");
        assert_eq!(table[1].results, vec![3, 1, 0]);
        assert_eq!(table[0].entrant, Entrant::Driver(uuid::Uuid::from_u128(1)));
    }

    #[test]
    fn a_championship_round_trips_its_file() {
        let dir = tempfile::tempdir().unwrap();
        let store = ChampionshipStore::open(dir.path());
        let c = store.create(
            "Autumn Series",
            vec!["a".into(), " a ".into(), "b".into(), String::new()],
            Vec::new(),
            5,
        );
        assert_eq!(c.id, "autumn-series");
        assert_eq!(c.events, vec!["a".to_string(), "b".to_string()]);
        assert_eq!(c.points, DEFAULT_POINTS.to_vec());
        store.update(&c.id, None, Some(vec![3, 2, 1]));
        let again = ChampionshipStore::open(dir.path());
        assert_eq!(again.get("autumn-series").unwrap().points, vec![3, 2, 1]);
        assert!(again.remove("autumn-series").is_some());
        assert!(ChampionshipStore::open(dir.path()).list().is_empty());
    }
}

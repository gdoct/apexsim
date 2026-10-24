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
use crate::events::{Event, EventPhase, ResultLine};
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
/// Most points a round's fastest lap scores.
pub const MAX_FASTEST_LAP_POINTS: u8 = 5;

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
    /// Points for each round's fastest lap (0 to
    /// [`MAX_FASTEST_LAP_POINTS`]); 0, the default, is no bonus, and is
    /// left out of the file.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub fastest_lap_points: u8,
    /// The fastest lap scores whatever its holder's place (any car not
    /// retired); false, the default, needs a points finish (a place the
    /// table gives points, not retired). Left out of the file when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub fastest_lap_any_place: bool,
}

fn default_points() -> Vec<u16> {
    DEFAULT_POINTS.to_vec()
}

fn is_zero(n: &u8) -> bool {
    *n == 0
}

/// A championship's fastest-lap rule (`Championship::fastest_lap_points`,
/// `fastest_lap_any_place`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize)]
pub struct FastestLapRule {
    #[serde(default)]
    pub points: u8,
    #[serde(default)]
    pub any_place: bool,
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
    /// Rounds whose fastest-lap points they scored.
    #[serde(default)]
    pub fastest_laps: u16,
}

/// A raced round's fastest lap: who set it and whether it scored.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FastestLap {
    pub entrant: Entrant,
    pub name: String,
    pub ms: u32,
    /// Its holder scored the championship's fastest-lap points.
    pub scored: bool,
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
    /// The race's fastest lap once it is raced (none when nobody set a
    /// lap).
    pub fastest_lap: Option<FastestLap>,
}

fn entrant_of(line: &ResultLine) -> Entrant {
    match line.driver_id {
        Some(id) => Entrant::Driver(id),
        None => Entrant::Ai(line.name.clone()),
    }
}

fn retired(line: &ResultLine) -> bool {
    line.status.as_deref() == Some("retired")
}

/// The points a line's place scores; a retired car scores none.
fn place_points(champ: &Championship, line: &ResultLine) -> u32 {
    let place = line.position as usize;
    if retired(line) || place == 0 {
        return 0;
    }
    u32::from(champ.points.get(place - 1).copied().unwrap_or(0))
}

/// A race's fastest lap: the lowest best lap in its classification, a
/// retired car's included; on equal laps the better-placed car holds it
/// (the kept result has no time for when a lap was set). It scores the
/// championship's fastest-lap points when there are any, its holder is
/// not retired and, without `fastest_lap_any_place`, their place scored
/// points; else nobody scores them.
pub fn fastest_lap(champ: &Championship, lines: &[ResultLine]) -> Option<FastestLap> {
    let mut by_place: Vec<&ResultLine> = lines.iter().collect();
    by_place.sort_by_key(|l| l.position);
    let mut best: Option<(&ResultLine, u32)> = None;
    for line in by_place {
        let Some(ms) = line.best_lap_ms.filter(|ms| *ms > 0) else {
            continue;
        };
        if best.is_none_or(|(_, b)| ms < b) {
            best = Some((line, ms));
        }
    }
    let (line, ms) = best?;
    let scored = champ.fastest_lap_points > 0
        && !retired(line)
        && (champ.fastest_lap_any_place || place_points(champ, line) > 0);
    Some(FastestLap {
        entrant: entrant_of(line),
        name: line.name.clone(),
        ms,
        scored,
    })
}

/// The rounds and the standings, best first.
pub fn standings(champ: &Championship, events: &[Event]) -> (Vec<Round>, Vec<Standing>) {
    let rounds: Vec<Round> = champ
        .events
        .iter()
        .map(|id| {
            let event = events.iter().find(|e| &e.id == id);
            let race = event.and_then(|e| e.result(EventPhase::Race));
            Round {
                event_id: id.clone(),
                name: event.map(|e| e.name.clone()).unwrap_or_default(),
                phase: event.map(|e| e.phase),
                raced: race.is_some(),
                fastest_lap: race.and_then(|r| fastest_lap(champ, &r.lines)),
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
        let fastest = rounds[n].fastest_lap.as_ref().filter(|f| f.scored);
        for line in &race.lines {
            let entrant = entrant_of(line);
            let standing = table.entry(entrant.clone()).or_insert_with(|| Standing {
                entrant: entrant.clone(),
                name: line.name.clone(),
                points: 0,
                wins: 0,
                results: vec![0; champ.events.len()],
                fastest_laps: 0,
            });
            standing.name = line.name.clone();
            standing.results[n] = line.position;
            standing.points += place_points(champ, line);
            if !retired(line) && line.position == 1 {
                standing.wins += 1;
            }
            if fastest.is_some_and(|f| f.entrant == entrant) {
                standing.points += u32::from(champ.fastest_lap_points);
                standing.fastest_laps += 1;
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
        fastest_lap: FastestLapRule,
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
            fastest_lap_points: fastest_lap.points.min(MAX_FASTEST_LAP_POINTS),
            fastest_lap_any_place: fastest_lap.any_place,
        };
        all.insert(id, c.clone());
        drop(all);
        self.save(&c);
        c
    }

    /// Change a championship's rounds, points or fastest-lap rule and
    /// write it.
    pub fn update(
        &self,
        id: &str,
        events: Option<Vec<String>>,
        points: Option<Vec<u16>>,
        fastest_lap: Option<FastestLapRule>,
    ) -> Option<Championship> {
        let mut all = self.lock();
        let c = all.get_mut(id)?;
        if let Some(events) = events {
            c.events = clean_rounds(events);
        }
        if let Some(points) = points {
            c.points = clean_points(points);
        }
        if let Some(rule) = fastest_lap {
            c.fastest_lap_points = rule.points.min(MAX_FASTEST_LAP_POINTS);
            c.fastest_lap_any_place = rule.any_place;
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
                        fastest_lap_points: c.fastest_lap_points,
                        fastest_lap_any_place: c.fastest_lap_any_place,
                        rounds: rounds
                            .into_iter()
                            .map(|r| ChampionshipRoundData {
                                event_id: r.event_id,
                                name: r.name,
                                phase: r.phase.map_or(0, EventPhase::wire),
                                raced: r.raced,
                                fastest_lap_ms: r.fastest_lap.as_ref().map_or(0, |f| f.ms),
                                fastest_lap_scored: r
                                    .fastest_lap
                                    .as_ref()
                                    .is_some_and(|f| f.scored),
                                fastest_lap: r.fastest_lap.map(|f| f.name).unwrap_or_default(),
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
                                fastest_laps: s.fastest_laps,
                            })
                            .collect(),
                    }
                })
                .collect(),
        }
    }
}

/// An event's race has just been kept: when the event is a round of any
/// championship, the session's humans and spectators are sent
/// `Championships` (as `RequestChampionships` answers it), so the results
/// screen shows standings that count the race just run.
pub(crate) async fn push_after_race(
    ctx: &crate::game_loop::GameLoopCtx,
    session: crate::data::SessionId,
    event_id: &str,
) {
    let (data, audience) = {
        let st = ctx.state.read().await;
        if !st
            .championships
            .list()
            .iter()
            .any(|c| c.events.iter().any(|e| e == event_id))
        {
            return;
        }
        let data = st.championships.wire(&st.events.list());
        let audience = crate::admin::hosted::session_audience(&st, session).await;
        (data, audience)
    };
    info!(
        "Event {} raced: its championships to {} in session {}",
        event_id,
        audience.len(),
        session
    );
    for player in audience {
        if let Some(conn) = ctx.player_connection(player).await {
            let _ = ctx
                .send(
                    conn,
                    crate::network::ServerMessage::Championships(data.clone()),
                )
                .await;
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
    use crate::events::PhaseResult;

    fn event(id: &str, lines: &[(&str, Option<u128>, &str)]) -> Event {
        let timed: Vec<(&str, Option<u128>, &str, Option<u32>)> =
            lines.iter().map(|(n, d, s)| (*n, *d, *s, None)).collect();
        timed_event(id, &timed)
    }

    /// A raced event whose lines carry best laps.
    fn timed_event(id: &str, lines: &[(&str, Option<u128>, &str, Option<u32>)]) -> Event {
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
                .map(|(i, (name, driver, status, best))| ResultLine {
                    position: i as u16 + 1,
                    name: (*name).into(),
                    driver_id: driver.map(uuid::Uuid::from_u128),
                    ai: driver.is_none().then_some(1),
                    ai_id: None,
                    car_id: uuid::Uuid::from_u128(2),
                    car_name: String::new(),
                    best_lap_ms: *best,
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
            fastest_lap_points: 0,
            fastest_lap_any_place: false,
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
            FastestLapRule::default(),
            5,
        );
        assert_eq!(c.id, "autumn-series");
        assert_eq!(c.events, vec!["a".to_string(), "b".to_string()]);
        assert_eq!(c.points, DEFAULT_POINTS.to_vec());
        store.update(&c.id, None, Some(vec![3, 2, 1]), None);
        let again = ChampionshipStore::open(dir.path());
        assert_eq!(again.get("autumn-series").unwrap().points, vec![3, 2, 1]);
        let rule = FastestLapRule {
            points: 9,
            any_place: true,
        };
        store.update(&c.id, None, None, Some(rule));
        let ruled = ChampionshipStore::open(dir.path())
            .get("autumn-series")
            .unwrap();
        assert_eq!(
            (ruled.fastest_lap_points, ruled.fastest_lap_any_place),
            (MAX_FASTEST_LAP_POINTS, true),
            "at most 5 points"
        );
        assert!(again.remove("autumn-series").is_some());
        assert!(ChampionshipStore::open(dir.path()).list().is_empty());
    }

    fn series(fastest_lap_points: u8, fastest_lap_any_place: bool) -> Championship {
        Championship {
            version: 1,
            id: "series".into(),
            name: "Series".into(),
            events: vec!["r1".into()],
            points: vec![10, 6],
            created_at: 0,
            fastest_lap_points,
            fastest_lap_any_place,
        }
    }

    fn points_of(table: &[Standing], name: &str) -> (u32, u16) {
        table
            .iter()
            .find(|s| s.name == name)
            .map(|s| (s.points, s.fastest_laps))
            .unwrap()
    }

    #[test]
    fn the_fastest_lap_needs_a_points_finish_unless_any_place_scores() {
        // Cy, third, outside the two scoring places, sets the fastest lap.
        let r1 = timed_event(
            "r1",
            &[
                ("Ada", Some(1), "finished", Some(81_000)),
                ("Bea", Some(2), "finished", Some(80_500)),
                ("Cy", Some(3), "finished", Some(80_000)),
            ],
        );
        let (rounds, table) = standings(&series(1, false), std::slice::from_ref(&r1));
        let fastest = rounds[0].fastest_lap.as_ref().unwrap();
        assert_eq!((fastest.name.as_str(), fastest.ms), ("Cy", 80_000));
        assert!(!fastest.scored, "no points finish, no bonus");
        assert_eq!(points_of(&table, "Cy"), (0, 0));
        assert_eq!(points_of(&table, "Bea"), (6, 0), "nobody else takes it");

        let (rounds, table) = standings(&series(1, true), std::slice::from_ref(&r1));
        assert!(rounds[0].fastest_lap.as_ref().unwrap().scored);
        assert_eq!(points_of(&table, "Cy"), (1, 1), "any place scores it");
        // Bea's fastest lap from a points finish scores under both rules.
        let r1 = timed_event(
            "r1",
            &[
                ("Ada", Some(1), "finished", Some(81_000)),
                ("Bea", Some(2), "finished", Some(79_000)),
            ],
        );
        let (_, table) = standings(&series(2, false), &[r1]);
        assert_eq!(points_of(&table, "Bea"), (8, 1));
        assert_eq!(table[0].name, "Ada", "count-back unchanged: 10 beats 8");
    }

    #[test]
    fn a_retired_car_holds_the_fastest_lap_and_never_scores_it() {
        let r1 = timed_event(
            "r1",
            &[
                ("Ada", Some(1), "finished", Some(81_000)),
                ("Bot", None, "retired", Some(79_000)),
            ],
        );
        for any_place in [false, true] {
            let (rounds, table) = standings(&series(1, any_place), std::slice::from_ref(&r1));
            let fastest = rounds[0].fastest_lap.as_ref().unwrap();
            assert_eq!(fastest.entrant, Entrant::Ai("Bot".into()));
            assert!(!fastest.scored);
            assert_eq!(points_of(&table, "Bot"), (0, 0));
            assert_eq!(points_of(&table, "Ada"), (10, 0));
        }
    }

    #[test]
    fn an_equal_fastest_lap_is_the_better_placed_cars() {
        // Lines kept out of place order: the place decides, not the order.
        let mut r1 = timed_event(
            "r1",
            &[
                ("Ada", Some(1), "finished", Some(80_000)),
                ("Bea", Some(2), "finished", Some(80_000)),
                ("Cy", Some(3), "finished", None),
            ],
        );
        r1.results[0].lines.reverse();
        let (rounds, table) = standings(&series(1, false), &[r1]);
        assert_eq!(rounds[0].fastest_lap.as_ref().unwrap().name, "Ada");
        assert_eq!(points_of(&table, "Ada"), (11, 1));
        assert_eq!(points_of(&table, "Bea"), (6, 0));
    }

    #[test]
    fn a_file_without_the_rule_scores_and_writes_as_before() {
        let old = serde_json::json!({
            "version": 1, "id": "series", "name": "Series",
            "events": ["r1"], "points": [10, 6], "created_at": 0,
        });
        let champ: Championship = serde_json::from_value(old.clone()).unwrap();
        assert_eq!(champ, series(0, false));
        assert_eq!(serde_json::to_value(&champ).unwrap(), old, "the same file");
        let r1 = timed_event(
            "r1",
            &[
                ("Ada", Some(1), "finished", Some(81_000)),
                ("Bea", Some(2), "finished", Some(79_000)),
            ],
        );
        let (rounds, table) = standings(&champ, &[r1]);
        // The lap is named on the round; nothing scores it.
        assert!(!rounds[0].fastest_lap.as_ref().unwrap().scored);
        assert_eq!(
            table.iter().map(|s| s.points).collect::<Vec<_>>(),
            vec![10, 6]
        );
        // No lap set at all: no fastest lap.
        let (rounds, _) = standings(&champ, &[event("r1", &[("Ada", Some(1), "finished")])]);
        assert!(rounds[0].fastest_lap.is_none());
    }
}

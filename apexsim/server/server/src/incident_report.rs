//! A race's incident list for stewards, as the admin dashboard shows it and
//! as it is kept on disk (`crate::incidents` holds the list the race's tick
//! fills).
//!
//! Each race that finishes leaves `<records dir>/results/<session
//! id>_<unix time>.incidents.json` beside its results (`[records] results`):
//! the cars, and every incident with its session tick, lap, seconds since
//! the green (`race_s`) and seconds into the server's replay of the race
//! (`replay_s`, where the client's `apexsim.watch seek` puts the replay).
//! A running race's list is made the same way from its live session.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::data::*;
use crate::game_session::GameSession;
use crate::incidents::Incident;

/// The file name's ending beside the results' `.json` and `.csv`.
pub const FILE_SUFFIX: &str = ".incidents.json";

/// The newest reports the dashboard lists.
pub const LIST_MAX: usize = 50;

/// One car of the race, as the incidents name it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ReportCar {
    pub id: PlayerId,
    pub name: String,
    pub ai: bool,
    /// Its slot on the grid.
    pub grid: u8,
}

/// An incident with its place in the race and in the replay.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ReportIncident {
    #[serde(flatten)]
    pub incident: Incident,
    /// Seconds since the green.
    pub race_s: f64,
    /// Seconds into the server's replay of the race; none when it was not
    /// recorded.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub replay_s: Option<f64>,
}

/// A race's incidents.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct IncidentReport {
    pub session_id: SessionId,
    pub track_name: String,
    /// Unix seconds when it was written (the results' own); 0 for a race
    /// still running.
    pub finished_at: u64,
    pub tick_rate: u16,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub race_start_tick: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub replay_start_tick: Option<u32>,
    pub cars: Vec<ReportCar>,
    pub incidents: Vec<ReportIncident>,
}

impl IncidentReport {
    /// The report of `session` as it stands, its humans named from `names`.
    pub fn of(
        session: &GameSession,
        names: &HashMap<PlayerId, String>,
        finished_at: u64,
    ) -> IncidentReport {
        let rate = f64::from(session.tick_rate_hz().max(1));
        let race_start = session.session.race_start_tick;
        let replay_start = session.incidents.replay_start_tick;
        let cars = session
            .session
            .participants
            .iter()
            .map(|(id, state)| ReportCar {
                id: *id,
                name: session
                    .ai_profiles
                    .get(id)
                    .map(|p| p.name.clone())
                    .or_else(|| names.get(id).cloned())
                    .unwrap_or_else(|| "Driver".to_string()),
                ai: session.ai_profiles.contains_key(id),
                grid: state.grid_position,
            })
            .collect();
        let incidents = session
            .incidents
            .incidents
            .iter()
            .map(|incident| ReportIncident {
                race_s: f64::from(incident.tick.saturating_sub(race_start.unwrap_or(0))) / rate,
                replay_s: replay_start
                    .map(|start| f64::from(incident.tick.saturating_sub(start)) / rate),
                incident: incident.clone(),
            })
            .collect();
        IncidentReport {
            session_id: session.session.id,
            track_name: session.track_config.name.clone(),
            finished_at,
            tick_rate: session.tick_rate_hz(),
            race_start_tick: race_start,
            replay_start_tick: replay_start,
            cars,
            incidents,
        }
    }

    /// The file's name, beside the results of the same race.
    pub fn file_name(&self) -> String {
        format!("{}_{}{}", self.session_id, self.finished_at, FILE_SUFFIX)
    }

    /// Write it into `dir` (made if missing); its path.
    pub fn write(&self, dir: &Path) -> std::io::Result<PathBuf> {
        std::fs::create_dir_all(dir)?;
        let path = dir.join(self.file_name());
        let body = serde_json::to_string_pretty(self).map_err(std::io::Error::other)?;
        std::fs::write(&path, body)?;
        Ok(path)
    }
}

/// The session's incident list as the client gets it
/// (`ServerMessage::Incidents`): each car by its index and name, the latest
/// [`crate::network::MAX_INCIDENTS_SENT`].
pub fn wire(session: &GameSession) -> crate::network::IncidentsData {
    let list = &session.incidents.incidents;
    let from = list
        .len()
        .saturating_sub(crate::network::MAX_INCIDENTS_SENT);
    let car = |id: &PlayerId| {
        let name = session.driver_name(id);
        (
            session.car_index_of(id),
            if name.is_empty() {
                "Driver".to_string()
            } else {
                name
            },
        )
    };
    crate::network::IncidentsData {
        session_id: session.session.id,
        tick_rate: session.tick_rate_hz(),
        race_start_tick: session.session.race_start_tick,
        incidents: list[from..]
            .iter()
            .map(|i| crate::network::IncidentData::of(i, car))
            .collect(),
    }
}

/// The reports in `dir`, newest first, at most [`LIST_MAX`]; one that does
/// not parse is left out.
pub fn list(dir: &Path) -> Vec<IncidentReport> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut reports: Vec<IncidentReport> = entries
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().ends_with(FILE_SUFFIX))
        .filter_map(|e| serde_json::from_slice(&std::fs::read(e.path()).ok()?).ok())
        .collect();
    reports.sort_by_key(|r| std::cmp::Reverse(r.finished_at));
    reports.truncate(LIST_MAX);
    reports
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::incidents::{IncidentKind, Severity};

    #[test]
    fn a_report_is_written_and_listed_back() {
        let a = uuid::Uuid::from_u128(1);
        let b = uuid::Uuid::from_u128(2);
        let report = IncidentReport {
            session_id: uuid::Uuid::from_u128(9),
            track_name: "Monzarella".into(),
            finished_at: 1_792_200_000,
            tick_rate: 420,
            race_start_tick: Some(840),
            replay_start_tick: Some(840),
            cars: vec![ReportCar {
                id: a,
                name: "Rex".into(),
                ai: true,
                grid: 1,
            }],
            incidents: vec![ReportIncident {
                incident: Incident {
                    tick: 1260,
                    lap: 1,
                    car: a,
                    kind: IncidentKind::Contact {
                        other: b,
                        closing_mps: 3.0,
                        severity: Severity::Light,
                    },
                },
                race_s: 1.0,
                replay_s: Some(1.0),
            }],
        };
        let dir = std::env::temp_dir().join(format!("apexsim_incidents_{}", std::process::id()));
        let path = report.write(&dir).unwrap();
        assert!(path
            .to_string_lossy()
            .ends_with("00000000-0000-0000-0000-000000000009_1792200000.incidents.json"));
        let json: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(json["incidents"][0]["kind"], "contact");
        assert_eq!(json["incidents"][0]["severity"], "light");
        assert_eq!(json["incidents"][0]["replay_s"], 1.0);
        assert_eq!(list(&dir), vec![report]);
        let _ = std::fs::remove_dir_all(&dir);
    }
}

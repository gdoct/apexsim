//! A finished race's classification, written to disk for leagues and tools.
//!
//! Each race session that finishes leaves `<records dir>/results/<session
//! id>_<unix time>.json` and a `.csv` beside it (`[records] results`, on by
//! default): every car in classified order, with its laps, best lap, race
//! time, the gap to the winner or the laps it was down, and the pit rule's
//! penalty. Cars still running at the end follow the finishers, by laps
//! and distance. Written off the game loop.

use std::path::{Path, PathBuf};

use serde::Serialize;

use crate::data::{CarConfigId, PlayerId};

/// One car's line of the classification.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ClassifiedCar {
    /// 1-based place; the finishers first, then the cars still running.
    pub position: u8,
    pub player_id: PlayerId,
    pub name: String,
    pub is_ai: bool,
    pub car_config_id: CarConfigId,
    /// What became of it: `finished`, `running` (still out when the race
    /// ended) or `retired`.
    pub status: &'static str,
    pub laps: u16,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub best_lap_ms: Option<u32>,
    /// From the green to its flag, ms; a car that did not finish has none.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub race_time_ms: Option<u32>,
    /// Behind the winner, ms, on the same lap; none for the winner and for
    /// a car laps down.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gap_ms: Option<u32>,
    pub laps_down: u16,
    /// Took the flag owing a stop or its second compound under the pit
    /// rule: classified behind every car that met it.
    pub missed_stops: bool,
}

/// A finished race, as written.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct RaceResult {
    pub session_id: String,
    pub track_name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub track_stem: Option<String>,
    /// Unix seconds when it was written.
    pub finished_at: u64,
    /// The race's distance: laps, or a timed race's seconds.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub lap_limit: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub race_seconds: Option<u32>,
    pub cars: Vec<ClassifiedCar>,
}

impl RaceResult {
    /// The classification as CSV: a header line, then one line a car.
    pub fn csv(&self) -> String {
        let mut out = String::from(
            "position,name,ai,status,laps,best_lap_ms,race_time_ms,gap_ms,laps_down,missed_stops\n",
        );
        let opt = |v: Option<u32>| v.map_or(String::new(), |v| v.to_string());
        for car in &self.cars {
            // A name is quoted, its own quotes doubled.
            let name = format!("\"{}\"", car.name.replace('"', "\"\""));
            out.push_str(&format!(
                "{},{},{},{},{},{},{},{},{},{}\n",
                car.position,
                name,
                car.is_ai,
                car.status,
                car.laps,
                opt(car.best_lap_ms),
                opt(car.race_time_ms),
                opt(car.gap_ms),
                car.laps_down,
                car.missed_stops
            ));
        }
        out
    }

    /// Write the JSON and the CSV into `dir` (made if missing); the JSON's
    /// path.
    pub fn write(&self, dir: &Path) -> std::io::Result<PathBuf> {
        std::fs::create_dir_all(dir)?;
        let stem = format!("{}_{}", self.session_id, self.finished_at);
        let json = dir.join(format!("{stem}.json"));
        let body = serde_json::to_string_pretty(self).map_err(std::io::Error::other)?;
        std::fs::write(&json, body)?;
        std::fs::write(dir.join(format!("{stem}.csv")), self.csv())?;
        Ok(json)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn car(position: u8, name: &str, status: &'static str, laps: u16) -> ClassifiedCar {
        ClassifiedCar {
            position,
            player_id: uuid::Uuid::from_u128(position as u128),
            name: name.to_string(),
            is_ai: true,
            car_config_id: uuid::Uuid::nil(),
            status,
            laps,
            best_lap_ms: Some(80_000),
            race_time_ms: (status == "finished").then_some(400_000 + position as u32),
            gap_ms: (position == 2).then_some(1_234),
            laps_down: 0,
            missed_stops: false,
        }
    }

    #[test]
    fn a_result_is_written_as_json_and_csv() {
        let result = RaceResult {
            session_id: "abc".into(),
            track_name: "Monzarella".into(),
            track_stem: Some("Monza".into()),
            finished_at: 1_792_200_000,
            lap_limit: Some(5),
            race_seconds: None,
            cars: vec![
                car(1, "Rex \"T\" Thunder", "finished", 5),
                car(2, "Luna Swift", "finished", 5),
                car(3, "Max Voltage", "retired", 2),
            ],
        };
        let csv = result.csv();
        let lines: Vec<&str> = csv.lines().collect();
        assert_eq!(lines.len(), 4);
        assert_eq!(
            lines[1],
            "1,\"Rex \"\"T\"\" Thunder\",true,finished,5,80000,400001,,0,false"
        );
        assert_eq!(lines[3], "3,\"Max Voltage\",true,retired,2,80000,,,0,false");
        let dir = std::env::temp_dir().join(format!("apexsim_results_{}", std::process::id()));
        let path = result.write(&dir).unwrap();
        let back: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(back["cars"][1]["gap_ms"], 1234);
        assert!(back["cars"][0].get("gap_ms").is_none(), "left off for none");
        assert!(dir.join("abc_1792200000.csv").is_file());
        let _ = std::fs::remove_dir_all(&dir);
    }
}

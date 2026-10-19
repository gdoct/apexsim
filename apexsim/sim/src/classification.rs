//! A session's classification as plain data: a qualifying session's
//! drivers by best lap ([`QualifyingResult`]) and a finished race's cars in
//! classified order ([`ClassifiedCar`]). The session builds them
//! (`game_session`); the record store (`records`) and the results writer
//! (`results`) keep them on disk.

use serde::{Deserialize, Serialize};

use crate::data::{CarConfigId, PlayerId, TrackConfigId};

/// One driver's line of a qualifying result.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct QualifyingEntry {
    pub name: String,
    pub car_config_id: CarConfigId,
    pub lap_time_ms: u32,
    pub is_ai: bool,
}

/// A qualifying session's classification: its drivers by best legal lap,
/// fastest first. `id` is the session's, so a session that is still setting
/// laps keeps replacing its own result.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct QualifyingResult {
    pub id: uuid::Uuid,
    pub track_id: TrackConfigId,
    /// The host car's class (`F1`, `GT3`...), for the picker's label.
    pub class: String,
    /// RFC 3339; stamped by the store when empty. Display only.
    pub recorded_at: String,
    pub entries: Vec<QualifyingEntry>,
}

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
    /// Seconds race control's penalties added to its race time
    /// (`race_control::Penalty::added_s`); counted in `race_time_ms`.
    pub penalty_s: u32,
}

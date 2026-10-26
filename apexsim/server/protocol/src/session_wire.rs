//! A session as the wire carries it: the messages and telemetry built from a
//! [`GameSession`], and the wire's shapes of the plain data the session
//! hands out (`WeekendState`, `ShootoutState`, `PitService`, ...).
//!
//! The session is simulation; nothing in it knows the wire. Everything that
//! turns it into `crate::network` types lives here, on the runtime side, so
//! the simulation modules use no runtime module (docs/architecture.md).
//! [`SessionWire`] keeps the builders callable as methods of the session.

use std::collections::HashMap;

use crate::data::{PlayerId, SessionId};
use crate::game_session::{
    GameSession, KnockedOut, PitBoxInfo, PitService, RosterRow, ShootoutState, TrackCorner,
    WeekendState,
};
use crate::network::{
    CarStateTelemetry, CompactCarState, CompactTelemetry, CornerData, KnockedOutData, PenaltyData,
    PitBoxData, PitServiceData, RosterEntry, ServerMessage, SessionPhysics, SessionRosterData,
    ShootoutStateData, SkyNow, Telemetry, TrackCornersData, WeekendStateData, YellowFlags,
};

/// The wire's messages and telemetry of a session.
pub trait SessionWire {
    /// What a driver's client steps its own car with (`SessionJoined.Physics`):
    /// the tick rate, and the road mesh when this session's track drives on one.
    fn session_physics(&self) -> SessionPhysics;
    /// What a member joining the session is told after `SessionJoined`
    /// about the phase it is in: the weekend's state and the shoot-out's,
    /// where there are such.
    fn phase_state_messages(&self) -> Vec<ServerMessage>;
    /// The `TrackCorners` message for a joining watcher; `None` when the
    /// track has no corners.
    fn track_corners_message(&self, session_id: SessionId) -> Option<ServerMessage>;
    /// Get telemetry for broadcast
    fn get_telemetry(&self) -> ServerMessage;
    /// Compact wire telemetry (protocol v2): cars are identified by their
    /// position in the deterministic `participants` iteration order, matching
    /// the indices announced in the most recent `SessionRoster`.
    fn get_compact_telemetry(&self) -> CompactTelemetry;
    /// The race's yellow sectors for telemetry, `None` while none is out.
    fn yellow_flags(&self) -> Option<YellowFlags>;
    /// The sky as it is now, as telemetry and the spectator stream carry it.
    fn sky_now(&self) -> SkyNow;
    /// Build the car-index → player mapping for compact telemetry. Human
    /// player names are looked up in `names` (lobby data); AI names come from
    /// their profiles.
    fn build_roster(&self, names: &HashMap<PlayerId, String>) -> SessionRosterData;
}

impl SessionWire for GameSession {
    fn session_physics(&self) -> SessionPhysics {
        SessionPhysics {
            tick_rate_hz: self.tick_rate_hz(),
            road_contact: u8::from(self.track_config.road_mesh.is_some()),
            ..Default::default()
        }
    }

    fn phase_state_messages(&self) -> Vec<ServerMessage> {
        self.weekend_state()
            .map(|s| ServerMessage::WeekendState(WeekendStateData::from(&s)))
            .into_iter()
            .chain(
                self.shootout_state()
                    .map(|s| ServerMessage::ShootoutState(ShootoutStateData::from(&s))),
            )
            .collect()
    }

    fn track_corners_message(&self, session_id: SessionId) -> Option<ServerMessage> {
        if self.track_corners().is_empty() {
            return None;
        }
        Some(ServerMessage::TrackCorners(TrackCornersData {
            session_id,
            track_length_m: self.track_length_m(),
            corners: self.track_corners().iter().map(CornerData::from).collect(),
        }))
    }

    fn get_telemetry(&self) -> ServerMessage {
        let car_states: Vec<CarStateTelemetry> = self
            .session
            .participants
            .values()
            .map(CarStateTelemetry::from)
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / self.tick_rate_hz() as f32) * 1000.0) as u16);

        let telemetry = Telemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
        };

        ServerMessage::Telemetry(telemetry)
    }

    fn get_compact_telemetry(&self) -> CompactTelemetry {
        let car_states: Vec<CompactCarState> = self
            .session
            .participants
            .values()
            .enumerate()
            .map(|(idx, s)| CompactCarState::from_car_state(s, idx as u8))
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / self.tick_rate_hz() as f32) * 1000.0) as u16);

        CompactTelemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
            race_clock: self.race_clock(),
            sky: Some(self.sky_now()),
            yellow_flags: self.yellow_flags(),
            race_phase: self.race_phase(),
            safety_car: self
                .race_control
                .safety_car
                .as_ref()
                .and_then(|sc| sc.wire_pose())
                .map(Into::into),
        }
    }

    fn yellow_flags(&self) -> Option<YellowFlags> {
        let sectors: Vec<[u16; 2]> = self
            .race_control
            .yellows()
            .map(|(sector, level)| [sector.min(u16::MAX as usize) as u16, level as u16])
            .collect();
        if sectors.is_empty() {
            return None;
        }
        Some(YellowFlags {
            sector_m: crate::race_control::MARSHAL_SECTOR_M as u16,
            sectors,
        })
    }

    fn sky_now(&self) -> SkyNow {
        SkyNow::of(
            &self.sky,
            self.track_config.track_surface.wind_now_mps,
            self.sky_seconds(),
        )
    }

    fn build_roster(&self, names: &HashMap<PlayerId, String>) -> SessionRosterData {
        SessionRosterData {
            session_id: self.session.id,
            entries: self
                .roster_rows(names)
                .into_iter()
                .map(RosterEntry::from)
                .collect(),
        }
    }
}

/// A started pit stop as `ServerMessage::PitService` (and the spectator
/// stream) carries it, with the car's roster index.
pub fn pit_service_data(car_index: u8, service: &PitService) -> PitServiceData {
    PitServiceData {
        car_index,
        pit_box: service.pit_box,
        tyres_s: service.tyres_s,
        compound: service.compound,
        fuel_s: service.fuel_s,
        fuel_l: service.fuel_l,
        repair_s: service.repair_s,
        repair_pct: service.repair_pct,
        total_s: service.total_s,
        driver_s: service.driver_s,
        driver: service.driver,
    }
}

impl From<&WeekendState> for WeekendStateData {
    fn from(s: &WeekendState) -> Self {
        WeekendStateData {
            phase: s.phase,
            round: s.round,
            rounds: s.rounds,
            finished: s.finished,
            advancing: s.advancing,
            eliminated: s.eliminated.iter().map(KnockedOutData::from).collect(),
            damage: s.damage,
            conditions: s.conditions,
            allowed_assists: s.allowed_assists,
        }
    }
}

impl From<&KnockedOut> for KnockedOutData {
    fn from(k: &KnockedOut) -> Self {
        KnockedOutData {
            car_index: k.car_index,
            round: k.round,
            grid_position: k.grid_position,
            lap_time_ms: k.lap_time_ms,
            late: k.late,
        }
    }
}

impl From<&ShootoutState> for ShootoutStateData {
    fn from(s: &ShootoutState) -> Self {
        ShootoutStateData {
            order: s.order.clone(),
            running: s.running,
            done: s.done,
            flying: s.flying,
        }
    }
}

impl From<&PitBoxInfo> for PitBoxData {
    fn from(b: &PitBoxInfo) -> Self {
        PitBoxData {
            pit_box: b.pit_box,
            x: b.x,
            y: b.y,
            z: b.z,
            yaw_rad: b.yaw_rad,
            lateral_m: b.lateral_m,
            lane_width_m: b.lane_width_m,
            radius_m: b.radius_m,
            manual: b.manual,
        }
    }
}

impl From<&TrackCorner> for CornerData {
    fn from(c: &TrackCorner) -> Self {
        CornerData {
            number: c.number,
            name: c.name.clone(),
            entry_m: c.entry_m,
            apex_m: c.apex_m,
            exit_m: c.exit_m,
            left: c.left,
        }
    }
}

impl From<RosterRow> for RosterEntry {
    fn from(r: RosterRow) -> Self {
        RosterEntry {
            car_index: r.car_index,
            player_id: r.player_id,
            player_name: r.player_name,
            is_ai: r.is_ai,
            car_config_id: r.car_config_id,
            livery: r.livery,
            away: r.away,
            grid_slot: r.grid_slot,
            ready: r.ready,
            dnq: r.dnq,
            missed_stops: r.missed_stops,
            qualifying_laps: r.qualifying_laps,
            penalties: r.penalties.iter().map(PenaltyData::from).collect(),
            own_livery: r.own_livery,
            new_sets_left: r.new_sets_left,
            used_set_wear: r.used_set_wear,
        }
    }
}

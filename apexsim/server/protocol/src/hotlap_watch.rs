//! A watched hotlap simulated where it is watched: the session a server
//! makes for `CreateSession { session_kind: HotlapWatch }` (one AI car in the
//! asked-for car, lapping alone, `GameSession::start_hotlap_watch`), built
//! here from the same message, ticked as the server ticks it, and handed out
//! as the spectator stream a server sends a viewer of it (the catch-up, then
//! each tick's records). The physics library runs it for the client's Watch
//! hotlap, so it needs no server.
//!
//! What a watch learns is kept as the server keeps it, in a record store:
//! the ladder level of its key and the fastest lap of its track and car,
//! resumed by the next watch of them ([`LocalWatch::keep`]).
//!
//! The session's ids are seeded, so a watch with the same message, content
//! and seed is the same laps; a server's session id is random, and with it
//! the sky a create leaves to the weather.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::ai_driver::{clamp_skill, AiDriverProfile};
use crate::data::*;
use crate::game_session::{GameSession, HOTLAP_WATCH_SKILL};
use crate::network::{ClientMessage, ServerMessage};
use crate::records::{watch_level_key, RecordStore};
use crate::seeded_race::{load_car_folder, resolve_car, seeded_id, tick_field};
use crate::session_stream::SessionStream;
use crate::session_wire::SessionWire;
use crate::track_loader::{RoadContactMode, TrackLoader};

/// The stream's frame rate, as the server's live streams (`REPLAY_FRAME_HZ`).
pub const WATCH_FRAME_HZ: u16 = 30;

/// What a watch is started from.
#[derive(Debug, Clone)]
pub struct WatchOptions {
    /// The client's `CreateSession` as MessagePack (its `watch`, sky, laps
    /// and damage are read; the track and car are named below).
    pub create_session: Vec<u8>,
    pub track_path: PathBuf,
    /// The cars folder (`default/` and `custom/` under it).
    pub cars_dir: PathBuf,
    /// The car: folder, id or name.
    pub car: String,
    pub road_contact: RoadContactMode,
    pub tick_rate: u16,
    pub seed: u64,
}

/// A watched hotlap and its stream.
pub struct LocalWatch {
    pub race: GameSession,
    stream: SessionStream,
    names: HashMap<PlayerId, String>,
    /// The catch-up is sent once, after the first tick.
    started: bool,
    /// Framed records not yet taken.
    pending: Vec<u8>,
    store: Option<RecordStore>,
}

impl LocalWatch {
    /// Build the watch, resuming what `store` kept for its key (when given).
    pub fn start(opts: &WatchOptions, store: Option<RecordStore>) -> Result<Self, String> {
        let message: ClientMessage = rmp_serde::from_slice(&opts.create_session)
            .map_err(|e| format!("CreateSession: {e}"))?;
        let ClientMessage::CreateSession {
            lap_limit,
            conditions,
            damage,
            watch,
            ..
        } = message
        else {
            return Err("not a CreateSession".to_string());
        };

        let mut track = TrackLoader::load_from_file_with(&opts.track_path, opts.road_contact)
            .map_err(|e| format!("track {}: {e}", opts.track_path.display()))?;
        let source = Arc::new(track.clone());
        // As the server resolves a create's sky, from the session's id: a
        // seeded one here.
        let session_id = seeded_id(opts.seed, u64::MAX);
        let conditions = conditions.at_site(&track).resolve_at(
            conditions.sky_seed_or(session_id.as_u64_pair().0),
            track.climate_site(),
        );
        conditions.apply_to_track(&mut track);

        let (cars, folders) = load_car_folder(&opts.cars_dir)?;
        let car = resolve_car(&cars, &folders, &opts.car).ok_or_else(|| {
            format!(
                "no car named '{}' under {}",
                opts.car,
                opts.cars_dir.display()
            )
        })?;

        let mut session = RaceSession::new(
            seeded_id(opts.seed, u64::MAX - 1),
            track.id,
            SessionKind::HotlapWatch,
            1,
            1,
            lap_limit,
        );
        session.id = session_id;
        session.host_car_id = Some(car);
        session.conditions = conditions;
        session.ai_skill = Some(clamp_skill(HOTLAP_WATCH_SKILL));

        let mut driver = AiDriverProfile::new("Hotlap", HOTLAP_WATCH_SKILL).with_car(car);
        driver.id = seeded_id(opts.seed, 0);
        driver.exact_line = true;
        let corners = crate::track_guide::wire_corners(&track, Some(&opts.track_path));

        let mut race = GameSession::with_ai_profiles(session, track, cars, vec![driver]);
        race.set_tick_rate(opts.tick_rate.max(1));
        race.set_source_track(source);
        race.spawn_ai_drivers();
        race.set_track_corners(corners);
        race.start_hotlap_watch();
        race.set_damage(damage);
        if let Some(setup) = watch.and_then(|w| w.setup) {
            race.set_hotlap_watch_setup(setup);
        }
        if race.session.participants.is_empty() {
            return Err("the car could not be seated".to_string());
        }
        // Where earlier watches of this key left off.
        if let (Some(store), Some(key)) = (&store, race.hotlap_watch_key()) {
            if let Some(level) = store.watch_level(&watch_level_key(&key)) {
                race.set_hotlap_watch_level(level);
            }
            if let Some(ghost) = store.watch_ghost(key.0, key.1) {
                race.set_hotlap_watch_ghost(ghost);
            }
        }

        let names = HashMap::new();
        let stream = SessionStream::new(&race, WATCH_FRAME_HZ, &names);
        Ok(Self {
            race,
            stream,
            names,
            started: false,
            pending: Vec::new(),
            store,
        })
    }

    /// Tick `ticks` times, adding each tick's records (after the first
    /// tick: the catch-up a newcomer gets) to the ones not yet taken.
    pub fn tick(&mut self, ticks: u32) {
        for _ in 0..ticks {
            tick_field(&mut self.race);
            let laps = self.race.take_lap_events();
            let pits = self.race.take_pit_events();
            let roster_changed = self.race.roster_is_dirty();
            let out = self
                .stream
                .tick(&self.race, &laps, &pits, &self.names, roster_changed);
            if self.started {
                for record in out.records {
                    frame_into(&mut self.pending, &record.body);
                }
            } else {
                // As a server's newcomer: the catch-up taken after the tick
                // in which the stream began, not that tick's records.
                for body in self.stream.catch_up(&self.race) {
                    frame_into(&mut self.pending, &body);
                }
                self.started = true;
            }
        }
    }

    /// The framed records (`[u32 big-endian length][body]`, back to back)
    /// since the last take.
    pub fn take(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.pending)
    }

    /// Bytes waiting to be taken.
    pub fn pending_len(&self) -> usize {
        self.pending.len()
    }

    /// The `TrackCorners` message a server sends a viewer before the
    /// stream, as MessagePack; `None` for a track with no corners.
    pub fn corners_message(&self) -> Option<Vec<u8>> {
        let message: ServerMessage = self.race.track_corners_message(self.race.session.id)?;
        rmp_serde::to_vec_named(&message).ok()
    }

    /// Keep what the watch learnt in its store (the server's
    /// `keep_hotlap_watch`): the ladder level of its key, and its ghost when
    /// the fastest yet for its track and car.
    pub fn keep(&self) {
        let (Some(store), Some(key)) = (&self.store, self.race.hotlap_watch_key()) else {
            return;
        };
        store.keep_watch_level(watch_level_key(&key), self.race.hotlap_watch_level());
        if let Some((_, ghost)) = self.race.hotlap_watch_ghost() {
            store.keep_watch_ghost(key.0, key.1, ghost);
        }
    }
}

/// A record body framed as on the wire.
fn frame_into(out: &mut Vec<u8>, body: &[u8]) {
    out.extend_from_slice(&(body.len() as u32).to_be_bytes());
    out.extend_from_slice(body);
}

/// The record store a client keeps its watches in.
pub fn open_store(dir: &Path) -> RecordStore {
    RecordStore::open(dir)
}

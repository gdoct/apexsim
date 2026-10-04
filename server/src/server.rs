//! Server assembly: owns `ServerState`, the 240Hz game loop, and the
//! `run_server` entry point used by both the binary and integration tests.

use crate::{
    car_loader::CarLoader, config::ServerConfig, data::*, game_session::GameSession,
    health::HealthState, lobby::LobbyManager, metrics::ServerMetrics, replay::ReplayManager,
    track_content::TrackContent, track_loader::TrackLoader, transport::TransportLayer,
};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::sync::RwLock;
use tracing::{debug, info, warn};

pub struct ServerState {
    pub config: ServerConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
    /// Every track as the lobby shows it, without its sidecars (ground,
    /// curbs, walls, pit lane, road mesh): `track_content` loads those the
    /// first time a session needs them.
    pub track_configs: HashMap<TrackConfigId, TrackConfig>,
    /// The tracks with their sidecars, loaded on first use. Behind an `Arc`
    /// so the game loop can load one off the loop, without the state lock.
    pub track_content: Arc<TrackContent>,
    pub sessions: HashMap<SessionId, GameSession>,
    pub players: HashMap<PlayerId, Player>,
    pub lobby: LobbyManager,
    pub replay: ReplayManager,
    /// Lap records across sessions (`crate::records`). Behind an `Arc` so
    /// the game loop can write a record after dropping the state lock: the
    /// store does its own locking and its own disk IO.
    pub records: Arc<crate::records::RecordStore>,
    /// Rendered races played to clients in the menu (`crate::showcase`).
    pub showcase: crate::showcase::ShowcaseState,
}

impl ServerState {
    pub fn new(config: ServerConfig) -> Self {
        let mut car_configs = HashMap::new();
        let mut track_configs = HashMap::new();

        // Load custom cars from configured directory
        let cars_dir = config.content.cars_dir.clone();
        debug!("Loading cars from {}...", cars_dir);
        Self::load_custom_cars(&mut car_configs, &cars_dir);

        if car_configs.is_empty() {
            warn!("No cars loaded! Creating default car.");
            let default_car = CarConfig::default();
            car_configs.insert(default_car.id, default_car);
        } else {
            debug!("Loaded {} car(s):", car_configs.len());
            for car in car_configs.values() {
                debug!("  - {} (ID: {})", car.name, car.id);
            }
        }

        // Load custom tracks from configured directory
        let tracks_dir = config.content.tracks_dir.clone();
        debug!("Loading tracks from {}...", tracks_dir);
        let mut track_content = TrackContent::new(config.physics.road_contact);
        Self::load_custom_tracks(
            &mut track_configs,
            &mut track_content,
            &tracks_dir,
            config.content.skip_imported_tracks,
        );

        if track_configs.is_empty() {
            warn!("No tracks loaded! Server will not be able to create sessions.");
        } else {
            debug!("Loaded {} track(s):", track_configs.len());
            for track in track_configs.values() {
                debug!("  - {} (ID: {})", track.name, track.id);
            }
        }

        let records = if config.records.enabled {
            crate::records::RecordStore::open(&config.records.dir)
        } else {
            crate::records::RecordStore::in_memory()
        };

        let showcase =
            crate::showcase::ShowcaseState::load(&config.showcase, &car_configs, &track_configs);

        Self {
            showcase,
            config,
            car_configs,
            track_configs,
            track_content: Arc::new(track_content),
            sessions: HashMap::new(),
            players: HashMap::new(),
            lobby: LobbyManager::new(),
            replay: ReplayManager::new(std::path::PathBuf::from("./replays")),
            records: Arc::new(records),
        }
    }

    /// Every track file under the folder as a catalog entry (no sidecars:
    /// `TrackContent` loads those when a session first needs them), read in
    /// parallel and kept in the walk's order, so which of two tracks
    /// claiming one id is kept does not depend on which parsed first.
    fn load_custom_tracks(
        track_configs: &mut HashMap<TrackConfigId, TrackConfig>,
        track_content: &mut TrackContent,
        tracks_dir_str: &str,
        skip_imported: bool,
    ) {
        let tracks_dir = Path::new(tracks_dir_str);

        // Content root is the parent of the tracks directory (e.g., ../content)
        let content_root = tracks_dir.parent().unwrap_or(tracks_dir);

        if !tracks_dir.exists() {
            warn!(
                "Tracks directory not found at {:?}, skipping custom track loading",
                tracks_dir
            );
            return;
        }

        let mut files = Vec::new();
        Self::collect_track_files(tracks_dir, skip_imported, &mut files);
        let loaded = Self::load_catalog_entries(&files);
        for (path, result) in files.into_iter().zip(loaded) {
            match result {
                Ok(mut track) => {
                    // Compute relative path from content root, normalize to forward slashes
                    let rel = path.strip_prefix(content_root).unwrap_or(&path);
                    let rel_norm = rel.to_string_lossy().replace('\\', "/");
                    track.source_path = Some(rel_norm);
                    if let Some(kept) = track_configs.get(&track.id) {
                        warn!(
                            "Track {:?} has the same track_id as {:?}; ignoring it \
                             (give a custom track its own track_id)",
                            track.source_path.as_deref().unwrap_or_default(),
                            kept.source_path.as_deref().unwrap_or_default()
                        );
                        continue;
                    }
                    track_content.register(track.id, path);
                    track_configs.insert(track.id, track);
                }
                Err(e) => {
                    warn!("Failed to load track from {:?}: {}", path, e);
                }
            }
        }
    }

    /// Parse the track files on every core, each result at its file's index.
    fn load_catalog_entries(
        paths: &[PathBuf],
    ) -> Vec<Result<TrackConfig, crate::track_loader::TrackLoadError>> {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let next = AtomicUsize::new(0);
        let threads = std::thread::available_parallelism()
            .map_or(1, |n| n.get())
            .clamp(1, paths.len().max(1));
        let mut results: Vec<Option<_>> = (0..paths.len()).map(|_| None).collect();
        std::thread::scope(|scope| {
            let workers: Vec<_> = (0..threads)
                .map(|_| {
                    scope.spawn(|| {
                        let mut parsed = Vec::new();
                        loop {
                            let i = next.fetch_add(1, Ordering::Relaxed);
                            let Some(path) = paths.get(i) else {
                                break;
                            };
                            parsed.push((i, TrackLoader::load_catalog_entry(path)));
                        }
                        parsed
                    })
                })
                .collect();
            for worker in workers {
                for (i, result) in worker.join().expect("a track loader thread panicked") {
                    results[i] = Some(result);
                }
            }
        });
        results
            .into_iter()
            .map(|r| r.expect("every track file was parsed"))
            .collect()
    }

    /// A track the AC importer wrote: its report sits beside the YAML.
    fn is_imported_track(path: &std::path::Path) -> bool {
        let (Some(dir), Some(stem)) = (path.parent(), path.file_stem()) else {
            return false;
        };
        dir.join(format!("{}.import.json", stem.to_string_lossy()))
            .is_file()
    }

    /// Real-world layout dossiers (`<Stem>.layout.json`, see CLAUDE.md
    /// "Real-world layouts") and the AC importer's report (`<Stem>.import.json`,
    /// see "Assetto Corsa track import") sit beside the track YAML but are not
    /// track configs, so trying to parse one as a track would only log a
    /// warning. (The client exports and the download caches used to live
    /// under the tracks directory too; they are in `build/tracks` and
    /// `.cache` now.)
    fn is_non_track_json(path: &std::path::Path) -> bool {
        let name = path
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or_default();
        let name = name.to_ascii_lowercase();
        name.ends_with(".layout.json") || name.ends_with(".import.json")
    }

    fn collect_track_files(dir: &Path, skip_imported: bool, files: &mut Vec<PathBuf>) {
        match std::fs::read_dir(dir) {
            Ok(entries) => {
                // Sorted, so which of two tracks claiming one id is kept does
                // not depend on the filesystem, and `custom/` last: the first
                // one loaded wins, so a player's track can never replace a
                // shipped one by reusing its id.
                let mut paths: Vec<_> = entries.filter_map(|e| e.ok()).map(|e| e.path()).collect();
                paths.sort_by_key(|p| {
                    let custom = p
                        .file_name()
                        .is_some_and(|n| n.eq_ignore_ascii_case("custom"));
                    (custom, p.clone())
                });
                for path in paths {
                    if path.is_dir() {
                        // Recursively load tracks from subdirectories.
                        if !Self::is_non_track_json(&path) {
                            Self::collect_track_files(&path, skip_imported, files);
                        }
                    } else if path.is_file() && !Self::is_non_track_json(&path) {
                        let ext = path.extension().and_then(|s| s.to_str());
                        if skip_imported && Self::is_imported_track(&path) {
                            debug!("Skipping imported track {:?}", path);
                            continue;
                        }
                        if ext == Some("json") || ext == Some("yaml") || ext == Some("yml") {
                            files.push(path);
                        }
                    }
                }
            }
            Err(e) => {
                warn!("Failed to read tracks directory {:?}: {}", dir, e);
            }
        }
    }

    fn load_custom_cars(car_configs: &mut HashMap<CarConfigId, CarConfig>, cars_dir_str: &str) {
        let cars_dir = std::path::Path::new(cars_dir_str);

        if !cars_dir.exists() {
            warn!(
                "Cars directory not found at {:?}, skipping custom car loading",
                cars_dir
            );
            return;
        }

        Self::load_cars_recursive(car_configs, cars_dir);
    }

    /// Every car under the folder (`crate::car_loader::car_toml_paths`:
    /// `default/` then `custom/`, sorted), the first to claim an id kept, so
    /// a player's car can never replace a shipped one by reusing its id.
    fn load_cars_recursive(
        car_configs: &mut HashMap<CarConfigId, CarConfig>,
        dir: &std::path::Path,
    ) {
        let mut sources: HashMap<CarConfigId, std::path::PathBuf> = HashMap::new();
        for path in crate::car_loader::car_toml_paths(dir) {
            match CarLoader::load_from_file(&path) {
                Ok(car) => {
                    if let Some(kept) = sources.get(&car.id) {
                        warn!(
                            "Car {:?} has the same id as {:?}; ignoring it                              (give a custom car its own id)",
                            path, kept
                        );
                        continue;
                    }
                    sources.insert(car.id, path);
                    car_configs.insert(car.id, car);
                }
                Err(e) => {
                    warn!("Failed to load car from {:?}: {}", path, e);
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn create_session(
        &mut self,
        host_player_id: PlayerId,
        host_car_id: CarConfigId,
        track_config_id: TrackConfigId,
        session_kind: SessionKind,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
        allowed_assists: AllowedAssists,
        conditions: SessionConditions,
        ai_skill: Option<u8>,
    ) -> Option<SessionId> {
        use crate::ai_driver::{clamp_skill, generate_ai_profiles};

        if self.sessions.len() >= self.config.server.max_sessions as usize {
            return None;
        }

        // The track with its sidecars: the game loop has loaded them off the
        // loop by now (`game_loop::track_loads`); anyone else waits here.
        let catalog = self.track_configs.get(&track_config_id)?;
        let mut track = (*self.track_content.complete(catalog)).clone();
        let mut session = RaceSession::new(
            host_player_id,
            track_config_id,
            session_kind,
            max_players,
            ai_count,
            lap_limit,
        );
        // Every figure of the air named (what the host left to the weather
        // worked out now, the wind's direction from the session's id), so
        // the session echoes and lists what it simulates.
        let conditions = conditions.resolve(session.id.as_u64_pair().0);
        // The session's own copy of the track carries the weather's grip,
        // the air's density and the wind: one bake here, no branch in the
        // tick.
        conditions.apply_to_track(&mut track);
        session.host_car_id = Some(host_car_id);
        session.allowed_assists = allowed_assists;
        session.conditions = conditions;
        session.ai_skill = ai_skill.map(clamp_skill);
        let session_id = session.id;

        // Create AI profiles if AI count is specified
        // No preferred cars: the session deals the field from the host car's
        // class (`class_field`), and the roster tells clients who drives what.
        let ai_profiles = if ai_count > 0 {
            generate_ai_profiles(ai_count, session.ai_skill)
        } else {
            Vec::new()
        };

        // Create game session with AI profiles
        let mut game_session = if !ai_profiles.is_empty() {
            GameSession::with_ai_profiles(session, track, self.car_configs.clone(), ai_profiles)
        } else {
            GameSession::new(session, track, self.car_configs.clone())
        };

        // Run the session at the configured server tick rate
        game_session.set_tick_rate(self.config.server.tick_rate_hz);

        // Spawn AI drivers immediately
        if ai_count > 0 {
            game_session.spawn_ai_drivers();
            debug!("Spawned {} AI drivers for session {}", ai_count, session_id);
        }

        self.sessions.insert(session_id, game_session);

        Some(session_id)
    }
}

/// Handle to a running server instance. Keeps the shared state and transport
/// reachable (e.g. for tests) and reports the actually-bound addresses so the
/// server can be started on ephemeral ports (bind to ":0").
pub struct ServerHandle {
    pub tcp_addr: SocketAddr,
    pub udp_addr: SocketAddr,
    pub health_addr: SocketAddr,
    pub state: Arc<RwLock<ServerState>>,
    pub transport: Arc<RwLock<TransportLayer>>,
    pub health: HealthState,
    pub metrics: Arc<ServerMetrics>,
}

impl ServerHandle {
    /// Notify clients and stop the transport layer. Background tasks are
    /// detached and stop on their own once the runtime shuts down. Only a
    /// transport READ lock is held (shutdown takes `&self`), so the game
    /// loop is never blocked during the notification grace period.
    pub async fn shutdown(&self) {
        self.health.set_healthy(false).await;
        self.transport.read().await.shutdown().await;
    }
}

/// Boot the server: load content, bind sockets, start the health endpoint and
/// the game loop. Returns once everything is running.
pub async fn run_server(config: ServerConfig) -> Result<ServerHandle, Box<dyn std::error::Error>> {
    config
        .validate()
        .map_err(|e| format!("Invalid configuration: {}", e))?;

    let state = Arc::new(RwLock::new(ServerState::new(config.clone())));

    info!(
        "Server initialized with {} car configs and {} track configs",
        state.read().await.car_configs.len(),
        state.read().await.track_configs.len()
    );

    let mut transport = TransportLayer::new(
        &config.network.tcp_bind,
        &config.network.udp_bind,
        &config.network.tls_cert_path,
        &config.network.tls_key_path,
        config.network.require_tls,
        config.network.heartbeat_timeout_ms,
        config.auth.clone(),
    )
    .await
    .map_err(|e| format!("Failed to initialize transport layer: {}", e))?;

    let metrics = ServerMetrics::new(transport.metrics.clone());

    let health_state = HealthState::new();
    let health_listener = tokio::net::TcpListener::bind(&config.network.health_bind)
        .await
        .map_err(|e| {
            format!(
                "Failed to bind health endpoint {}: {}",
                config.network.health_bind, e
            )
        })?;
    let health_addr = health_listener.local_addr()?;
    let health_state_clone = health_state.clone();
    let health_metrics = Arc::clone(&metrics);
    tokio::spawn(async move {
        if let Err(e) =
            crate::health::serve_health(health_listener, health_state_clone, Some(health_metrics))
                .await
        {
            warn!("Health server error: {}", e);
        }
    });

    transport.start().await;
    let tcp_addr = transport.tcp_local_addr();
    let udp_addr = transport.udp_local_addr();
    let transport = Arc::new(RwLock::new(transport));

    health_state.set_ready(true).await;

    let loop_state = Arc::clone(&state);
    let loop_transport = Arc::clone(&transport);
    let loop_metrics = Arc::clone(&metrics);
    let tick_rate = config.server.tick_rate_hz;
    tokio::spawn(async move {
        crate::game_loop::run_game_loop(loop_state, loop_transport, tick_rate, loop_metrics).await;
    });

    info!("Server is running (TCP {}, UDP {})", tcp_addr, udp_addr);

    Ok(ServerHandle {
        tcp_addr,
        udp_addr,
        health_addr,
        state,
        transport,
        health: health_state,
        metrics,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    #[test]
    fn test_server_state_creation() {
        let config = ServerConfig::default();
        let state = ServerState::new(config);

        assert!(!state.car_configs.is_empty());
        assert!(!state.track_configs.is_empty());
        assert_eq!(state.sessions.len(), 0);
    }

    #[test]
    fn dossiers_and_import_reports_are_not_tracks() {
        let is = |name: &str| ServerState::is_non_track_json(std::path::Path::new(name));
        assert!(is("custom/KsZandvoort.import.json"));
        assert!(is("default/Spa.layout.json"));
        assert!(is("default/Spa.Layout.JSON"));
        assert!(!is("custom/Mine.json"));
        assert!(!is("custom/Mine.yaml"));
    }

    /// `content/tracks/custom` is read after `default` whatever the
    /// filesystem's order, so a custom track reusing a shipped track's id is
    /// the one left out; one with its own id loads beside it.
    #[test]
    fn custom_tracks_load_beside_the_default_ones_and_never_replace_them() {
        let root = tempfile::tempdir().unwrap();
        let tracks = root.path().join("tracks");
        let write = |folder: &str, stem: &str, id: &str| {
            let dir = tracks.join(folder);
            std::fs::create_dir_all(&dir).unwrap();
            std::fs::write(
                dir.join(format!("{stem}.yaml")),
                format!(
                    "name: {stem}\ntrack_id: {id}\nnodes:\n  - {{ x: 0.0, y: 0.0 }}\n  \
                     - {{ x: 100.0, y: 0.0 }}\n  - {{ x: 100.0, y: 100.0 }}\n  \
                     - {{ x: 0.0, y: 100.0 }}\ndefault_width: 10.0\nclosed_loop: true\n"
                ),
            )
            .unwrap();
        };
        let shared = "6f1c2c4e-3b1f-4c8a-9d57-1a2b3c4d5e6f";
        write("default", "Shipped", shared);
        write("custom", "Clash", shared);
        write("custom", "Mine", "0d9f8e7c-6b5a-4f3e-8d2c-1b0a9f8e7d6c");

        let mut configs = HashMap::new();
        let mut content = TrackContent::new(crate::config::RoadContactMode::Centerline);
        ServerState::load_custom_tracks(
            &mut configs,
            &mut content,
            tracks.to_str().unwrap(),
            false,
        );
        for id in configs.keys() {
            assert!(content.needs_loading(*id), "each kept track is registered");
        }

        let mut sources: Vec<String> = configs
            .values()
            .map(|t| t.source_path.clone().unwrap_or_default())
            .collect();
        sources.sort();
        assert_eq!(
            sources,
            vec!["tracks/custom/Mine.yaml", "tracks/default/Shipped.yaml"]
        );

        // An imported track (its report beside the YAML) is left out when
        // asked, as the debug-build test servers do; the rest still load.
        std::fs::write(tracks.join("custom/Mine.import.json"), "{}").unwrap();
        let mut configs = HashMap::new();
        let mut content = TrackContent::new(crate::config::RoadContactMode::Centerline);
        ServerState::load_custom_tracks(&mut configs, &mut content, tracks.to_str().unwrap(), true);
        let sources: Vec<String> = configs
            .values()
            .map(|t| t.source_path.clone().unwrap_or_default())
            .collect();
        assert_eq!(sources, vec!["tracks/default/Shipped.yaml"]);
    }

    /// The same rule for cars: `content/cars/custom` is read after `default`
    /// (whatever the names sort to inside them), so a custom car reusing a
    /// shipped id is the one left out, and one with its own id loads beside
    /// it. A folder of cars with neither subfolder is still read as it is.
    #[test]
    fn custom_cars_load_beside_the_default_ones_and_never_replace_them() {
        let write = |root: &std::path::Path, folder: &str, name: &str, id: &str| {
            let dir = root.join(folder);
            std::fs::create_dir_all(&dir).unwrap();
            std::fs::write(
                dir.join("car.toml"),
                format!(
                    "id = \"{id}\"\nname = \"{name}\"\nversion = \"1.0.0\"\nmodel = \"m.glb\"\n\n\
                     [physics]\nmass_kg = 1000.0\nmax_engine_force_n = 5000.0\n\
                     max_brake_force_n = 10000.0\ndrag_coefficient = 0.3\n\
                     grip_coefficient = 1.0\nmax_steering_angle_rad = 0.5\nwheelbase_m = 2.6\n"
                ),
            )
            .unwrap();
        };
        let names = |root: &std::path::Path| {
            let mut configs = HashMap::new();
            ServerState::load_custom_cars(&mut configs, root.to_str().unwrap());
            let mut names: Vec<String> = configs.values().map(|c| c.name.clone()).collect();
            names.sort();
            names
        };

        let split = tempfile::tempdir().unwrap();
        let shared = "6f1c2c4e-3b1f-4c8a-9d57-1a2b3c4d5e6f";
        // `custom/aaa` sorts before `default/zzz`: the folder order decides, not the name.
        write(split.path(), "default/zzz-shipped", "Shipped", shared);
        write(split.path(), "custom/aaa-clash", "Clash", shared);
        write(
            split.path(),
            "custom/mine",
            "Mine",
            "0d9f8e7c-6b5a-4f3e-8d2c-1b0a9f8e7d6c",
        );
        assert_eq!(names(split.path()), vec!["Mine", "Shipped"]);

        let flat = tempfile::tempdir().unwrap();
        write(flat.path(), "old", "Old", shared);
        assert_eq!(names(flat.path()), vec!["Old"]);
    }

    #[test]
    fn test_create_session() {
        let config = ServerConfig::default();
        let mut state = ServerState::new(config);

        let host_id = Uuid::new_v4();
        let track_id = state.track_configs.values().next().unwrap().id;
        let car_id = state.car_configs.values().next().unwrap().id;

        let session_id = state.create_session(
            host_id,
            car_id,
            track_id,
            SessionKind::Practice,
            8,
            2,
            5,
            AllowedAssists::ALL,
            SessionConditions::DEFAULT,
            Some(95),
        );

        let session_id = session_id.expect("session created");
        assert_eq!(state.sessions.len(), 1);
        // The host's level: the field round it, not novice to ace.
        let session = &state.sessions[&session_id];
        assert_eq!(session.session.ai_skill, Some(95));
        let skills: Vec<u8> = session
            .ai_profiles
            .values()
            .map(|p| p.skill_level)
            .collect();
        assert_eq!(skills.len(), 2);
        assert!(skills.iter().all(|s| (93..=97).contains(s)), "{skills:?}");
    }

    #[test]
    fn test_max_sessions_limit() {
        let config = ServerConfig::default();
        let mut state = ServerState::new(config);
        state.config.server.max_sessions = 2;

        let host_id = Uuid::new_v4();
        let track_id = state.track_configs.values().next().unwrap().id;
        let car_id = state.car_configs.values().next().unwrap().id;

        // Create max sessions
        for _ in 0..2 {
            let result = state.create_session(
                host_id,
                car_id,
                track_id,
                SessionKind::Sandbox,
                8,
                0,
                3,
                AllowedAssists::ALL,
                SessionConditions::DEFAULT,
                None,
            );
            assert!(result.is_some());
        }

        // Try to create one more
        let result = state.create_session(
            host_id,
            car_id,
            track_id,
            SessionKind::Multiplayer,
            8,
            0,
            3,
            AllowedAssists::ALL,
            SessionConditions::DEFAULT,
            None,
        );
        assert!(result.is_none());
    }
}

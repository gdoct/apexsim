//! Server assembly: owns `ServerState`, the 240Hz game loop, and the
//! `run_server` entry point used by both the binary and integration tests.

use crate::{
    car_loader::CarLoader, config::ServerConfig, data::*, game_session::GameSession,
    health::HealthState, lobby::LobbyManager, metrics::ServerMetrics, replay::ReplayManager,
    track_loader::TrackLoader, transport::TransportLayer,
};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::sync::RwLock;
use tracing::{debug, info, warn};

pub struct ServerState {
    pub config: ServerConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
    pub track_configs: HashMap<TrackConfigId, TrackConfig>,
    pub sessions: HashMap<SessionId, GameSession>,
    pub players: HashMap<PlayerId, Player>,
    pub lobby: LobbyManager,
    pub replay: ReplayManager,
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
        Self::load_custom_tracks(&mut track_configs, &tracks_dir);

        if track_configs.is_empty() {
            warn!("No tracks loaded! Server will not be able to create sessions.");
        } else {
            debug!("Loaded {} track(s):", track_configs.len());
            for track in track_configs.values() {
                debug!("  - {} (ID: {})", track.name, track.id);
            }
        }

        Self {
            config,
            car_configs,
            track_configs,
            sessions: HashMap::new(),
            players: HashMap::new(),
            lobby: LobbyManager::new(),
            replay: ReplayManager::new(std::path::PathBuf::from("./replays")),
        }
    }

    fn load_custom_tracks(
        track_configs: &mut HashMap<TrackConfigId, TrackConfig>,
        tracks_dir_str: &str,
    ) {
        let tracks_dir = std::path::Path::new(tracks_dir_str);

        // Content root is the parent of the tracks directory (e.g., ../content)
        let content_root = tracks_dir.parent().unwrap_or(tracks_dir);

        if !tracks_dir.exists() {
            warn!(
                "Tracks directory not found at {:?}, skipping custom track loading",
                tracks_dir
            );
            return;
        }

        Self::load_tracks_recursive(track_configs, tracks_dir, content_root);
    }

    fn load_tracks_recursive(
        track_configs: &mut HashMap<TrackConfigId, TrackConfig>,
        dir: &std::path::Path,
        content_root: &std::path::Path,
    ) {
        match std::fs::read_dir(dir) {
            Ok(entries) => {
                for entry in entries.filter_map(|e| e.ok()) {
                    let path = entry.path();
                    if path.is_dir() {
                        // Recursively load tracks from subdirectories
                        Self::load_tracks_recursive(track_configs, &path, content_root);
                    } else if path.is_file() {
                        let ext = path.extension().and_then(|s| s.to_str());
                        if ext == Some("json") || ext == Some("yaml") || ext == Some("yml") {
                            match TrackLoader::load_from_file(&path) {
                                Ok(mut track) => {
                                    // Compute relative path from content root, normalize to forward slashes
                                    let rel = path.strip_prefix(content_root).unwrap_or(&path);
                                    let rel_norm = rel.to_string_lossy().replace('\\', "/");
                                    track.source_path = Some(rel_norm);
                                    track_configs.insert(track.id, track);
                                }
                                Err(e) => {
                                    warn!("Failed to load track from {:?}: {}", path, e);
                                }
                            }
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

    fn load_cars_recursive(
        car_configs: &mut HashMap<CarConfigId, CarConfig>,
        dir: &std::path::Path,
    ) {
        match std::fs::read_dir(dir) {
            Ok(entries) => {
                for entry in entries.filter_map(|e| e.ok()) {
                    let path = entry.path();
                    if path.is_dir() {
                        // Recursively load cars from subdirectories
                        Self::load_cars_recursive(car_configs, &path);
                    } else if path.is_file() {
                        let ext = path.extension().and_then(|s| s.to_str());
                        if ext == Some("toml") {
                            // Check if this is a car.toml file
                            if path.file_name().and_then(|s| s.to_str()) == Some("car.toml") {
                                match CarLoader::load_from_file(&path) {
                                    Ok(car) => {
                                        car_configs.insert(car.id, car);
                                    }
                                    Err(e) => {
                                        warn!("Failed to load car from {:?}: {}", path, e);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Err(e) => {
                warn!("Failed to read cars directory {:?}: {}", dir, e);
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
    ) -> Option<SessionId> {
        use crate::ai_driver::generate_default_ai_profiles;

        if self.sessions.len() >= self.config.server.max_sessions as usize {
            return None;
        }

        let track = self.track_configs.get(&track_config_id)?.clone();
        let mut session = RaceSession::new(
            host_player_id,
            track_config_id,
            session_kind,
            max_players,
            ai_count,
            lap_limit,
        );
        session.host_car_id = Some(host_car_id);
        let session_id = session.id;

        // Create AI profiles if AI count is specified
        let ai_profiles = if ai_count > 0 {
            generate_default_ai_profiles(ai_count)
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
    fn test_create_session() {
        let config = ServerConfig::default();
        let mut state = ServerState::new(config);

        let host_id = Uuid::new_v4();
        let track_id = state.track_configs.values().next().unwrap().id;
        let car_id = state.car_configs.values().next().unwrap().id;

        let session_id =
            state.create_session(host_id, car_id, track_id, SessionKind::Practice, 8, 2, 5);

        assert!(session_id.is_some());
        assert_eq!(state.sessions.len(), 1);
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
            let result =
                state.create_session(host_id, car_id, track_id, SessionKind::Sandbox, 8, 0, 3);
            assert!(result.is_some());
        }

        // Try to create one more
        let result =
            state.create_session(host_id, car_id, track_id, SessionKind::Multiplayer, 8, 0, 3);
        assert!(result.is_none());
    }
}

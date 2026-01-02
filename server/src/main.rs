use apexsim_server::{
    car_loader::CarLoader,
    config::ServerConfig,
    data::*,
    game_session::GameSession,
    health::{HealthState, run_health_server},
    lobby::LobbyManager,
    replay::ReplayManager,
    track_loader::TrackLoader,
    transport::TransportLayer,
};
use clap::Parser;
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;
use tokio::time::{interval, Duration};
use tracing::{debug, info, warn};

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Path to server.toml configuration file
    #[arg(short, long, default_value = "./server.toml")]
    config: String,

    /// Override log level (trace|debug|info|warn|error)
    #[arg(short, long)]
    log_level: Option<String>,
}

struct ServerState {
    config: ServerConfig,
    car_configs: HashMap<CarConfigId, CarConfig>,
    track_configs: HashMap<TrackConfigId, TrackConfig>,
    sessions: HashMap<SessionId, GameSession>,
    players: HashMap<PlayerId, Player>,
    lobby: LobbyManager,
    replay: ReplayManager,
}

impl ServerState {
    fn new(config: ServerConfig) -> Self {
        let mut car_configs = HashMap::new();
        let mut track_configs = HashMap::new();

        // Load custom cars from configured directory
        let cars_dir = config.content.cars_dir.clone();
        info!("Loading cars from {}...", cars_dir);
        Self::load_custom_cars(&mut car_configs, &cars_dir);

        if car_configs.is_empty() {
            warn!("No cars loaded! Creating default car.");
            let default_car = CarConfig::default();
            car_configs.insert(default_car.id, default_car);
        } else {
            info!("Loaded {} car(s):", car_configs.len());
            for car in car_configs.values() {
                info!("  - {} (ID: {})", car.name, car.id);
            }
        }

        // Load custom tracks from configured directory
        let tracks_dir = config.content.tracks_dir.clone();
        info!("Loading tracks from {}...", tracks_dir);
        Self::load_custom_tracks(&mut track_configs, &tracks_dir);

        if track_configs.is_empty() {
            warn!("No tracks loaded! Server will not be able to create sessions.");
        } else {
            info!("Loaded {} track(s):", track_configs.len());
            for track in track_configs.values() {
                info!("  - {} (ID: {})", track.name, track.id);
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

    fn load_custom_tracks(track_configs: &mut HashMap<TrackConfigId, TrackConfig>, tracks_dir_str: &str) {
        let tracks_dir = std::path::Path::new(tracks_dir_str);

        // Content root is the parent of the tracks directory (e.g., ../content)
        let content_root = tracks_dir.parent().unwrap_or(tracks_dir);

        if !tracks_dir.exists() {
            info!("Tracks directory not found at {:?}, skipping custom track loading", tracks_dir);
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
            info!("Cars directory not found at {:?}, skipping custom car loading", cars_dir);
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

    #[allow(dead_code)]
    fn create_session(
        &mut self,
        host_player_id: PlayerId,
        track_config_id: TrackConfigId,
        session_kind: SessionKind,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    ) -> Option<SessionId> {
        if self.sessions.len() >= self.config.server.max_sessions as usize {
            return None;
        }

        let track = self.track_configs.get(&track_config_id)?.clone();
        let session = RaceSession::new(host_player_id, track_config_id, session_kind, max_players, ai_count, lap_limit);
        let session_id = session.id;

        let game_session = GameSession::new(session, track, self.car_configs.clone());
        self.sessions.insert(session_id, game_session);

        Some(session_id)
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    // Initialize tracing
    let log_level = args
        .log_level
        .as_deref()
        .unwrap_or("info");

    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new(log_level)),
        )
        .init();

    info!("Starting ApexSim Racing Server v0.1.0");

    // Load configuration
    let config = ServerConfig::load_or_default(&args.config);
    info!("Configuration loaded from: {}", args.config);
    info!("TCP bind: {}", config.network.tcp_bind);
    info!("UDP bind: {}", config.network.udp_bind);
    info!("Tick rate: {}Hz", config.server.tick_rate_hz);

    // Initialize server state
    let state = Arc::new(RwLock::new(ServerState::new(config.clone())));

    info!("Server initialized with {} car configs and {} track configs",
        state.read().await.car_configs.len(),
        state.read().await.track_configs.len());

    // Initialize health state
    let health_state = HealthState::new();

    // Start health check server
    let health_bind = config.network.health_bind.clone();
    let health_state_clone = health_state.clone();
    tokio::spawn(async move {
        if let Err(e) = run_health_server(health_bind, health_state_clone).await {
            warn!("Health server error: {}", e);
        }
    });

    // Initialize transport layer
    let mut transport = match TransportLayer::new(
        &config.network.tcp_bind,
        &config.network.udp_bind,
        &config.network.tls_cert_path,
        &config.network.tls_key_path,
        config.network.require_tls,
        config.network.heartbeat_timeout_ms,
    ).await {
        Ok(t) => {
            info!("Transport layer initialized successfully");
            t
        }
        Err(e) => {
            return Err(format!("Failed to initialize transport layer: {}", e).into());
        }
    };

    // Start transport layer
    transport.start().await;
    let transport = Arc::new(RwLock::new(transport));

    // Mark server as ready
    health_state.set_ready(true).await;
    info!("Server marked as ready");

    // Start main game loop
    let loop_state = Arc::clone(&state);
    let loop_transport = Arc::clone(&transport);
    let tick_rate = config.server.tick_rate_hz;

    tokio::spawn(async move {
        run_game_loop(loop_state, loop_transport, tick_rate).await;
    });

    info!("Server is running. Press Ctrl+C to stop.");

    // Wait for shutdown signal
    tokio::signal::ctrl_c().await?;

    info!("Shutdown signal received. Cleaning up...");

    // Mark server as unhealthy
    health_state.set_healthy(false).await;

    // Shutdown transport layer (notifies all clients)
    transport.write().await.shutdown().await;

    // Cleanup
    let final_state = state.read().await;
    info!("Server shutting down with {} active sessions", final_state.sessions.len());

    Ok(())
}

/// Send lobby state to a specific connection
async fn send_lobby_state(
    connection_id: ConnectionId,
    state: &Arc<RwLock<ServerState>>,
    transport: &TransportLayer,
) -> Result<(), Box<dyn std::error::Error>> {
    use apexsim_server::network::{ServerMessage, CarConfigSummary, TrackConfigSummary};

    let state_read = state.read().await;

    // Get lobby players and sessions
    let players_in_lobby = state_read.lobby.get_lobby_players().await;
    let available_sessions = state_read.lobby.get_available_sessions().await;

    // Get car and track configs
    let car_configs: Vec<CarConfigSummary> = state_read
        .car_configs
        .values()
        .map(|c| CarConfigSummary {
            id: c.id,
            name: c.name.clone(),
            model_path: format!("res://content/cars/{}/{}", c.id, c.model),
            mass_kg: c.mass_kg,
            max_engine_force_n: c.max_engine_power_w / 100.0, // Rough approximation
        })
        .collect();

    let track_configs: Vec<TrackConfigSummary> = state_read
        .track_configs
        .values()
        .map(|t| TrackConfigSummary {
            id: t.id,
            name: t.name.clone(),
        })
        .collect();

    drop(state_read);

    let lobby_state = ServerMessage::LobbyState {
        players_in_lobby,
        available_sessions,
        car_configs,
        track_configs,
    };

    transport.send_tcp(connection_id, lobby_state).await?;
    Ok(())
}

/// Broadcast lobby state to all connected clients in the lobby
async fn broadcast_lobby_state(
    state: &Arc<RwLock<ServerState>>,
    transport: &TransportLayer,
) -> Result<(), Box<dyn std::error::Error>> {
    use apexsim_server::network::{ServerMessage, CarConfigSummary, TrackConfigSummary};

    let state_read = state.read().await;

    // Get lobby players and sessions
    let players_in_lobby = state_read.lobby.get_lobby_players().await;
    let available_sessions = state_read.lobby.get_available_sessions().await;

    // Get car and track configs
    let car_configs: Vec<CarConfigSummary> = state_read
        .car_configs
        .values()
        .map(|c| CarConfigSummary {
            id: c.id,
            name: c.name.clone(),
            model_path: format!("res://content/cars/{}/{}", c.id, c.model),
            mass_kg: c.mass_kg,
            max_engine_force_n: c.max_engine_power_w / 100.0, // Rough approximation
        })
        .collect();

    let track_configs: Vec<TrackConfigSummary> = state_read
        .track_configs
        .values()
        .map(|t| TrackConfigSummary {
            id: t.id,
            name: t.name.clone(),
        })
        .collect();

    drop(state_read);

    let lobby_state = ServerMessage::LobbyState {
        players_in_lobby,
        available_sessions,
        car_configs,
        track_configs,
    };

    // Broadcast to all connections
    transport.broadcast_tcp(lobby_state).await;
    Ok(())
}

async fn run_game_loop(state: Arc<RwLock<ServerState>>, transport: Arc<RwLock<TransportLayer>>, tick_rate: u16) {
    const SHOULD_LOG_TICKS: bool = false;
    let tick_duration = Duration::from_micros((1_000_000.0 / tick_rate as f64) as u64);
    let mut ticker = interval(tick_duration);

    let mut tick_count = 0u64;
    let mut player_inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();

    loop {
        ticker.tick().await;
        tick_count += 1;

        if SHOULD_LOG_TICKS {
        // Log every second (240 ticks at 240Hz)
            if tick_count % tick_rate as u64 == 0 {
                let state_read = state.read().await;
                info!(
                    "Tick {} - Active sessions: {}, Players: {}",
                    tick_count,
                    state_read.sessions.len(),
                    state_read.players.len()
                );
            }
        }

        // Process incoming TCP messages (non-blocking)
        let mut transport_write = transport.write().await;
        while let Ok(Some((connection_id, msg))) = tokio::time::timeout(
            Duration::from_micros(100),
            transport_write.recv_tcp()
        ).await {
            use apexsim_server::network::{ClientMessage, ServerMessage};
            use apexsim_server::lobby::{LobbyPlayerState, LobbySessionInfo, SessionVisibility};

            match msg {
                ClientMessage::Authenticate { player_name, .. } => {
                    // Add player to lobby after authentication
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let state_write = state.write().await;
                        let lobby_player = LobbyPlayerState {
                            player_id: conn_info.player_id,
                            player_name: player_name.clone(),
                            connection_id,
                            selected_car: None,
                        };
                        state_write.lobby.add_player(lobby_player).await;

                        // Send initial lobby state
                        drop(state_write);
                        if let Err(e) = send_lobby_state(connection_id, &state, &transport_write).await {
                            warn!("Failed to send lobby state: {:?}", e);
                        }
                    }
                }

                ClientMessage::SelectCar { car_config_id } => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let state_write = state.write().await;
                        state_write.lobby.set_player_car(conn_info.player_id, car_config_id).await;
                    }
                }

                ClientMessage::RequestLobbyState => {
                    if let Err(e) = send_lobby_state(connection_id, &state, &transport_write).await {
                        warn!("Failed to send lobby state: {:?}", e);
                    }
                }

                ClientMessage::CreateSession { track_config_id, max_players, ai_count, lap_limit, session_kind } => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let mut state_write = state.write().await;

                        // Get host's selected car
                        let selected_car = state_write.lobby.get_player_car(conn_info.player_id).await;

                        if let Some(car_id) = selected_car {
                            // Create session
                            if let Some(session_id) = state_write.create_session(
                                conn_info.player_id,
                                track_config_id,
                                session_kind,
                                max_players,
                                ai_count,
                                lap_limit
                            ) {
                                info!("Session {} created by player {}", session_id, conn_info.player_name);

                                // Register session in lobby
                                let track_name = state_write.track_configs.get(&track_config_id)
                                    .map(|t| t.name.clone())
                                    .unwrap_or_else(|| "Unknown Track".to_string());

                                let track_file = state_write.track_configs.get(&track_config_id)
                                    .and_then(|t| t.source_path.clone())
                                    .unwrap_or_else(|| "tracks/unknown.yaml".to_string());

                                let session_info = LobbySessionInfo {
                                    session_id,
                                    host_player_id: conn_info.player_id,
                                    host_name: conn_info.player_name.clone(),
                                    track_name,
                                    track_file,
                                    session_kind,
                                    track_config_id,
                                    max_players,
                                    current_player_count: 0, // join_session will increment this
                                    spectator_count: 0,
                                    state: SessionState::Lobby,
                                    visibility: SessionVisibility::Public,
                                    password_hash: None,
                                    created_at: std::time::Instant::now(),
                                };

                                state_write.lobby.register_session(session_info).await;

                                // Join host to their own session (lobby and game session)
                                let joined = state_write.lobby.join_session(conn_info.player_id, session_id).await;

                                if joined {
                                    // Add host to the actual game session
                                    if let Some(game_session) = state_write.sessions.get_mut(&session_id) {
                                        if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
                                            let _ = transport_write.send_tcp(connection_id, ServerMessage::SessionJoined {
                                                session_id,
                                                your_grid_position: grid_pos,
                                            }).await;
                                            // Track that player is in a session
                                            transport_write.set_player_session(connection_id, Some(session_id)).await;
                                        } else {
                                            // Failed to add player to game session
                                            warn!("Failed to add player {} to game session {}", conn_info.player_id, session_id);
                                            let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                                code: 500,
                                                message: "Failed to add player to game session".to_string(),
                                            }).await;
                                        }
                                    } else {
                                        // Session not found in sessions map
                                        warn!("Session {} not found in sessions map", session_id);
                                        let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                            code: 500,
                                            message: "Internal error: session not found".to_string(),
                                        }).await;
                                    }
                                } else {
                                    // Failed to join lobby
                                    warn!("Player {} failed to join lobby for session {}", conn_info.player_id, session_id);
                                    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                        code: 500,
                                        message: "Failed to join session lobby".to_string(),
                                    }).await;
                                }
                            } else {
                                // Failed to create session
                                warn!("Failed to create session for player {}: track_id={}", conn_info.player_id, track_config_id);
                                
                                // Check why it failed
                                if state_write.sessions.len() >= state_write.config.server.max_sessions as usize {
                                    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                        code: 503,
                                        message: "Server is at max session capacity".to_string(),
                                    }).await;
                                } else if !state_write.track_configs.contains_key(&track_config_id) {
                                    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                        code: 404,
                                        message: "Track configuration not found".to_string(),
                                    }).await;
                                } else {
                                    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                        code: 500,
                                        message: "Failed to create session".to_string(),
                                    }).await;
                                }
                            }
                        } else {
                            // No car selected
                            let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                code: 400,
                                message: "Must select a car before creating session".to_string(),
                            }).await;
                        }
                    }
                }

                ClientMessage::JoinSession { session_id } => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let mut state_write = state.write().await;
                        
                        // Get player's selected car
                        let selected_car = state_write.lobby.get_player_car(conn_info.player_id).await;
                        
                        let joined = state_write.lobby.join_session(conn_info.player_id, session_id).await;

                        if joined {
                            // Add player to the actual game session
                            if let Some(game_session) = state_write.sessions.get_mut(&session_id) {
                                if let Some(car_id) = selected_car {
                                    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
                                        info!("Player {} joined session {} at grid position {}", 
                                            conn_info.player_name, session_id, grid_pos);
                                        let _ = transport_write.send_tcp(connection_id, ServerMessage::SessionJoined {
                                            session_id,
                                            your_grid_position: grid_pos,
                                        }).await;
                                        // Track that player is in a session
                                        transport_write.set_player_session(connection_id, Some(session_id)).await;
                                    } else {
                                        // Failed to add to session (full)
                                        state_write.lobby.leave_session(conn_info.player_id, connection_id).await;
                                        let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                            code: 400,
                                            message: "Session is full".to_string(),
                                        }).await;
                                    }
                                } else {
                                    // No car selected
                                    state_write.lobby.leave_session(conn_info.player_id, connection_id).await;
                                    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                        code: 400,
                                        message: "Must select a car before joining session".to_string(),
                                    }).await;
                                }
                            }
                        } else {
                            let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                code: 400,
                                message: "Unable to join session (full or not in lobby state)".to_string(),
                            }).await;
                        }
                    }
                }

                ClientMessage::JoinAsSpectator { session_id } => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let state_write = state.write().await;
                        let joined = state_write.lobby.join_as_spectator(conn_info.player_id, session_id).await;

                        if joined {
                            info!("Player {} joined session {} as spectator", conn_info.player_name, session_id);
                            let _ = transport_write.send_tcp(connection_id, ServerMessage::SessionJoined {
                                session_id,
                                your_grid_position: 0, // 0 indicates spectator
                            }).await;
                            // Track that player is in a session
                            transport_write.set_player_session(connection_id, Some(session_id)).await;
                        } else {
                            let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                code: 404,
                                message: "Session not found".to_string(),
                            }).await;
                        }
                    }
                }

                ClientMessage::LeaveSession => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let state_write = state.write().await;
                        state_write.lobby.leave_session(conn_info.player_id, connection_id).await;
                        info!("Player {} left their session", conn_info.player_name);
                        let _ = transport_write.send_tcp(connection_id, ServerMessage::SessionLeft).await;
                        // Clear session tracking
                        transport_write.set_player_session(connection_id, None).await;
                    }
                }

                ClientMessage::StartSession => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let mut state_write = state.write().await;
                        
                        // Find which session the player is in
                        let session_id = state_write.lobby.get_player_session(conn_info.player_id).await;
                        
                        if let Some(session_id) = session_id {
                            if let Some(game_session) = state_write.sessions.get_mut(&session_id) {
                                // Only host can start the session
                                if game_session.session.host_player_id == conn_info.player_id {
                                    game_session.start_countdown();
                                    info!("Player {} started session {}", conn_info.player_name, session_id);

                                    // Notify all participants
                                    let msg = ServerMessage::SessionStarting { countdown_seconds: 5 };
                                    let participant_count = game_session.session.participants.len();
                                    info!("Broadcasting SessionStarting to {} participants", participant_count);

                                    for player_id in game_session.session.participants.keys() {
                                        if let Some(conn_id) = transport_write.get_player_connection(*player_id).await {
                                            info!("Sending SessionStarting to player {} (connection {})", player_id, conn_id);
                                            let _ = transport_write.send_tcp(conn_id, msg.clone()).await;
                                        } else {
                                            warn!("Could not find connection for player {}", player_id);
                                        }
                                    }
                                } else {
                                    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
                                        code: 403,
                                        message: "Only the host can start the session".to_string(),
                                    }).await;
                                }
                            }
                        }
                    }
                }

                ClientMessage::Disconnect => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let state_write = state.write().await;
                        state_write.lobby.remove_player(conn_info.player_id).await;
                    }
                }

                ClientMessage::PlayerInput { throttle, brake, steering, .. } => {
                    if let Some(conn_info) = transport_write.get_connection(connection_id).await {
                        let input = PlayerInputData {
                            throttle,
                            brake,
                            steering,
                        };
                        player_inputs.insert(conn_info.player_id, input);
                    }
                }

                _ => {
                    // Other messages (StartSession, etc.) handled elsewhere
                }
            }
        }

        // Cleanup stale connections every second
        if tick_count % tick_rate as u64 == 0 {
            transport_write.cleanup_stale_connections().await;
        }

        // Broadcast lobby state every 2 seconds
        if tick_count % (tick_rate as u64 * 2) == 0 {
            if let Err(e) = broadcast_lobby_state(&state, &transport_write).await {
                warn!("Failed to broadcast lobby state: {:?}", e);
            }
        }

        drop(transport_write);

        // Update all sessions
        let mut state_write = state.write().await;

        // Use collected inputs
        let inputs = player_inputs.clone();

        // Collect replay operations to execute after iteration
        let mut replay_starts = Vec::new();
        let mut replay_frames = Vec::new();
        let mut replay_stops = Vec::new();

        // Collect sessions to remove (empty or finished)
        let mut sessions_to_remove = Vec::new();

        // Tick each session
        for (session_id, game_session) in state_write.sessions.iter_mut() {
            // Check if session has no players (everyone left)
            if game_session.session.participants.is_empty() {
                info!("Session {} has no players, marking for removal", session_id);
                sessions_to_remove.push(*session_id);
                continue;
            }

            // Generate AI inputs for AI players
            let mut session_inputs = inputs.clone();
            
            for (player_id, _car_state) in &game_session.session.participants {
                // Check if this is an AI player (simplified check)
                if !session_inputs.contains_key(player_id) {
                    // No human input, generate AI input
                    let ai_input = game_session.generate_ai_input(player_id);
                    session_inputs.insert(*player_id, ai_input);
                }
            }

            let prev_state = game_session.session.state;
            game_session.tick(&session_inputs);
            let new_state = game_session.session.state;

            // Collect replay recording operations
            if prev_state != SessionState::Racing && new_state == SessionState::Racing {
                let participants: Vec<_> = game_session.session.participants.keys()
                    .map(|pid| apexsim_server::replay::ReplayParticipant {
                        player_id: *pid,
                        player_name: format!("Player-{}", pid), // TODO: Get actual names
                        car_config_id: game_session.session.participants.get(pid)
                            .and_then(|cs| Some(cs.car_config_id))
                            .unwrap_or_else(|| uuid::Uuid::nil()),
                        finish_position: None,
                    })
                    .collect();

                let track_config_id = game_session.session.track_config_id;
                let metadata = (*session_id, track_config_id, participants);
                replay_starts.push(metadata);
            }

            // Collect telemetry frame if racing
            if new_state == SessionState::Racing {
                let telemetry = game_session.get_telemetry();
                // Extract telemetry data from the ServerMessage
                if let apexsim_server::network::ServerMessage::Telemetry(tel) = telemetry {
                    replay_frames.push((*session_id, game_session.session.current_tick, tel));
                }
            }

            // Collect replay stops when session finishes
            if prev_state == SessionState::Racing && new_state == SessionState::Finished {
                replay_stops.push(*session_id);
            }

            // Log state changes
            if game_session.session.current_tick % tick_rate as u32 == 0 {
                match new_state {
                    SessionState::Countdown => {
                        if let Some(countdown) = game_session.session.countdown_ticks_remaining {
                            let seconds = countdown / tick_rate;
                            info!("Session {} countdown: {}s", session_id, seconds);
                        }
                    }
                    SessionState::Racing => {
                        let lap_info: Vec<String> = game_session
                            .session
                            .participants
                            .values()
                            .map(|s| format!("L{}", s.current_lap))
                            .collect();
                        info!("Session {} racing - Laps: {:?}", session_id, lap_info);
                    }
                    SessionState::Finished => {
                        info!("Session {} finished", session_id);
                    }
                    _ => {}
                }
            }
        }

        // Execute collected replay operations
        for (session_id, track_config_id, participants) in replay_starts {
            use apexsim_server::replay::ReplayMetadata;

            let track_name = state_write.track_configs.get(&track_config_id)
                .map(|t| t.name.clone())
                .unwrap_or_else(|| "Unknown Track".to_string());

            let metadata = ReplayMetadata {
                session_id,
                track_config_id,
                track_name,
                recorded_at: std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_secs(),
                duration_ticks: 0,
                tick_rate,
                participants,
            };

            state_write.replay.start_recording(metadata).await;
            info!("Started replay recording for session {}", session_id);
        }

        for (session_id, tick, telemetry) in replay_frames {
            state_write.replay.record_frame(session_id, tick, telemetry).await;
        }

        for session_id in replay_stops {
            match state_write.replay.stop_recording(session_id).await {
                Ok(replay_path) => {
                    info!("Replay saved for session {} to {:?}", session_id, replay_path);
                }
                Err(e) => {
                    warn!("Failed to save replay for session {}: {}", session_id, e);
                }
            }
        }

        // Remove empty sessions from the game state and lobby
        for session_id in sessions_to_remove {
            state_write.sessions.remove(&session_id);
            state_write.lobby.unregister_session(session_id).await;
            info!("Removed empty session {}", session_id);
        }

        // Broadcast telemetry to all session participants (via TCP for now)
        let transport_write2 = transport.write().await;
        for (session_id, game_session) in state_write.sessions.iter() {
            // Only send telemetry if session is active (not in Lobby or Closed state)
            let should_send_telemetry = matches!(
                game_session.session.state,
                SessionState::Countdown | SessionState::Racing | SessionState::Finished
            );

            if !should_send_telemetry {
                continue; // Skip telemetry for lobby sessions
            }

            // Collect real (non-AI) players who have active connections
            let mut real_players_with_connections = Vec::new();
            for player_id in game_session.session.participants.keys() {
                // Skip AI players
                if game_session.session.ai_player_ids.contains(player_id) {
                    continue;
                }

                // Check if player has an active connection
                if let Some(conn_id) = transport_write2.get_player_connection(*player_id).await {
                    real_players_with_connections.push((*player_id, conn_id));
                }
            }

            // Skip telemetry calculation and broadcast if no real players are connected
            if real_players_with_connections.is_empty() {
                if tick_count % 60 == 0 {
                    debug!("Skipping telemetry for session {} - no real players connected", session_id);
                }
                continue;
            }

            let telemetry_msg = game_session.get_telemetry();
            let participant_count = game_session.session.participants.len();

            if participant_count > 0 && tick_count % 60 == 0 {
                debug!("Broadcasting telemetry for session {} to {} real players (total participants: {}, state: {:?})",
                    session_id, real_players_with_connections.len(), participant_count, game_session.session.state);
            }

            // Send telemetry only to real players who are still connected and in this session
            for (player_id, conn_id) in real_players_with_connections {
                // Verify player is still in this session (check via lobby manager)
                if let Some(player_session) = state_write.lobby.get_player_session(player_id).await {
                    if player_session == *session_id {
                        let _ = transport_write2.send_tcp(conn_id, telemetry_msg.clone()).await;
                    }
                }
            }
        }
        drop(transport_write2);

        // Cleanup finished sessions (older than timeout)
        let timeout_seconds = state_write.config.server.session_timeout_seconds as u64;
        state_write.sessions.retain(|id, session| {
            if session.session.state == SessionState::Finished {
                let age_ticks = tick_count.saturating_sub(session.session.current_tick as u64);
                let age_seconds = age_ticks / tick_rate as u64;
                
                if age_seconds > timeout_seconds {
                    info!("Removing finished session: {}", id);
                    return false;
                }
            }
            true
        });
    }
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

        let session_id = state.create_session(host_id, track_id, SessionKind::Practice, 8, 2, 5);

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

        // Create max sessions
        for _ in 0..2 {
            let result = state.create_session(host_id, track_id, SessionKind::Sandbox, 8, 0, 3);
            assert!(result.is_some());
        }

        // Try to create one more
        let result = state.create_session(host_id, track_id, SessionKind::Multiplayer, 8, 0, 3);
        assert!(result.is_none());
    }
}

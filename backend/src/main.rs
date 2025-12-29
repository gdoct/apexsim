use apexsim_server::{
    config::ServerConfig,
    data::*,
    game_session::GameSession,
    health::{HealthState, run_health_server},
    transport::TransportLayer,
    network::ServerMessage,
};
use clap::Parser;
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;
use tokio::time::{interval, Duration};
use tracing::{info, warn};

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
}

impl ServerState {
    fn new(config: ServerConfig) -> Self {
        let mut car_configs = HashMap::new();
        let mut track_configs = HashMap::new();

        // Create default car and track
        let default_car = CarConfig::default();
        let default_track = TrackConfig::default();

        car_configs.insert(default_car.id, default_car);
        track_configs.insert(default_track.id, default_track);

        Self {
            config,
            car_configs,
            track_configs,
            sessions: HashMap::new(),
            players: HashMap::new(),
        }
    }

    #[allow(dead_code)]
    fn create_session(
        &mut self,
        host_player_id: PlayerId,
        track_config_id: TrackConfigId,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    ) -> Option<SessionId> {
        if self.sessions.len() >= self.config.server.max_sessions as usize {
            return None;
        }

        let track = self.track_configs.get(&track_config_id)?.clone();
        let session = RaceSession::new(host_player_id, track_config_id, max_players, ai_count, lap_limit);
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
    
    // Cleanup
    let final_state = state.read().await;
    info!("Server shutting down with {} active sessions", final_state.sessions.len());

    Ok(())
}

async fn run_game_loop(state: Arc<RwLock<ServerState>>, _transport: Arc<RwLock<TransportLayer>>, tick_rate: u16) {
    const SHOULD_LOG_TICKS: bool = false;
    let tick_duration = Duration::from_micros((1_000_000.0 / tick_rate as f64) as u64);
    let mut ticker = interval(tick_duration);

    let mut tick_count = 0u64;

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

        // Update all sessions
        let mut state_write = state.write().await;
        
        // Collect inputs (empty for now, will be populated by network layer)
        let inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();

        // Tick each session
        for (session_id, game_session) in state_write.sessions.iter_mut() {
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

            game_session.tick(&session_inputs);

            // Log state changes
            if game_session.session.current_tick % tick_rate as u32 == 0 {
                match game_session.session.state {
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

        let session_id = state.create_session(host_id, track_id, 8, 2, 5);

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
            let result = state.create_session(host_id, track_id, 8, 0, 3);
            assert!(result.is_some());
        }

        // Try to create one more
        let result = state.create_session(host_id, track_id, 8, 0, 3);
        assert!(result.is_none());
    }
}

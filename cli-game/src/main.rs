mod client;
mod config;
mod protocol;

use crate::client::NetworkClient;
use crate::config::ClientConfig;
use crate::protocol::*;
use anyhow::{Context, Result};
use clap::Parser;
use console::{style, Term};
use dialoguer::{Input, Select};
use std::time::Duration;
use tracing::{error, info, warn};

#[derive(Parser, Debug)]
#[command(author, version, about = "ApexSim Racing CLI Client", long_about = None)]
struct Args {
    /// Path to client.toml configuration file
    #[arg(short, long, default_value = "./client.toml")]
    config: String,

    /// Server TCP address (overrides config)
    #[arg(short, long)]
    server: Option<String>,

    /// Player name (overrides config)
    #[arg(short, long)]
    name: Option<String>,

    /// Auth token (overrides config)
    #[arg(short, long)]
    token: Option<String>,

    /// Log level (trace|debug|info|warn|error)
    #[arg(short, long, default_value = "info")]
    log_level: String,
}

struct GameClient {
    network: NetworkClient,
    config: ClientConfig,
    player_id: Option<PlayerId>,
    current_session: Option<SessionId>,
    lobby_state: Option<LobbyState>,
    term: Term,
    heartbeat_tick: u32,
}

#[derive(Debug, Clone)]
struct LobbyState {
    players: Vec<LobbyPlayer>,
    sessions: Vec<SessionSummary>,
    cars: Vec<CarConfigSummary>,
    tracks: Vec<TrackConfigSummary>,
}

impl GameClient {
    fn new(network: NetworkClient, config: ClientConfig) -> Self {
        Self {
            network,
            config,
            player_id: None,
            current_session: None,
            lobby_state: None,
            term: Term::stdout(),
            heartbeat_tick: 0,
        }
    }

    async fn authenticate(&mut self) -> Result<()> {
        println!(
            "\n{} Authenticating as '{}'...",
            style("→").cyan(),
            self.config.player.name
        );

        self.network
            .send(ClientMessage::Authenticate {
                token: self.config.player.token.clone(),
                player_name: self.config.player.name.clone(),
            })
            .await?;

        let response = self.network.receive().await?;

        match response {
            ServerMessage::AuthSuccess {
                player_id,
                server_version,
            } => {
                self.player_id = Some(player_id);
                println!(
                    "{} Authentication successful! Player ID: {}",
                    style("✓").green(),
                    player_id
                );
                println!(
                    "  Server version: {}",
                    server_version
                );
                Ok(())
            }
            ServerMessage::AuthFailure { reason } => {
                Err(anyhow::anyhow!("Authentication failed: {}", reason))
            }
            other => Err(anyhow::anyhow!("Unexpected response: {:?}", other)),
        }
    }

    async fn request_lobby_state(&mut self) -> Result<()> {
        self.network.send(ClientMessage::RequestLobbyState).await?;

        // Wait for LobbyState response, skipping telemetry and heartbeat messages
        let start = tokio::time::Instant::now();
        let timeout = Duration::from_secs(10);
        
        loop {
            if start.elapsed() > timeout {
                return Err(anyhow::anyhow!("Timeout waiting for lobby state"));
            }
            
            let response = match self.network.try_receive(Duration::from_millis(500)).await? {
                Some(msg) => msg,
                None => continue,
            };

            match response {
                ServerMessage::LobbyState {
                    players_in_lobby,
                    available_sessions,
                    car_configs,
                    track_configs,
                } => {
                    self.lobby_state = Some(LobbyState {
                        players: players_in_lobby,
                        sessions: available_sessions,
                        cars: car_configs,
                        tracks: track_configs,
                    });
                    return Ok(());
                }
                ServerMessage::Error { code, message } => {
                    return Err(anyhow::anyhow!("Server error {}: {}", code, message));
                }
                ServerMessage::HeartbeatAck { .. } | ServerMessage::Telemetry(_) => {
                    // Skip these messages and keep waiting for LobbyState
                    continue;
                }
                other => {
                    return Err(anyhow::anyhow!("Unexpected response: {:?}", other));
                }
            }
        }
    }

    fn display_lobby_state(&self) {
        // Clear screen for cleaner display
        let _ = self.term.clear_screen();
        
        let Some(lobby) = &self.lobby_state else {
            println!("{} No lobby state available", style("!").yellow());
            return;
        };

        println!("\n{}", style("═══ LOBBY STATE ═══").cyan().bold());

        // Players in lobby
        println!("\n{}", style("Players in Lobby:").bold());
        if lobby.players.is_empty() {
            println!("  (no players)");
        } else {
            for player in &lobby.players {
                let car_info = match &player.selected_car {
                    Some(car_id) => lobby
                        .cars
                        .iter()
                        .find(|c| c.id == *car_id)
                        .map(|c| c.name.as_str())
                        .unwrap_or("Unknown"),
                    None => "No car selected",
                };
                let session_info = match &player.in_session {
                    Some(_) => " [in session]",
                    None => "",
                };
                println!(
                    "  • {} - {}{}",
                    style(&player.name).green(),
                    car_info,
                    session_info
                );
            }
        }

        // Available sessions
        println!("\n{}", style("Available Sessions:").bold());
        if lobby.sessions.is_empty() {
            println!("  (no sessions)");
        } else {
            for (i, session) in lobby.sessions.iter().enumerate() {
                let status = match session.state {
                    SessionState::Lobby => style("Lobby").green(),
                    SessionState::Starting => style("Starting").yellow(),
                    SessionState::Racing => style("Racing").red(),
                    SessionState::Finished => style("Finished").dim(),
                    SessionState::Closed => style("Closed").dim(),
                };
                println!(
                    "  [{}] {} - {} ({}/{}) [{}]",
                    style(i + 1).cyan(),
                    style(&session.track_name).bold(),
                    session.host_name,
                    session.player_count,
                    session.max_players,
                    status
                );
                println!("      ID: {}", style(&session.id).dim());
            }
        }

        // Available cars
        println!("\n{}", style("Available Cars:").bold());
        for car in &lobby.cars {
            println!("  • {}", car.name);
        }

        // Available tracks
        println!("\n{}", style("Available Tracks:").bold());
        for track in &lobby.tracks {
            println!("  • {}", track.name);
        }

        println!();
    }

    async fn select_car(&mut self) -> Result<()> {
        let Some(lobby) = &self.lobby_state else {
            println!("{} Please refresh lobby state first", style("!").yellow());
            return Ok(());
        };

        if lobby.cars.is_empty() {
            println!("{} No cars available", style("!").yellow());
            return Ok(());
        }

        let car_names: Vec<&str> = lobby.cars.iter().map(|c| c.name.as_str()).collect();

        let selection = Select::new()
            .with_prompt("Select a car")
            .items(&car_names)
            .default(0)
            .interact()?;

        let car_id = lobby.cars[selection].id;

        self.network
            .send(ClientMessage::SelectCar {
                car_config_id: car_id,
            })
            .await?;

        println!(
            "{} Selected car: {}",
            style("✓").green(),
            car_names[selection]
        );
        
        // Give server time to process
        tokio::time::sleep(Duration::from_millis(100)).await;
        Ok(())
    }

    async fn create_session(&mut self) -> Result<()> {
        let Some(lobby) = &self.lobby_state else {
            println!("{} Please refresh lobby state first", style("!").yellow());
            return Ok(());
        };

        if lobby.tracks.is_empty() {
            println!("{} No tracks available", style("!").yellow());
            return Ok(());
        }

        // Select track
        let track_names: Vec<&str> = lobby.tracks.iter().map(|t| t.name.as_str()).collect();
        let track_selection = Select::new()
            .with_prompt("Select a track")
            .items(&track_names)
            .default(0)
            .interact()?;

        let track_id = lobby.tracks[track_selection].id;

        // Get session parameters
        let max_players: u8 = Input::new()
            .with_prompt("Max players")
            .default(8)
            .interact()?;

        let ai_count: u8 = Input::new()
            .with_prompt("AI drivers")
            .default(0)
            .interact()?;

        let lap_limit: u8 = Input::new()
            .with_prompt("Number of laps")
            .default(5)
            .interact()?;

        println!(
            "\n{} Creating session on {} with {} max players, {} AI, {} laps...",
            style("→").cyan(),
            track_names[track_selection],
            max_players,
            ai_count,
            lap_limit
        );

        self.network
            .send(ClientMessage::CreateSession {
                track_config_id: track_id,
                max_players,
                ai_count,
                lap_limit,
            })
            .await?;

        // Wait for response, skipping any broadcast messages
        let response = self.wait_for_response(Duration::from_secs(10)).await?;

        match response {
            ServerMessage::SessionJoined {
                session_id,
                your_grid_position,
            } => {
                self.current_session = Some(session_id);
                println!(
                    "{} Session created! ID: {}, Grid position: {}",
                    style("✓").green(),
                    session_id,
                    your_grid_position
                );
                Ok(())
            }
            ServerMessage::Error { code, message } => {
                Err(anyhow::anyhow!("Failed to create session: {} ({})", message, code))
            }
            other => Err(anyhow::anyhow!("Unexpected response: {:?}", other)),
        }
    }

    async fn join_session(&mut self) -> Result<()> {
        let Some(lobby) = &self.lobby_state else {
            println!("{} Please refresh lobby state first", style("!").yellow());
            return Ok(());
        };

        let joinable_sessions: Vec<&SessionSummary> = lobby
            .sessions
            .iter()
            .filter(|s| s.state == SessionState::Lobby && s.player_count < s.max_players)
            .collect();

        if joinable_sessions.is_empty() {
            println!("{} No joinable sessions available", style("!").yellow());
            return Ok(());
        }

        let session_names: Vec<String> = joinable_sessions
            .iter()
            .map(|s| {
                format!(
                    "{} - {} ({}/{})",
                    s.track_name, s.host_name, s.player_count, s.max_players
                )
            })
            .collect();

        let selection = Select::new()
            .with_prompt("Select a session to join")
            .items(&session_names)
            .default(0)
            .interact()?;

        let session_id = joinable_sessions[selection].id;

        println!(
            "\n{} Joining session {}...",
            style("→").cyan(),
            session_id
        );

        self.network
            .send(ClientMessage::JoinSession { session_id })
            .await?;

        // Wait for response, skipping any broadcast messages
        let response = self.wait_for_response(Duration::from_secs(10)).await?;

        match response {
            ServerMessage::SessionJoined {
                session_id,
                your_grid_position,
            } => {
                self.current_session = Some(session_id);
                println!(
                    "{} Joined session! Grid position: {}",
                    style("✓").green(),
                    your_grid_position
                );
                Ok(())
            }
            ServerMessage::Error { code, message } => {
                Err(anyhow::anyhow!("Failed to join session: {} ({})", message, code))
            }
            other => Err(anyhow::anyhow!("Unexpected response: {:?}", other)),
        }
    }

    async fn leave_session(&mut self) -> Result<()> {
        if self.current_session.is_none() {
            println!("{} Not in a session", style("!").yellow());
            return Ok(());
        }

        self.network.send(ClientMessage::LeaveSession).await?;

        // Wait for response, skipping any broadcast messages
        let response = self.wait_for_response(Duration::from_secs(5)).await?;

        match response {
            ServerMessage::SessionLeft => {
                self.current_session = None;
                println!("{} Left session", style("✓").green());
                Ok(())
            }
            ServerMessage::Error { code, message } => {
                Err(anyhow::anyhow!("Failed to leave session: {} ({})", message, code))
            }
            other => Err(anyhow::anyhow!("Unexpected response: {:?}", other)),
        }
    }

    async fn start_session(&mut self) -> Result<()> {
        if self.current_session.is_none() {
            println!("{} Not in a session", style("!").yellow());
            return Ok(());
        }

        println!("{} Starting session...", style("→").cyan());
        self.network.send(ClientMessage::StartSession).await?;

        // Wait for response, skipping any broadcast messages
        let response = self.wait_for_response(Duration::from_secs(5)).await?;

        match response {
            ServerMessage::SessionStarting { countdown_seconds } => {
                println!(
                    "{} Session starting in {} seconds!",
                    style("✓").green(),
                    countdown_seconds
                );

                // Enter telemetry mode
                self.receive_telemetry_loop().await?;
                Ok(())
            }
            ServerMessage::Error { code, message } => {
                Err(anyhow::anyhow!("Failed to start session: {} ({})", message, code))
            }
            other => Err(anyhow::anyhow!("Unexpected response: {:?}", other)),
        }
    }

    async fn receive_telemetry_loop(&mut self) -> Result<()> {
        println!("\n{}", style("═══ RACE TELEMETRY ═══").cyan().bold());
        println!("Press Ctrl+C to exit telemetry view\n");

        let mut tick_count = 0u32;
        loop {
            match self
                .network
                .try_receive(Duration::from_millis(100))
                .await
            {
                Ok(Some(ServerMessage::Telemetry(telemetry))) => {
                    tick_count += 1;
                    // Only print every 240th tick (approximately once per second at 240Hz)
                    if tick_count % 240 == 0 {
                        println!(
                            "Tick: {} | State: {:?} | Cars: {}",
                            telemetry.server_tick,
                            telemetry.session_state,
                            telemetry.car_states.len()
                        );

                        for car in &telemetry.car_states {
                            println!(
                                "  Car {} - Pos: ({:.1}, {:.1}, {:.1}) Speed: {:.1} m/s Lap: {}",
                                car.player_id,
                                car.pos_x,
                                car.pos_y,
                                car.pos_z,
                                car.speed_mps,
                                car.current_lap
                            );
                        }
                    }

                    if telemetry.session_state == SessionState::Finished {
                        println!("\n{} Race finished!", style("✓").green());
                        break;
                    }
                }
                Ok(Some(ServerMessage::SessionLeft)) => {
                    println!("{} Session ended", style("!").yellow());
                    self.current_session = None;
                    break;
                }
                Ok(Some(msg)) => {
                    println!("  [Server] {:?}", msg);
                }
                Ok(None) => {
                    // No message, continue
                }
                Err(e) => {
                    warn!("Error receiving message: {}", e);
                    break;
                }
            }
        }

        Ok(())
    }

    async fn send_heartbeat(&mut self) -> Result<()> {
        self.heartbeat_tick += 1;
        self.network
            .send(ClientMessage::Heartbeat { client_tick: self.heartbeat_tick })
            .await?;
        Ok(())
    }

    /// Wait for a specific response, skipping broadcast LobbyState and HeartbeatAck messages
    async fn wait_for_response(&mut self, timeout: Duration) -> Result<ServerMessage> {
        let start = tokio::time::Instant::now();
        let mut last_heartbeat = tokio::time::Instant::now();
        
        loop {
            if start.elapsed() > timeout {
                return Err(anyhow::anyhow!("Timeout waiting for response ({}s elapsed)", timeout.as_secs()));
            }
            
            // Send heartbeat every 2 seconds to keep connection alive
            if last_heartbeat.elapsed() > Duration::from_secs(2) {
                let _ = self.send_heartbeat().await;
                last_heartbeat = tokio::time::Instant::now();
            }
            
            // Try to receive with a short timeout so we can send heartbeats
            let msg = match self.network.try_receive(Duration::from_millis(500)).await? {
                Some(msg) => msg,
                None => continue, // No message yet, loop and potentially send heartbeat
            };
            
            // Skip broadcast messages, return anything else
            match &msg {
                ServerMessage::LobbyState { .. } => {
                    // This is a broadcast, update our state but keep waiting
                    if let ServerMessage::LobbyState {
                        players_in_lobby,
                        available_sessions,
                        car_configs,
                        track_configs,
                    } = msg {
                        self.lobby_state = Some(LobbyState {
                            players: players_in_lobby,
                            sessions: available_sessions,
                            cars: car_configs,
                            tracks: track_configs,
                        });
                    }
                    continue;
                }
                ServerMessage::HeartbeatAck { .. } => {
                    // Heartbeat acknowledgment, keep waiting
                    continue;
                }
                ServerMessage::Telemetry(_) => {
                    // Telemetry data streaming, keep waiting for actual response
                    continue;
                }
                _ => return Ok(msg),
            }
        }
    }

    fn display_main_menu(&self) -> Vec<&'static str> {
        let mut options = vec![
            "Refresh lobby state",
            "Select car",
            "Create new session",
            "Join session",
        ];

        if self.current_session.is_some() {
            options.push("Leave session");
            options.push("Start session");
        }

        options.push("Send heartbeat");
        options.push("Quit");

        options
    }

    async fn run_menu(&mut self) -> Result<bool> {
        // Send heartbeat before showing menu
        let _ = self.send_heartbeat().await;
        
        // Clear screen and show current state
        let _ = self.term.clear_screen();
        
        // Show compact status
        let player_name = &self.config.player.name;
        let session_info = if let Some(_) = &self.current_session {
            style("[In Session]").yellow()
        } else {
            style("[In Lobby]").green()
        };
        println!("\n{} {} {}", style("●").green(), style(player_name).bold(), session_info);
        
        if let Some(lobby) = &self.lobby_state {
            println!("  Players online: {} | Sessions: {} | Cars: {} | Tracks: {}",
                lobby.players.len(),
                lobby.sessions.len(),
                lobby.cars.len(),
                lobby.tracks.len()
            );
        }
        println!();
        
        let options = self.display_main_menu();

        let selection = Select::new()
            .with_prompt("What would you like to do?")
            .items(&options)
            .default(0)
            .interact()?;

        let selected = options[selection];

        match selected {
            "Refresh lobby state" => {
                self.request_lobby_state().await?;
                self.display_lobby_state();
            }
            "Select car" => {
                self.select_car().await?;
            }
            "Create new session" => {
                self.create_session().await?;
            }
            "Join session" => {
                self.join_session().await?;
            }
            "Leave session" => {
                self.leave_session().await?;
            }
            "Start session" => {
                self.start_session().await?;
            }
            "Send heartbeat" => {
                self.send_heartbeat().await?;
                println!("{} Heartbeat sent", style("✓").green());
            }
            "Quit" => {
                return Ok(false);
            }
            _ => {}
        }

        Ok(true)
    }
}

fn print_banner() {
    println!(
        r#"
    ___                     _____ _         
   /   |  ____  ___  _  __ / ___/(_)___ ___ 
  / /| | / __ \/ _ \| |/_/ \__ \/ / __ `__ \
 / ___ |/ /_/ /  __/>  <  ___/ / / / / / / /
/_/  |_/ .___/\___/_/|_| /____/_/_/ /_/ /_/ 
      /_/                                   
               {} {}
"#,
        style("CLI Client").cyan().bold(),
        style("v0.1.0").dim()
    );
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    // Initialize logging
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new(&args.log_level)),
        )
        .init();

    print_banner();

    // Load configuration
    let mut config = ClientConfig::load_or_default(&args.config);

    // Apply CLI overrides
    if let Some(server) = args.server {
        config.server.tcp_address = server;
    }
    if let Some(name) = args.name {
        config.player.name = name;
    }
    if let Some(token) = args.token {
        config.player.token = token;
    }

    println!(
        "{} Server: {}",
        style("Config").cyan(),
        config.server.tcp_address
    );
    println!(
        "{} Player: {}",
        style("Config").cyan(),
        config.player.name
    );

    // Create network client
    let tcp_addr = config.get_tcp_addr()?;
    let mut network = NetworkClient::new(tcp_addr);

    // Connect to server
    network.connect().await.context("Failed to connect to server")?;

    // Create game client
    let mut client = GameClient::new(network, config);

    // Authenticate
    client.authenticate().await?;

    // Initial lobby state request
    client.request_lobby_state().await?;
    client.display_lobby_state();

    // Main menu loop
    loop {
        match client.run_menu().await {
            Ok(true) => continue,
            Ok(false) => break,
            Err(e) => {
                println!("{} Error: {}", style("✗").red(), e);
                // Continue on error, don't exit
            }
        }
    }

    // Disconnect
    client.network.disconnect().await?;
    println!("{} Goodbye!", style("✓").green());

    Ok(())
}

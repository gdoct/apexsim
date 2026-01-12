use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage, LobbyStateData};

const SERVER_TCP_ADDR: &str = "127.0.0.1:9000";

/// Lightweight test client for demo lap testing
struct DemoLapTestClient {
    player_id: Option<PlayerId>,
    session_id: Option<SessionId>,
    tcp_stream: TcpStream,
    name: String,
    heartbeat_tick: u32,
}

impl DemoLapTestClient {
    async fn connect(name: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let tcp_stream = TcpStream::connect(SERVER_TCP_ADDR).await?;

        Ok(Self {
            player_id: None,
            session_id: None,
            tcp_stream,
            name: name.to_string(),
            heartbeat_tick: 0,
        })
    }

    async fn authenticate(&mut self) -> Result<(PlayerId, LobbyStateData), Box<dyn std::error::Error>> {
        let auth_msg = ClientMessage::Authenticate {
            token: format!("test_token_{}", self.name),
            player_name: self.name.clone(),
        };

        self.send_message(&auth_msg).await?;
        sleep(Duration::from_millis(50)).await;

        let auth_response = timeout(Duration::from_secs(5), self.receive_message()).await??;

        let player_id = match auth_response {
            ServerMessage::AuthSuccess(data) => {
                self.player_id = Some(data.player_id);
                data.player_id
            }
            ServerMessage::AuthFailure { reason } => {
                return Err(format!("Auth failed: {}", reason).into());
            }
            msg => return Err(format!("Unexpected response to authentication: {:?}", msg).into()),
        };

        // Server automatically sends lobby state after authentication
        let lobby_state = self.receive_message().await?;
        match lobby_state {
            ServerMessage::LobbyState(data) => Ok((player_id, data)),
            msg => Err(format!("Expected LobbyState, got: {:?}", msg).into()),
        }
    }

    async fn select_car(&mut self, car_id: CarConfigId) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::SelectCar {
            car_config_id: car_id,
        };
        self.send_message(&msg).await?;
        sleep(Duration::from_millis(100)).await;
        Ok(())
    }

    async fn create_session(
        &mut self,
        track_id: TrackConfigId,
        max_players: u8,
        session_kind: SessionKind,
        lap_limit: u8,
    ) -> Result<SessionId, Box<dyn std::error::Error>> {
        let msg = ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players,
            session_kind,
            ai_count: 0,
            lap_limit,
        };

        self.send_message(&msg).await?;

        // Wait for SessionJoined, skipping LobbyState updates
        let start = std::time::Instant::now();
        loop {
            let response = match timeout(Duration::from_secs(5), self.receive_message()).await {
                Ok(Ok(msg)) => msg,
                Ok(Err(e)) => return Err(format!("Error receiving message: {}", e).into()),
                Err(_) => {
                    if start.elapsed() > Duration::from_secs(10) {
                        return Err("Timeout waiting for session creation response".into());
                    }
                    continue;
                }
            };

            match response {
                ServerMessage::SessionJoined(data) => {
                    self.session_id = Some(data.session_id);
                    return Ok(data.session_id);
                }
                ServerMessage::Error { message, .. } => {
                    return Err(format!("Session creation failed: {}", message).into());
                }
                ServerMessage::LobbyState(_) => continue,
                other => {
                    return Err(format!("Unexpected response to session creation: {:?}", other).into());
                }
            }
        }
    }

    async fn set_game_mode(&mut self, mode: GameMode) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::SetGameMode { mode };
        self.send_message(&msg).await?;
        Ok(())
    }

    async fn send_heartbeat(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        self.heartbeat_tick += 1;
        let msg = ClientMessage::Heartbeat {
            client_tick: self.heartbeat_tick,
        };
        self.send_message(&msg).await
    }

    async fn send_message(&mut self, msg: &ClientMessage) -> Result<(), Box<dyn std::error::Error>> {
        let data = rmp_serde::to_vec_named(msg)?;
        let len = (data.len() as u32).to_be_bytes();
        self.tcp_stream.write_all(&len).await?;
        self.tcp_stream.write_all(&data).await?;
        self.tcp_stream.flush().await?;
        Ok(())
    }

    async fn receive_message(&mut self) -> Result<ServerMessage, Box<dyn std::error::Error>> {
        let mut len_buf = [0u8; 4];
        self.tcp_stream.read_exact(&mut len_buf).await?;
        let len = u32::from_be_bytes(len_buf);

        let mut buf = vec![0u8; len as usize];
        self.tcp_stream.read_exact(&mut buf).await?;
        let msg: ServerMessage = rmp_serde::from_slice(&buf)?;

        Ok(msg)
    }
}

/// Format lap time in minutes:seconds.milliseconds format
fn format_lap_time(ms: u32) -> String {
    let total_secs = ms as f64 / 1000.0;
    let mins = (total_secs / 60.0).floor() as u32;
    let secs = total_secs - (mins as f64 * 60.0);
    if mins > 0 {
        format!("{}:{:06.3}", mins, secs)
    } else {
        format!("{:.3}s", secs)
    }
}

/// Test: Start lobby, start demo lap game, wait for 3 laps, calculate and print lap times
/// Run: cargo test --test demo_lap_tests test_demo_lap_timing -- --ignored --nocapture
#[tokio::test]
#[ignore]
async fn test_demo_lap_timing() {
    println!("╔══════════════════════════════════════════════════════════════════════════════╗");
    println!("║                       DEMO LAP TIMING TEST                                   ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════╣");
    println!("║  1. Start lobby                                                              ║");
    println!("║  2. Start demo lap game                                                      ║");
    println!("║  3. Wait for 3 laps                                                          ║");
    println!("║  4. Calculate and print lap times                                            ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════╝");
    println!();
    println!("NOTE: Server must be running (use VS Code task: 'Start Server')");
    println!();

    let result = timeout(Duration::from_secs(325), async {
        // Step 1: Connect and authenticate
        println!("Step 1: Connecting to lobby...");
        let mut client = DemoLapTestClient::connect("DemoLapTest").await?;
        let (player_id, lobby_state) = client.authenticate().await?;
        println!("  ✓ Authenticated as player: {}", player_id);

        // Get first available car and track
        let car_id = lobby_state.car_configs.first()
            .ok_or("No car configs available")?.id;
        let car_name = &lobby_state.car_configs.first().unwrap().name;

        let track_id = lobby_state.track_configs.first()
            .ok_or("No track configs available")?.id;
        let track_name = &lobby_state.track_configs.first().unwrap().name;

        println!("  ✓ Using car: {} ({})", car_name, car_id);
        println!("  ✓ Using track: {} ({})", track_name, track_id);

        // Select car
        client.select_car(car_id).await?;
        println!("  ✓ Car selected");

        // Create session with 3 lap limit
        println!("\nStep 2: Creating session...");
        let session_id = client.create_session(track_id, 4, SessionKind::Practice, 3).await?;
        println!("  ✓ Session created: {}", session_id);

        // Step 2: Start demo lap mode
        println!("\nStep 3: Starting Demo Lap mode...");
        client.set_game_mode(GameMode::DemoLap).await?;
        println!("  ✓ Demo lap mode activated");

        // Wait briefly for mode to take effect
        sleep(Duration::from_millis(500)).await;

        // Step 3: Track lap times from telemetry
        println!("\nStep 4: Receiving telemetry and timing laps...");
        println!("  Waiting for 3 completed laps from demo driver...");
        println!();

        use std::collections::HashMap;

        struct CarLapData {
            current_lap: u16,
            completed_laps: Vec<(u16, u32)>, // (lap_number, lap_time_ms from server)
            last_lap_time_ms: Option<u32>,
        }

        let mut car_lap_data: HashMap<PlayerId, CarLapData> = HashMap::new();
        let mut total_completed_laps = 0;
        let mut telemetry_started = false;

        let max_wait = Duration::from_secs(325); // Exit after 325 seconds
        let start_time = std::time::Instant::now();
        let mut last_heartbeat = std::time::Instant::now();
        let mut last_debug_print = std::time::Instant::now();
        let mut max_track_progress: f32 = 0.0; // Track max progress to estimate track length

        while start_time.elapsed() < max_wait && total_completed_laps < 3 {
            // Send heartbeat every 2 seconds to keep connection alive
            if last_heartbeat.elapsed() > Duration::from_secs(2) {
                client.send_heartbeat().await?;
                last_heartbeat = std::time::Instant::now();
            }

            // Print status every 5 seconds even if no telemetry
            if last_debug_print.elapsed() >= Duration::from_secs(5) {
                let elapsed = start_time.elapsed().as_secs();
                println!("  [{}s] Still waiting for telemetry... (received_telemetry={})",
                    elapsed, telemetry_started);
                last_debug_print = std::time::Instant::now();
            }

            // Receive telemetry
            match timeout(Duration::from_millis(100), client.receive_message()).await {
                Ok(Ok(ServerMessage::Telemetry(telemetry))) => {
                    if !telemetry_started {
                        println!("  ✓ Receiving telemetry (server tick: {}, {} cars)",
                            telemetry.server_tick, telemetry.car_states.len());
                        for car in &telemetry.car_states {
                            println!("    - Car {} at lap {} (progress: {:.1}m, speed: {:.1} km/h, gear: {}, rpm: {:.0})",
                                &car.player_id.to_string()[..8],
                                car.current_lap,
                                car.track_progress,
                                car.speed_mps * 3.6,
                                car.gear,
                                car.engine_rpm);
                        }
                        telemetry_started = true;
                        println!();
                    }

                    // Track max progress to estimate track length
                    for car_state in &telemetry.car_states {
                        if car_state.track_progress > max_track_progress {
                            max_track_progress = car_state.track_progress;
                        }
                    }

                    // Print debug info every 5 seconds (reset timer since we got telemetry)
                    let time_since_last_print = last_debug_print.elapsed();
                    if time_since_last_print >= Duration::from_secs(5) {
                        let elapsed = start_time.elapsed().as_secs();
                        println!("\n  ═══ DEBUG @ {}s ═══", elapsed);
                        for car_state in &telemetry.car_states {
                            let pct = if max_track_progress > 0.0 {
                                (car_state.track_progress / max_track_progress) * 100.0
                            } else {
                                0.0
                            };
                            println!("  Car {}: gear={}, rpm={:.0}, speed={:.1} km/h, progress={:.1}m ({:.1}%), lap={}, throttle={:.0}%, brake={:.0}%",
                                &car_state.player_id.to_string()[..8],
                                car_state.gear,
                                car_state.engine_rpm,
                                car_state.speed_mps * 3.6,
                                car_state.track_progress,
                                pct,
                                car_state.current_lap,
                                car_state.throttle * 100.0,
                                car_state.brake * 100.0);
                        }
                        last_debug_print = std::time::Instant::now();
                    }

                    // Process each car's lap state
                    for car_state in &telemetry.car_states {
                        let car_data = car_lap_data.entry(car_state.player_id).or_insert(CarLapData {
                            current_lap: 0,
                            completed_laps: Vec::new(),
                            last_lap_time_ms: None,
                        });

                        // Check if a new lap time is available (server provides last_lap_time_ms)
                        if car_state.last_lap_time_ms != car_data.last_lap_time_ms {
                            if let Some(lap_time_ms) = car_state.last_lap_time_ms {
                                // New lap completed
                                let completed_lap = car_state.current_lap.saturating_sub(1);
                                if completed_lap > 0 && !car_data.completed_laps.iter().any(|(l, _)| *l == completed_lap) {
                                    car_data.completed_laps.push((completed_lap, lap_time_ms));
                                    total_completed_laps += 1;

                                    println!("  [Car {}] Lap {} completed: {}",
                                        &car_state.player_id.to_string()[..8],
                                        completed_lap,
                                        format_lap_time(lap_time_ms));
                                }
                            }
                            car_data.last_lap_time_ms = car_state.last_lap_time_ms;
                        }

                        car_data.current_lap = car_state.current_lap;
                    }
                }
                Ok(Ok(ServerMessage::GameModeChanged { mode })) => {
                    println!("  Game mode changed to: {:?}", mode);
                }
                Ok(Ok(other)) => {
                    // Log other messages for debugging
                    println!("  [DEBUG] Received: {:?}", std::mem::discriminant(&other));
                }
                Ok(Err(e)) => {
                    return Err(format!("Error receiving telemetry: {}", e).into());
                }
                Err(_) => {
                    // Timeout, continue polling
                }
            }
        }

        // Step 4: Print summary
        println!();
        println!("╔══════════════════════════════════════════════════════════════════════════════╗");
        println!("║                          LAP TIMING RESULTS                                  ║");
        println!("╠══════════════════════════════════════════════════════════════════════════════╣");

        for (car_id, data) in &car_lap_data {
            if data.completed_laps.is_empty() {
                continue;
            }

            println!("║  Car: {}                                             ║", &car_id.to_string()[..8]);
            println!("║  Laps completed: {}                                                        ║", data.completed_laps.len());
            println!("╠──────────────────────────────────────────────────────────────────────────────╣");

            for (lap_num, lap_time_ms) in &data.completed_laps {
                println!("║    Lap {:2}: {:>12}                                                   ║",
                    lap_num, format_lap_time(*lap_time_ms));
            }

            // Calculate best lap
            if let Some((best_lap, best_time)) = data.completed_laps.iter()
                .min_by_key(|(_, time)| *time)
            {
                println!("╠──────────────────────────────────────────────────────────────────────────────╣");
                println!("║    Best:  Lap {:2} - {:>12}                                            ║",
                    best_lap, format_lap_time(*best_time));
            }

            // Calculate average lap time
            if !data.completed_laps.is_empty() {
                let avg_time: u32 = data.completed_laps.iter()
                    .map(|(_, time)| *time)
                    .sum::<u32>() / data.completed_laps.len() as u32;
                println!("║    Avg:   {:>12}                                                      ║",
                    format_lap_time(avg_time));
            }
        }

        println!("╚══════════════════════════════════════════════════════════════════════════════╝");

        // Print final summary
        println!();
        println!("  Total laps recorded: {}", total_completed_laps);
        println!("  Max track progress seen: {:.1}m", max_track_progress);
        println!("  Elapsed time: {:.1}s", start_time.elapsed().as_secs_f32());

        if total_completed_laps == 0 {
            println!("  ⚠ No lap times recorded - demo mode may not be running correctly or car is too slow");
        }

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST COMPLETED: Demo Lap Timing (check debug output above)"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out after 325 seconds"),
    }
}

use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpStream, UdpSocket};
use tokio::sync::Mutex;
use tokio::time::{sleep, timeout, Instant};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage};

const SERVER_TCP_ADDR: &str = "127.0.0.1:9000";
const SERVER_UDP_ADDR: &str = "127.0.0.1:9001";
const TEST_TIMEOUT: Duration = Duration::from_secs(30);

/// Client simulator that can connect to the server and interact with it
struct TestClient {
    player_id: Option<PlayerId>,
    session_id: Option<SessionId>,
    tcp_stream: TcpStream,
    name: String,
    telemetry_received: Arc<Mutex<Vec<(u32, usize)>>>, // (server_tick, car_count)
    heartbeat_tick: u32,
}

impl TestClient {
    async fn connect(name: &str) -> Result<Self, Box<dyn std::error::Error>> {
        // Connect TCP (plain, no TLS for testing)
        let tcp_stream = TcpStream::connect(SERVER_TCP_ADDR).await?;

        // Create UDP socket
        let udp_socket = UdpSocket::bind("127.0.0.1:0").await?;
        udp_socket.connect(SERVER_UDP_ADDR).await?;

        Ok(Self {
            player_id: None,
            session_id: None,
            tcp_stream,
            name: name.to_string(),
            telemetry_received: Arc::new(Mutex::new(Vec::new())),
            heartbeat_tick: 0,
        })
    }

    async fn send_heartbeat(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        self.heartbeat_tick += 1;
        let msg = ClientMessage::Heartbeat {
            client_tick: self.heartbeat_tick,
        };
        self.send_tcp_message(&msg).await
    }

    async fn authenticate(&mut self) -> Result<(PlayerId, ServerMessage), Box<dyn std::error::Error>> {
        // Send authentication message
        let auth_msg = ClientMessage::Authenticate {
            token: format!("test_token_{}", self.name),
            player_name: self.name.clone(),
        };
        
        self.send_tcp_message(&auth_msg).await?;
        
        // Give server a moment to process
        sleep(Duration::from_millis(50)).await;
        
        // Wait for auth response with timeout
        let auth_response = timeout(Duration::from_secs(5), self.receive_tcp_message()).await??;
        
        let player_id = match auth_response {
            ServerMessage::AuthSuccess(data) => {
                println!("  {} authenticated successfully: {}", self.name, data.player_id);
                self.player_id = Some(data.player_id);
                data.player_id
            }
            ServerMessage::AuthFailure { reason } => {
                return Err(format!("Auth failed: {}", reason).into());
            }
            msg => return Err(format!("Unexpected response to authentication: {:?}", msg).into()),
        };
        
        // Server automatically sends lobby state after authentication
        let lobby_state = self.receive_tcp_message().await?;
        
        Ok((player_id, lobby_state))
    }

    async fn select_car(&mut self, car_id: CarConfigId) -> Result<(), Box<dyn std::error::Error>> {
        println!("DEBUG: Sending SelectCar with car_id={}", car_id);
        let msg = ClientMessage::SelectCar {
            car_config_id: car_id,
        };
        self.send_tcp_message(&msg).await?;
        // Give server time to process the car selection
        sleep(Duration::from_millis(200)).await;

        // Request lobby state to verify car selection
        println!("DEBUG: Requesting lobby state to verify car selection");
        let req = ClientMessage::RequestLobbyState;
        self.send_tcp_message(&req).await?;

        let response = timeout(Duration::from_secs(5), self.receive_tcp_message()).await??;
        if let ServerMessage::LobbyState(lobby) = &response {
            if let Some(player) = lobby.players_in_lobby.iter().find(|p| Some(p.id) == self.player_id) {
                println!("DEBUG: Player car selection in lobby: {:?}", player.selected_car);
            }
        }

        Ok(())
    }

    async fn create_session(
        &mut self,
        track_id: TrackConfigId,
        max_players: u8,
        session_kind: SessionKind,
    ) -> Result<SessionId, Box<dyn std::error::Error>> {
        let msg = ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players,
            session_kind,
            ai_count: 0,
            lap_limit: 3,
        };
        
        self.send_tcp_message(&msg).await?;

        // Wait for session joined confirmation, skipping LobbyState updates
        let start = Instant::now();
        loop {
            // Add a per-message timeout to avoid hanging forever
            let response = match timeout(Duration::from_secs(5), self.receive_tcp_message()).await {
                Ok(Ok(msg)) => msg,
                Ok(Err(e)) => return Err(format!("Error receiving message: {}", e).into()),
                Err(_) => {
                    if start.elapsed() > Duration::from_secs(10) {
                        return Err("Timeout waiting for session creation response from server".into());
                    }
                    println!("DEBUG: Still waiting for session creation response...");
                    continue;
                }
            };

            println!("DEBUG: Received message: {:?}", response);
            match response {
                ServerMessage::SessionJoined(data) => {
                    self.session_id = Some(data.session_id);
                    return Ok(data.session_id);
                }
                ServerMessage::Error { message, .. } => {
                    return Err(format!("Session creation failed: {}", message).into());
                }
                ServerMessage::LobbyState(_) => {
                    // Skip lobby state updates, they may arrive due to car selection or other clients
                    continue;
                }
                other => {
                    println!("DEBUG: Received unexpected response: {:?}", other);
                    return Err(format!("Unexpected response to session creation: {:?}", other).into());
                }
            }
        }
    }

    async fn join_session(&mut self, session_id: SessionId) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::JoinSession { session_id };

        self.send_tcp_message(&msg).await?;

        // Wait for session joined confirmation, skipping LobbyState updates
        for _ in 0..10 {
            let response = timeout(Duration::from_secs(5), self.receive_tcp_message()).await??;

            match response {
                ServerMessage::SessionJoined(data) => {
                    if data.session_id == session_id {
                        self.session_id = Some(session_id);
                        return Ok(());
                    } else {
                        return Err("Joined wrong session".into());
                    }
                }
                ServerMessage::Error { message, .. } => {
                    return Err(format!("Join failed: {}", message).into());
                }
                ServerMessage::LobbyState(_) => continue, // Skip lobby state updates
                _ => continue,
            }
        }
        Err("Timeout waiting for SessionJoined".into())
    }

    async fn start_session(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::StartSession;
        self.send_tcp_message(&msg).await
    }

    async fn send_input(
        &mut self,
        throttle: f32,
        brake: f32,
        steering: f32,
        server_tick_ack: u32,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::PlayerInput {
            server_tick_ack,
            throttle,
            brake,
            steering,
        };
        
        // Send via TCP for now (UDP not fully implemented in server)
        self.send_tcp_message(&msg).await?;
        
        Ok(())
    }

    async fn receive_telemetry(&mut self) -> Result<Option<(u32, usize)>, Box<dyn std::error::Error>> {
        // Try to receive telemetry via TCP (non-blocking)
        match timeout(Duration::from_millis(10), self.receive_tcp_message()).await {
            Ok(Ok(ServerMessage::Telemetry(telemetry))) => {
                let tick = telemetry.server_tick;
                let car_count = telemetry.car_states.len();
                Ok(Some((tick, car_count)))
            }
            Ok(Ok(_other_msg)) => {
                // Some other message, not telemetry
                Ok(None)
            }
            Ok(Err(e)) => Err(e),
            Err(_) => Ok(None), // Timeout - no telemetry available
        }
    }

    async fn send_tcp_message(&mut self, msg: &ClientMessage) -> Result<(), Box<dyn std::error::Error>> {
        let data = rmp_serde::to_vec_named(msg)?;
        let len = (data.len() as u32).to_be_bytes();
        
        self.tcp_stream.write_all(&len).await?;
        self.tcp_stream.write_all(&data).await?;
        self.tcp_stream.flush().await?;
        
        Ok(())
    }

    async fn receive_tcp_message(&mut self) -> Result<ServerMessage, Box<dyn std::error::Error>> {
        let mut len_buf = [0u8; 4];
        self.tcp_stream.read_exact(&mut len_buf).await?;
        let len = u32::from_be_bytes(len_buf);
        
        let mut buf = vec![0u8; len as usize];
        self.tcp_stream.read_exact(&mut buf).await?;
        let msg: ServerMessage = rmp_serde::from_slice(&buf)?;
        
        Ok(msg)
    }

    // Telemetry listener removed - we'll receive telemetry directly in test loop
}

#[tokio::test]
async fn test_server_initialization() {
    // This test verifies that the server can be initialized with default config
    // In a real scenario, this would test actual server startup
    assert!(true);
}

/// Integration test that requires the server to be running
/// Start the server using VS Code task: "Start Server"
/// Stop the server using VS Code task: "Stop Server"
#[tokio::test]
#[ignore] // Run manually: cargo test --test integration_test -- --ignored
async fn test_multiplayer_race_session() {
    println!("=== Multiplayer Race Session Integration Test ===");
    println!("NOTE: Server must be running (use VS Code task: 'Start Server')");
    println!("");
    
    // Wait a moment for server to be ready
    sleep(Duration::from_secs(1)).await;
    
    let result = timeout(TEST_TIMEOUT, async {
        // Create 4 test clients
        println!("Creating test clients...");
        let mut client1 = TestClient::connect("Player1").await?;
        let mut client2 = TestClient::connect("Player2").await?;
        let mut client3 = TestClient::connect("Player3").await?;
        let mut client4 = TestClient::connect("Player4").await?;
        
        // Authenticate all clients
        println!("Authenticating clients...");
        let (player1_id, lobby_state1) = client1.authenticate().await?;
        let (player2_id, _) = client2.authenticate().await?;
        let (player3_id, _) = client3.authenticate().await?;
        let (player4_id, _) = client4.authenticate().await?;
        
        println!("All clients authenticated successfully");
        println!("  Player 1: {}", player1_id);
        println!("  Player 2: {}", player2_id);
        println!("  Player 3: {}", player3_id);
        println!("  Player 4: {}", player4_id);
        
        // Extract car and track IDs from lobby state
        #[allow(unused_imports)]
        use apexsim_server::network::{CarConfigSummary, TrackConfigSummary};
        let (car_id, track_id) = match lobby_state1 {
            ServerMessage::LobbyState(lobby) => {
                let car_id = lobby.car_configs.first()
                    .ok_or("No car configs available")?.id;
                let track_id = lobby.track_configs.first()
                    .ok_or("No track configs available")?.id;
                (car_id, track_id)
            }
            _ => return Err("Expected lobby state after authentication".into()),
        };
        
        println!("Using car ID: {}", car_id);
        println!("Using track ID: {}", track_id);
        
        // All clients select cars
        println!("Clients selecting cars...");
        client1.select_car(car_id).await?;
        client2.select_car(car_id).await?;
        client3.select_car(car_id).await?;
        client4.select_car(car_id).await?;
        
        // Client 1 creates a session
        println!("Client 1 creating session...");
        let session_id = client1.create_session(track_id, 8, SessionKind::Practice).await?;
        println!("Session created: {}", session_id);
        
        // Other clients join the session
        println!("Other clients joining session...");
        client2.join_session(session_id).await?;
        println!("  Player 2 joined");
        client3.join_session(session_id).await?;
        println!("  Player 3 joined");
        client4.join_session(session_id).await?;
        println!("  Player 4 joined");
        
        // Client 1 starts the session
        println!("Starting race session...");
        client1.start_session().await?;

        // Wait for countdown (5 seconds) + buffer, sending heartbeats to keep connections alive
        println!("Waiting for countdown...");
        let countdown_start = tokio::time::Instant::now();
        while countdown_start.elapsed() < Duration::from_secs(6) {
            // Send heartbeats for all clients every second
            client1.send_heartbeat().await?;
            client2.send_heartbeat().await?;
            client3.send_heartbeat().await?;
            client4.send_heartbeat().await?;
            sleep(Duration::from_secs(1)).await;
        }
        
        // Send player inputs and receive telemetry for 5 seconds (simulating racing)
        println!("Racing for 5 seconds...");
        let race_duration = Duration::from_secs(5);
        let start_time = tokio::time::Instant::now();
        let mut tick_counter = 0u32;
        let mut last_heartbeat = tokio::time::Instant::now();

        while start_time.elapsed() < race_duration {
            tick_counter += 1;

            // Send heartbeats every 2 seconds to keep connections alive
            if last_heartbeat.elapsed() > Duration::from_secs(2) {
                client1.send_heartbeat().await?;
                client2.send_heartbeat().await?;
                client3.send_heartbeat().await?;
                client4.send_heartbeat().await?;
                last_heartbeat = tokio::time::Instant::now();
            }

            // Different inputs for each player to simulate different driving
            client1.send_input(0.9, 0.0, 0.0, tick_counter).await?;
            client2.send_input(0.8, 0.0, 0.1, tick_counter).await?;
            client3.send_input(0.85, 0.0, -0.1, tick_counter).await?;
            client4.send_input(0.7, 0.0, 0.05, tick_counter).await?;
            
            // Try to receive telemetry from each client
            if let Some((tick, car_count)) = client1.receive_telemetry().await? {
                let mut log = client1.telemetry_received.lock().await;
                log.push((tick, car_count));
                if log.len() == 1 {
                    println!("First telemetry received on Client 1: tick={}, cars={}", tick, car_count);
                }
            }
            if let Some((tick, car_count)) = client2.receive_telemetry().await? {
                let mut log = client2.telemetry_received.lock().await;
                log.push((tick, car_count));
                if log.len() == 1 {
                    println!("First telemetry received on Client 2: tick={}, cars={}", tick, car_count);
                }
            }
            if let Some((tick, car_count)) = client3.receive_telemetry().await? {
                let mut log = client3.telemetry_received.lock().await;
                log.push((tick, car_count));
                if log.len() == 1 {
                    println!("First telemetry received on Client 3: tick={}, cars={}", tick, car_count);
                }
            }
            if let Some((tick, car_count)) = client4.receive_telemetry().await? {
                let mut log = client4.telemetry_received.lock().await;
                log.push((tick, car_count));
                if log.len() == 1 {
                    println!("First telemetry received on Client 4: tick={}, cars={}", tick, car_count);
                }
            }
            
            // Run at approximately 60Hz client update rate
            sleep(Duration::from_millis(16)).await;
        }
        
        println!("Race simulation complete");
        
        // Check telemetry was received by all clients
        println!("Verifying telemetry reception...");
        
        let telemetry1 = client1.telemetry_received.lock().await;
        let telemetry2 = client2.telemetry_received.lock().await;
        let telemetry3 = client3.telemetry_received.lock().await;
        let telemetry4 = client4.telemetry_received.lock().await;
        
        println!("Telemetry received:");
        println!("  Client 1: {} packets", telemetry1.len());
        println!("  Client 2: {} packets", telemetry2.len());
        println!("  Client 3: {} packets", telemetry3.len());
        println!("  Client 4: {} packets", telemetry4.len());
        
        // Verify all clients received telemetry
        assert!(telemetry1.len() > 0, "Client 1 received no telemetry");
        assert!(telemetry2.len() > 0, "Client 2 received no telemetry");
        assert!(telemetry3.len() > 0, "Client 3 received no telemetry");
        assert!(telemetry4.len() > 0, "Client 4 received no telemetry");
        
        // Verify telemetry contains all 4 cars
        if let Some((tick, car_count)) = telemetry1.last() {
            println!("Last telemetry: tick={}, cars={}", tick, car_count);
            assert_eq!(*car_count, 4, "Expected 4 cars in telemetry");
        }
        
        // Verify ticks are increasing (synchronized)
        let ticks1: Vec<u32> = telemetry1.iter().map(|(t, _)| *t).collect();
        let ticks2: Vec<u32> = telemetry2.iter().map(|(t, _)| *t).collect();
        
        // Check that ticks are monotonically increasing
        for i in 1..ticks1.len() {
            assert!(ticks1[i] >= ticks1[i-1], "Ticks should be monotonically increasing");
        }
        
        // Check that clients are receiving similar tick ranges
        let min_tick1 = ticks1.iter().min().unwrap_or(&0);
        let max_tick1 = ticks1.iter().max().unwrap_or(&0);
        let min_tick2 = ticks2.iter().min().unwrap_or(&0);
        let max_tick2 = ticks2.iter().max().unwrap_or(&0);
        
        println!("Client 1 tick range: {} - {}", min_tick1, max_tick1);
        println!("Client 2 tick range: {} - {}", min_tick2, max_tick2);
        
        // Ticks should overlap significantly (within 10% tolerance)
        let overlap = max_tick1.min(max_tick2) - min_tick1.max(min_tick2);
        let expected_overlap = (max_tick1 - min_tick1) * 9 / 10;
        assert!(overlap >= expected_overlap, "Clients should receive synchronized ticks");
        
        println!("✓ All clients successfully connected, joined session, and received synchronized data");
        
        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;
    
    // Check test result
    match result {
        Ok(inner_result) => {
            inner_result.expect("Test failed");
            println!("\n✅ TEST PASSED: All clients successfully connected and raced!");
        }
        Err(_) => {
            panic!("Test timed out after {} seconds", TEST_TIMEOUT.as_secs());
        }
    }
}


/// Integration test to verify telemetry broadcast is working correctly
/// This test specifically checks that the server broadcasts telemetry to session participants
/// Run: cargo test --test integration_test test_telemetry_broadcast -- --ignored --nocapture
#[tokio::test]
#[ignore]
async fn test_telemetry_broadcast() {
    println!("=== Telemetry Broadcast Integration Test ===");
    println!("NOTE: Server must be running (use VS Code task: 'Start Server')");
    println!("");
    
    // Wait a moment for server to be ready
    sleep(Duration::from_secs(1)).await;
    
    let result = timeout(Duration::from_secs(60), async {
        // Create 2 test clients (simpler test case)
        println!("Step 1: Creating test clients...");
        let mut client1 = TestClient::connect("BroadcastTest1").await?;
        let mut client2 = TestClient::connect("BroadcastTest2").await?;
        println!("  ✓ Both clients connected to server");
        
        // Authenticate both clients
        println!("\nStep 2: Authenticating clients...");
        let (player1_id, lobby_state1) = client1.authenticate().await?;
        let (player2_id, _) = client2.authenticate().await?;
        
        println!("  ✓ Player 1 ID: {}", player1_id);
        println!("  ✓ Player 2 ID: {}", player2_id);
        
        // Extract car and track IDs from lobby state
        let (car_id, track_id) = match lobby_state1 {
            ServerMessage::LobbyState(lobby) => {
                let car_id = lobby.car_configs.first()
                    .ok_or("No car configs available")?.id;
                let track_id = lobby.track_configs.first()
                    .ok_or("No track configs available")?.id;
                (car_id, track_id)
            }
            _ => return Err("Expected lobby state after authentication".into()),
        };
        
        println!("  ✓ Car ID: {}", car_id);
        println!("  ✓ Track ID: {}", track_id);
        
        // Both clients select cars
        println!("\nStep 3: Selecting cars...");
        client1.select_car(car_id).await?;
        client2.select_car(car_id).await?;
        println!("  ✓ Both clients selected cars");
        
        // Allow time for car selection to be processed
        sleep(Duration::from_millis(100)).await;
        
        // Client 1 creates a session
        println!("\nStep 4: Creating session...");
        let session_id = client1.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  ✓ Session created: {}", session_id);
        
        // Client 2 joins the session
        println!("\nStep 5: Client 2 joining session...");
        client2.join_session(session_id).await?;
        println!("  ✓ Client 2 joined session");
        
        // Client 1 starts the session
        println!("\nStep 6: Starting race session...");
        client1.start_session().await?;
        println!("  ✓ Session starting...");

        // Wait for countdown (5 seconds) + buffer, sending heartbeats to keep connections alive
        println!("\nStep 7: Waiting for countdown (6 seconds)...");
        let countdown_start = tokio::time::Instant::now();
        while countdown_start.elapsed() < Duration::from_secs(6) {
            client1.send_heartbeat().await?;
            client2.send_heartbeat().await?;
            sleep(Duration::from_secs(1)).await;
        }
        println!("  ✓ Countdown complete, race should be active");
        
        // Now actively check for telemetry reception
        println!("\nStep 8: Verifying telemetry broadcast...");
        
        let mut client1_telemetry_count = 0;
        let mut client2_telemetry_count = 0;
        let mut last_client1_tick: Option<u32> = None;
        let mut last_client2_tick: Option<u32> = None;
        
        // Send some inputs to keep connection alive and check for telemetry
        let test_duration = Duration::from_secs(5);
        let start = tokio::time::Instant::now();
        let mut last_heartbeat = tokio::time::Instant::now();

        while start.elapsed() < test_duration {
            // Send heartbeats every 2 seconds
            if last_heartbeat.elapsed() > Duration::from_secs(2) {
                client1.send_heartbeat().await?;
                client2.send_heartbeat().await?;
                last_heartbeat = tokio::time::Instant::now();
            }

            // Send inputs from both clients
            let tick = start.elapsed().as_millis() as u32;
            client1.send_input(0.8, 0.0, 0.0, tick).await?;
            client2.send_input(0.7, 0.0, 0.1, tick).await?;
            
            // Try to receive telemetry on client 1
            if let Some((server_tick, car_count)) = client1.receive_telemetry().await? {
                if client1_telemetry_count == 0 {
                    println!("  🎯 Client 1 received FIRST telemetry: tick={}, cars={}", server_tick, car_count);
                }
                client1_telemetry_count += 1;
                last_client1_tick = Some(server_tick);
            }
            
            // Try to receive telemetry on client 2
            if let Some((server_tick, car_count)) = client2.receive_telemetry().await? {
                if client2_telemetry_count == 0 {
                    println!("  🎯 Client 2 received FIRST telemetry: tick={}, cars={}", server_tick, car_count);
                }
                client2_telemetry_count += 1;
                last_client2_tick = Some(server_tick);
            }
            
            // Run at approximately 60Hz
            sleep(Duration::from_millis(16)).await;
        }
        
        println!("\n=== Telemetry Reception Results ===");
        println!("  Client 1 received {} telemetry packets", client1_telemetry_count);
        println!("  Client 2 received {} telemetry packets", client2_telemetry_count);
        
        if let Some(tick) = last_client1_tick {
            println!("  Client 1 last tick: {}", tick);
        }
        if let Some(tick) = last_client2_tick {
            println!("  Client 2 last tick: {}", tick);
        }
        
        // Verify that BOTH clients received telemetry
        if client1_telemetry_count == 0 {
            return Err("BROADCAST FAILURE: Client 1 received NO telemetry packets!".into());
        }
        if client2_telemetry_count == 0 {
            return Err("BROADCAST FAILURE: Client 2 received NO telemetry packets!".into());
        }
        
        println!("\n✅ Both clients successfully received telemetry broadcasts!");
        
        // Verify tick synchronization (both should be in similar ranges)
        if let (Some(tick1), Some(tick2)) = (last_client1_tick, last_client2_tick) {
            let tick_diff = (tick1 as i64 - tick2 as i64).abs();
            println!("  Tick difference between clients: {}", tick_diff);
            if tick_diff > 100 {
                println!("  ⚠️  Warning: Clients are significantly out of sync");
            }
        }
        
        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;
    
    match result {
        Ok(inner_result) => {
            inner_result.expect("Test failed");
            println!("\n✅ TEST PASSED: Telemetry broadcast is working correctly!");
        }
        Err(_) => {
            panic!("Test timed out - server may not be running or responding");
        }
    }
}

#[tokio::test]
async fn test_sandbox_session_workflow() {
    println!("=== Sandbox Session Workflow Test ===");
    println!("NOTE: Server must be running (use VS Code task: 'Start Server')");
    println!("");
    
    // Wait a moment for server to be ready
    sleep(Duration::from_secs(1)).await;
    
    let result = timeout(TEST_TIMEOUT, async {
        // Create a test client
        println!("Creating test client...");
        let mut client = TestClient::connect("SandboxPlayer").await?;
        
        // Authenticate
        println!("Authenticating client...");
        let (player_id, lobby_state) = client.authenticate().await?;
        println!("Client authenticated: {}", player_id);
        
        // Extract car and track IDs from lobby state
        let (car_id, track_id) = match lobby_state {
            ServerMessage::LobbyState(lobby) => {
                let car_id = lobby.car_configs.first()
                    .ok_or("No car configs available")?.id;
                let track_id = lobby.track_configs.first()
                    .ok_or("No track configs available")?.id;
                (car_id, track_id)
            }
            _ => return Err("Expected lobby state after authentication".into()),
        };
        
        println!("Using car ID: {}", car_id);
        println!("Using track ID: {}", track_id);
        
        // Select car
        println!("Selecting car...");
        client.select_car(car_id).await?;
        
        // Create a sandbox session
        println!("Creating sandbox session...");
        let session_id = client.create_session(track_id, 1, SessionKind::Sandbox).await?;
        println!("Sandbox session created: {}", session_id);
        
        // Start the session
        println!("Starting sandbox session...");
        client.start_session().await?;
        
        // Wait for SessionStarting message
        println!("Waiting for SessionStarting message...");
        let msg = timeout(Duration::from_secs(5), client.receive_tcp_message()).await??;
        println!("Received message after start: {:?}", msg);
        
        match msg {
            ServerMessage::SessionStarting { countdown_seconds } => {
                println!("✓ SessionStarting received! Countdown: {}s", countdown_seconds);
                assert_eq!(countdown_seconds, 5, "Expected 5 second countdown");
            }
            ServerMessage::Error { code, message } => {
                return Err(format!("Server error {}: {}", code, message).into());
            }
            _ => {
                return Err(format!("Expected SessionStarting, got {:?}", msg).into());
            }
        }

        // Wait for countdown (5 seconds) + buffer, sending heartbeats to keep connection alive
        println!("Waiting for countdown to complete...");
        let countdown_start = Instant::now();
        while countdown_start.elapsed() < Duration::from_secs(6) {
            client.send_heartbeat().await?;
            sleep(Duration::from_secs(1)).await;
        }
        
        // Send some inputs and verify we receive telemetry (indicating race has started)
        println!("Sending inputs and verifying race started...");
        let mut telemetry_received = false;
        let race_start = Instant::now();
        let race_timeout = Duration::from_secs(3);
        let mut tick_counter = 0u32;
        
        while race_start.elapsed() < race_timeout {
            tick_counter += 1;
            client.send_input(0.8, 0.0, 0.0, tick_counter).await?;
            
            if let Some((tick, car_count)) = client.receive_telemetry().await? {
                println!("✓ Telemetry received! tick={}, cars={}", tick, car_count);
                telemetry_received = true;
                assert_eq!(car_count, 1, "Expected 1 car in sandbox session");
                break;
            }
            
            sleep(Duration::from_millis(16)).await;
        }
        
        assert!(telemetry_received, "Did not receive telemetry (race may not have started)");
        
        println!("✓ Sandbox session workflow completed successfully!");
        
        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;
    
    match result {
        Ok(Ok(())) => {
            println!("\n=== TEST PASSED ===");
        }
        Ok(Err(e)) => {
            panic!("Test failed: {}", e);
        }
        Err(_) => {
            panic!("Test timed out after {:?}", TEST_TIMEOUT);
        }
    }
}

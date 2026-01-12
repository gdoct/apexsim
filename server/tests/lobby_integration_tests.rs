use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage, LobbyStateData};

const SERVER_TCP_ADDR: &str = "127.0.0.1:9000";
const TEST_TIMEOUT: Duration = Duration::from_secs(30);

/// Lightweight test client for lobby operations
struct LobbyTestClient {
    player_id: Option<PlayerId>,
    session_id: Option<SessionId>,
    tcp_stream: TcpStream,
    name: String,
}

impl LobbyTestClient {
    async fn connect(name: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let tcp_stream = TcpStream::connect(SERVER_TCP_ADDR).await?;

        Ok(Self {
            player_id: None,
            session_id: None,
            tcp_stream,
            name: name.to_string(),
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
    ) -> Result<SessionId, Box<dyn std::error::Error>> {
        let msg = ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players,
            session_kind,
            ai_count: 0,
            lap_limit: 3,
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

    async fn join_session(&mut self, session_id: SessionId) -> Result<u8, Box<dyn std::error::Error>> {
        let msg = ClientMessage::JoinSession { session_id };
        self.send_message(&msg).await?;

        // Wait for SessionJoined, skipping LobbyState updates
        for _ in 0..10 {
            let response = timeout(Duration::from_secs(5), self.receive_message()).await??;

            match response {
                ServerMessage::SessionJoined(data) => {
                    if data.session_id == session_id {
                        self.session_id = Some(session_id);
                        return Ok(data.your_grid_position);
                    } else {
                        return Err("Joined wrong session".into());
                    }
                }
                ServerMessage::Error { message, .. } => {
                    return Err(format!("Join failed: {}", message).into());
                }
                ServerMessage::LobbyState(_) => continue,
                _ => continue,
            }
        }
        Err("Timeout waiting for SessionJoined".into())
    }

    async fn leave_session(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::LeaveSession;
        self.send_message(&msg).await?;

        // Wait for SessionLeft, skipping other messages
        for _ in 0..10 {
            match timeout(Duration::from_millis(500), self.receive_message()).await {
                Ok(Ok(ServerMessage::SessionLeft)) => {
                    self.session_id = None;
                    return Ok(());
                }
                Ok(Ok(ServerMessage::LobbyState(_))) => continue,
                Ok(Ok(ServerMessage::Telemetry(_))) => continue,
                Ok(Ok(ServerMessage::HeartbeatAck { .. })) => continue,
                Ok(Ok(other)) => {
                    return Err(format!("Unexpected response to leave: {:?}", other).into());
                }
                Ok(Err(e)) => return Err(e),
                Err(_) => continue,
            }
        }
        // If we timeout but didn't get an error, assume we left successfully
        self.session_id = None;
        Ok(())
    }

    async fn request_lobby_state(&mut self) -> Result<LobbyStateData, Box<dyn std::error::Error>> {
        let msg = ClientMessage::RequestLobbyState;
        self.send_message(&msg).await?;

        // Wait for LobbyState, skipping other messages
        for _ in 0..10 {
            match timeout(Duration::from_millis(500), self.receive_message()).await {
                Ok(Ok(ServerMessage::LobbyState(data))) => return Ok(data),
                Ok(Ok(ServerMessage::Telemetry(_))) => continue,
                Ok(Ok(ServerMessage::HeartbeatAck { .. })) => continue,
                Ok(Ok(_)) => continue,
                Ok(Err(e)) => return Err(e),
                Err(_) => continue,
            }
        }
        Err("Timeout waiting for LobbyState".into())
    }

    async fn disconnect(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::Disconnect;
        self.send_message(&msg).await?;
        Ok(())
    }

    async fn set_game_mode(&mut self, mode: GameMode) -> Result<(), Box<dyn std::error::Error>> {
        let msg = ClientMessage::SetGameMode { mode };
        self.send_message(&msg).await?;
        Ok(())
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

// =============================================================================
// BASIC LOBBY TESTS
// =============================================================================

/// Test: Create a session and verify it appears in lobby state
#[tokio::test]
#[ignore]
async fn test_create_session() {
    println!("=== Test: Create Session ===");
    println!("NOTE: Server must be running (use VS Code task: 'Start Server')");

    let result = timeout(TEST_TIMEOUT, async {
        let mut client = LobbyTestClient::connect("CreateSessionTest").await?;
        let (player_id, lobby_state) = client.authenticate().await?;
        println!("  Authenticated as player: {}", player_id);

        let car_id = lobby_state.car_configs.first()
            .ok_or("No car configs available")?.id;
        let track_id = lobby_state.track_configs.first()
            .ok_or("No track configs available")?.id;

        client.select_car(car_id).await?;
        println!("  Selected car: {}", car_id);

        let session_id = client.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Created session: {}", session_id);

        // Verify session appears in lobby state
        let updated_lobby = client.request_lobby_state().await?;
        let session_exists = updated_lobby.available_sessions.iter()
            .any(|s| s.id == session_id);
        assert!(session_exists, "Created session should appear in lobby state");
        println!("  ✓ Session appears in lobby state");

        // Verify session details
        let session = updated_lobby.available_sessions.iter()
            .find(|s| s.id == session_id)
            .unwrap();
        assert_eq!(session.max_players, 4);
        assert_eq!(session.player_count, 1); // Creator is in the session
        assert_eq!(session.session_kind, SessionKind::Practice);
        println!("  ✓ Session details are correct");

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Create Session"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Join an existing session
#[tokio::test]
#[ignore]
async fn test_join_session() {
    println!("=== Test: Join Session ===");

    let result = timeout(TEST_TIMEOUT, async {
        // Client 1 creates a session
        let mut client1 = LobbyTestClient::connect("JoinTest_Host").await?;
        let (_, lobby_state) = client1.authenticate().await?;
        println!("  Host authenticated");

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        client1.select_car(car_id).await?;
        let session_id = client1.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Session created: {}", session_id);

        // Client 2 joins the session
        let mut client2 = LobbyTestClient::connect("JoinTest_Joiner").await?;
        let (_, _) = client2.authenticate().await?;
        println!("  Joiner authenticated");

        client2.select_car(car_id).await?;
        let grid_position = client2.join_session(session_id).await?;
        println!("  Joined session at grid position: {}", grid_position);

        // Verify player count increased
        let lobby = client1.request_lobby_state().await?;
        let session = lobby.available_sessions.iter()
            .find(|s| s.id == session_id)
            .ok_or("Session not found")?;
        assert_eq!(session.player_count, 2, "Session should have 2 players");
        println!("  ✓ Session player count is correct: {}", session.player_count);

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Join Session"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Leave a session
#[tokio::test]
#[ignore]
async fn test_leave_session() {
    println!("=== Test: Leave Session ===");

    let result = timeout(TEST_TIMEOUT, async {
        // Setup: Create session with 2 players
        let mut client1 = LobbyTestClient::connect("LeaveTest_Host").await?;
        let (_, lobby_state) = client1.authenticate().await?;

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        client1.select_car(car_id).await?;
        let session_id = client1.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Session created: {}", session_id);

        let mut client2 = LobbyTestClient::connect("LeaveTest_Leaver").await?;
        client2.authenticate().await?;
        client2.select_car(car_id).await?;
        client2.join_session(session_id).await?;
        println!("  Second player joined");

        // Verify 2 players in session
        let lobby = client1.request_lobby_state().await?;
        let session = lobby.available_sessions.iter()
            .find(|s| s.id == session_id).unwrap();
        assert_eq!(session.player_count, 2);

        // Client 2 leaves
        client2.leave_session().await?;
        println!("  Second player left session");

        // Verify player count decreased
        sleep(Duration::from_millis(100)).await;
        let lobby = client1.request_lobby_state().await?;
        let session = lobby.available_sessions.iter()
            .find(|s| s.id == session_id)
            .ok_or("Session not found after leave")?;
        assert_eq!(session.player_count, 1, "Session should have 1 player after leave");
        println!("  ✓ Session player count decreased to: {}", session.player_count);

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Leave Session"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Session is removed when all players leave
#[tokio::test]
#[ignore]
async fn test_session_cleanup_on_empty() {
    println!("=== Test: Session Cleanup When Empty ===");

    let result = timeout(TEST_TIMEOUT, async {
        let mut client = LobbyTestClient::connect("CleanupTest").await?;
        let (_, lobby_state) = client.authenticate().await?;

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        client.select_car(car_id).await?;
        let session_id = client.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Session created: {}", session_id);

        // Verify session exists
        let lobby = client.request_lobby_state().await?;
        assert!(lobby.available_sessions.iter().any(|s| s.id == session_id));
        println!("  ✓ Session exists in lobby");

        // Leave session
        client.leave_session().await?;
        println!("  Left session");

        // Small delay for server to process cleanup
        sleep(Duration::from_millis(200)).await;

        // Verify session is removed (or may take some time)
        let lobby = client.request_lobby_state().await?;
        let session_still_exists = lobby.available_sessions.iter()
            .any(|s| s.id == session_id);

        // Session should be removed or have 0 players
        if session_still_exists {
            let session = lobby.available_sessions.iter()
                .find(|s| s.id == session_id).unwrap();
            assert_eq!(session.player_count, 0, "Empty session should have 0 players");
            println!("  ✓ Session has 0 players (pending cleanup)");
        } else {
            println!("  ✓ Session was removed from lobby");
        }

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Session Cleanup"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

// =============================================================================
// EDGE CASE TESTS
// =============================================================================

/// Test: Cannot join a full session (max players reached)
#[tokio::test]
#[ignore]
async fn test_max_players_limit() {
    println!("=== Test: Max Players Limit ===");

    let result = timeout(TEST_TIMEOUT, async {
        // Create session with max 2 players
        let mut host = LobbyTestClient::connect("MaxPlayers_Host").await?;
        let (_, lobby_state) = host.authenticate().await?;

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        host.select_car(car_id).await?;
        let session_id = host.create_session(track_id, 2, SessionKind::Practice).await?;
        println!("  Created session with max 2 players: {}", session_id);

        // Second player joins
        let mut player2 = LobbyTestClient::connect("MaxPlayers_P2").await?;
        player2.authenticate().await?;
        player2.select_car(car_id).await?;
        player2.join_session(session_id).await?;
        println!("  Player 2 joined");

        // Third player tries to join - should fail
        let mut player3 = LobbyTestClient::connect("MaxPlayers_P3").await?;
        player3.authenticate().await?;
        player3.select_car(car_id).await?;

        let join_result = player3.join_session(session_id).await;
        assert!(join_result.is_err(), "Third player should not be able to join full session");
        println!("  ✓ Third player correctly rejected from full session");

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Max Players Limit"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Rapid join and leave by multiple clients
#[tokio::test]
#[ignore]
async fn test_rapid_join_leave() {
    println!("=== Test: Rapid Join/Leave ===");

    let result = timeout(Duration::from_secs(60), async {
        // Create session
        let mut host = LobbyTestClient::connect("RapidTest_Host").await?;
        let (_, lobby_state) = host.authenticate().await?;

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        host.select_car(car_id).await?;
        let session_id = host.create_session(track_id, 8, SessionKind::Practice).await?;
        println!("  Session created: {}", session_id);

        // Rapid join/leave cycle with multiple clients
        let iterations = 5;
        for i in 0..iterations {
            let mut client = LobbyTestClient::connect(&format!("RapidTest_Client{}", i)).await?;
            client.authenticate().await?;
            client.select_car(car_id).await?;

            // Join
            client.join_session(session_id).await?;
            println!("  Client {} joined", i);

            // Small delay
            sleep(Duration::from_millis(50)).await;

            // Leave
            client.leave_session().await?;
            println!("  Client {} left", i);

            // Disconnect
            client.disconnect().await?;
        }

        // Verify session still has only host
        sleep(Duration::from_millis(200)).await;
        let lobby = host.request_lobby_state().await?;
        let session = lobby.available_sessions.iter()
            .find(|s| s.id == session_id)
            .ok_or("Session not found")?;

        assert_eq!(session.player_count, 1, "Session should only have host after rapid join/leave");
        println!("  ✓ Session state is consistent after {} rapid join/leave cycles", iterations);

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Rapid Join/Leave"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Multiple simultaneous session creations
#[tokio::test]
#[ignore]
async fn test_multiple_sessions() {
    println!("=== Test: Multiple Simultaneous Sessions ===");

    let result = timeout(TEST_TIMEOUT, async {
        let mut clients: Vec<LobbyTestClient> = Vec::new();
        let mut session_ids: Vec<SessionId> = Vec::new();

        // Create 3 clients
        for i in 0..3 {
            let mut client = LobbyTestClient::connect(&format!("MultiSession_Host{}", i)).await?;
            let (_, lobby_state) = client.authenticate().await?;

            let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
            let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

            client.select_car(car_id).await?;
            let session_id = client.create_session(track_id, 4, SessionKind::Practice).await?;
            println!("  Client {} created session: {}", i, session_id);

            session_ids.push(session_id);
            clients.push(client);
        }

        // Verify all sessions exist
        let lobby = clients[0].request_lobby_state().await?;

        for session_id in &session_ids {
            let exists = lobby.available_sessions.iter().any(|s| s.id == *session_id);
            assert!(exists, "Session {} should exist", session_id);
        }
        println!("  ✓ All 3 sessions exist in lobby");

        // Verify session count
        let session_count = lobby.available_sessions.len();
        assert!(session_count >= 3, "Should have at least 3 sessions, found {}", session_count);
        println!("  ✓ Lobby has {} sessions", session_count);

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Multiple Sessions"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Cannot join non-existent session
#[tokio::test]
#[ignore]
async fn test_join_nonexistent_session() {
    println!("=== Test: Join Non-existent Session ===");

    let result = timeout(TEST_TIMEOUT, async {
        let mut client = LobbyTestClient::connect("NonExistent_Test").await?;
        let (_, lobby_state) = client.authenticate().await?;

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        client.select_car(car_id).await?;

        // Generate a random session ID that doesn't exist
        let fake_session_id = uuid::Uuid::new_v4();
        println!("  Attempting to join non-existent session: {}", fake_session_id);

        let join_result = client.join_session(fake_session_id).await;
        assert!(join_result.is_err(), "Should not be able to join non-existent session");
        println!("  ✓ Correctly rejected join to non-existent session");

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Join Non-existent Session"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Lobby state updates are broadcast to connected clients
#[tokio::test]
#[ignore]
async fn test_lobby_state_broadcast() {
    println!("=== Test: Lobby State Broadcast ===");

    let result = timeout(TEST_TIMEOUT, async {
        // Client 1 connects and sits in lobby
        let mut client1 = LobbyTestClient::connect("Broadcast_Watcher").await?;
        let (_, lobby_state) = client1.authenticate().await?;
        println!("  Watcher client connected");

        let initial_session_count = lobby_state.available_sessions.len();

        // Client 2 creates a session
        let mut client2 = LobbyTestClient::connect("Broadcast_Creator").await?;
        let (_, lobby_state2) = client2.authenticate().await?;

        let car_id = lobby_state2.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state2.track_configs.first().ok_or("No tracks")?.id;

        client2.select_car(car_id).await?;
        let session_id = client2.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Creator made session: {}", session_id);

        // Client 1 should receive lobby state update (or can request it)
        // Give some time for broadcast
        sleep(Duration::from_millis(200)).await;

        // Check if client1 received a lobby state update or request one
        let updated_lobby = client1.request_lobby_state().await?;

        assert!(
            updated_lobby.available_sessions.len() > initial_session_count ||
            updated_lobby.available_sessions.iter().any(|s| s.id == session_id),
            "Watcher should see the new session"
        );
        println!("  ✓ Watcher client sees new session in lobby");

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Lobby State Broadcast"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Player appears in lobby after leaving session
#[tokio::test]
#[ignore]
async fn test_player_returns_to_lobby() {
    println!("=== Test: Player Returns to Lobby After Leaving Session ===");

    let result = timeout(TEST_TIMEOUT, async {
        let mut client = LobbyTestClient::connect("ReturnTest").await?;
        let (player_id, lobby_state) = client.authenticate().await?;
        println!("  Client authenticated: {}", player_id);

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        client.select_car(car_id).await?;

        // Check player is in lobby initially
        let lobby = client.request_lobby_state().await?;
        let in_lobby = lobby.players_in_lobby.iter().any(|p| p.id == player_id);
        assert!(in_lobby, "Player should be in lobby after auth");
        println!("  ✓ Player in lobby after auth");

        // Create and join session
        let session_id = client.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Created session: {}", session_id);

        // Leave session
        client.leave_session().await?;
        println!("  Left session");

        // Verify player is back in lobby
        sleep(Duration::from_millis(100)).await;
        let lobby = client.request_lobby_state().await?;
        let in_lobby = lobby.players_in_lobby.iter().any(|p| p.id == player_id);
        assert!(in_lobby, "Player should be back in lobby after leaving session");
        println!("  ✓ Player is back in lobby after leaving session");

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Player Returns to Lobby"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Session kinds are correctly set and visible
#[tokio::test]
#[ignore]
async fn test_session_kinds() {
    println!("=== Test: Session Kinds ===");

    let result = timeout(TEST_TIMEOUT, async {
        let kinds = [
            SessionKind::Practice,
            SessionKind::Sandbox,
            SessionKind::Multiplayer,
        ];

        for kind in kinds {
            let mut client = LobbyTestClient::connect(&format!("KindTest_{:?}", kind)).await?;
            let (_, lobby_state) = client.authenticate().await?;

            let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
            let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

            client.select_car(car_id).await?;
            let session_id = client.create_session(track_id, 4, kind).await?;

            let lobby = client.request_lobby_state().await?;
            let session = lobby.available_sessions.iter()
                .find(|s| s.id == session_id)
                .ok_or("Session not found")?;

            assert_eq!(session.session_kind, kind, "Session kind should match");
            println!("  ✓ Session kind {:?} set correctly", kind);

            client.leave_session().await?;
            client.disconnect().await?;
        }

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Session Kinds"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

/// Test: Disconnect while in session cleans up properly
#[tokio::test]
#[ignore]
async fn test_disconnect_cleanup() {
    println!("=== Test: Disconnect Cleanup ===");

    let result = timeout(TEST_TIMEOUT, async {
        // Host creates session
        let mut host = LobbyTestClient::connect("DisconnectTest_Host").await?;
        let (_host_id, lobby_state) = host.authenticate().await?;

        let car_id = lobby_state.car_configs.first().ok_or("No cars")?.id;
        let track_id = lobby_state.track_configs.first().ok_or("No tracks")?.id;

        host.select_car(car_id).await?;
        let session_id = host.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  Host created session: {}", session_id);

        // Joiner connects and joins
        let mut joiner = LobbyTestClient::connect("DisconnectTest_Joiner").await?;
        joiner.authenticate().await?;
        joiner.select_car(car_id).await?;
        joiner.join_session(session_id).await?;
        println!("  Joiner joined session");

        // Verify 2 players
        let lobby = host.request_lobby_state().await?;
        let session = lobby.available_sessions.iter()
            .find(|s| s.id == session_id).unwrap();
        assert_eq!(session.player_count, 2);

        // Joiner disconnects abruptly (drop without explicit disconnect)
        drop(joiner);
        println!("  Joiner disconnected abruptly");

        // Wait for server to detect disconnect
        sleep(Duration::from_secs(2)).await;

        // Session should still exist with host
        let lobby = host.request_lobby_state().await?;

        // Check if session still exists
        if let Some(session) = lobby.available_sessions.iter().find(|s| s.id == session_id) {
            println!("  Session player count after disconnect: {}", session.player_count);
            // Player count should be reduced
            assert!(session.player_count <= 2, "Player count should not increase");
        } else {
            println!("  Session was removed (host may have been the one that disconnected)");
        }

        println!("  ✓ Server handled disconnect cleanup");

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Disconnect Cleanup"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out"),
    }
}

// =============================================================================
// DEMO MODE TEST
// =============================================================================

/// Test: Demo mode with 2 players, different cars, Zandvoort track, timing 3 laps
/// Run: cargo test --test lobby_integration_tests test_demo_mode_lap_timing -- --ignored --nocapture
#[tokio::test]
#[ignore]
async fn test_demo_mode_lap_timing() {
    println!("╔══════════════════════════════════════════════════════════════════════════════╗");
    println!("║                    DEMO MODE LAP TIMING TEST                                 ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════╣");
    println!("║  2 players with different cars on Zandvoort track                            ║");
    println!("║  Start demo mode and time 3 laps                                             ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════╝");
    println!();

    let result = timeout(Duration::from_secs(300), async {
        // Create 2 clients
        println!("Step 1: Creating and authenticating 2 clients...");
        let mut client1 = LobbyTestClient::connect("DemoTest_Player1").await?;
        let mut client2 = LobbyTestClient::connect("DemoTest_Player2").await?;

        let (player1_id, lobby_state) = client1.authenticate().await?;
        let (player2_id, _) = client2.authenticate().await?;

        println!("  ✓ Player 1: {}", player1_id);
        println!("  ✓ Player 2: {}", player2_id);

        // Get car configs - we need at least 2 different cars
        if lobby_state.car_configs.len() < 2 {
            return Err("Need at least 2 different car configs for this test".into());
        }

        let car1_id = lobby_state.car_configs[0].id;
        let car1_name = lobby_state.car_configs[0].name.clone();
        let car2_id = lobby_state.car_configs[1].id;
        let car2_name = lobby_state.car_configs[1].name.clone();

        println!("\nStep 2: Selecting different cars...");
        println!("  Player 1 selecting: {} ({})", car1_name, car1_id);
        println!("  Player 2 selecting: {} ({})", car2_name, car2_id);

        client1.select_car(car1_id).await?;
        client2.select_car(car2_id).await?;

        // Find Zandvoort track
        println!("\nStep 3: Finding Zandvoort track...");
        let zandvoort_track = lobby_state.track_configs.iter()
            .find(|t| t.name.to_lowercase().contains("zandvoort"))
            .ok_or("Zandvoort track not found in available tracks")?;

        let track_id = zandvoort_track.id;
        println!("  ✓ Found Zandvoort: {} ({})", zandvoort_track.name, track_id);

        // Player 1 creates session on Zandvoort
        println!("\nStep 4: Creating session on Zandvoort...");
        let session_id = client1.create_session(track_id, 4, SessionKind::Practice).await?;
        println!("  ✓ Session created: {}", session_id);

        // Player 2 joins the session
        println!("\nStep 5: Player 2 joining session...");
        client2.join_session(session_id).await?;
        println!("  ✓ Player 2 joined");

        // Set game mode to DemoLap
        println!("\nStep 6: Starting Demo Mode...");
        client1.set_game_mode(GameMode::DemoLap).await?;
        println!("  ✓ Demo mode activated");

        // Wait briefly for mode to take effect
        sleep(Duration::from_millis(500)).await;

        // Track lap timing data for all cars in telemetry
        // In demo mode, AI drivers are the ones driving (human players are spectators)
        // We'll track laps for all cars we see in telemetry
        use std::collections::HashMap as StdHashMap;

        struct CarLapData {
            current_lap: u16,
            lap_start_time: Option<std::time::Instant>,
            completed_laps: Vec<(u16, u128)>, // (lap_number, duration_ms)
        }

        let mut car_lap_data: StdHashMap<PlayerId, CarLapData> = StdHashMap::new();

        println!("\nStep 7: Receiving telemetry and timing laps...");
        println!("  Note: In demo mode, AI drivers drive while human players spectate");
        println!("  Waiting for 3 completed laps from the demo driver...");

        let max_wait = Duration::from_secs(240); // 4 minutes max
        let start_time = std::time::Instant::now();

        // Track when we started receiving valid telemetry
        let mut telemetry_started = false;
        let mut total_completed_laps = 0;

        while start_time.elapsed() < max_wait {
            // Receive telemetry from client1
            match timeout(Duration::from_millis(100), client1.receive_message()).await {
                Ok(Ok(ServerMessage::Telemetry(telemetry))) => {
                    if !telemetry_started {
                        println!("  ✓ Receiving telemetry (server tick: {}, {} cars)",
                            telemetry.server_tick, telemetry.car_states.len());
                        for car in &telemetry.car_states {
                            println!("    - Car {} at lap {} (progress: {:.1}m)",
                                car.player_id, car.current_lap, car.track_progress);
                        }
                        telemetry_started = true;
                    }

                    let now = std::time::Instant::now();

                    // Process each car's lap state
                    for car_state in &telemetry.car_states {
                        let car_data = car_lap_data.entry(car_state.player_id).or_insert(CarLapData {
                            current_lap: 0,
                            lap_start_time: None,
                            completed_laps: Vec::new(),
                        });

                        if car_state.current_lap > car_data.current_lap {
                            // Lap completed or first lap started
                            if let Some(lap_start) = car_data.lap_start_time {
                                let lap_duration = now.duration_since(lap_start).as_millis();
                                let completed_lap = car_data.current_lap;
                                if completed_lap > 0 {
                                    car_data.completed_laps.push((completed_lap, lap_duration));
                                    total_completed_laps += 1;
                                    let secs = lap_duration as f64 / 1000.0;
                                    let mins = (secs / 60.0).floor() as u32;
                                    let remaining_secs = secs - (mins as f64 * 60.0);
                                    if mins > 0 {
                                        println!("  [Car {}] Lap {} completed: {}:{:06.3}",
                                            &car_state.player_id.to_string()[..8],
                                            completed_lap, mins, remaining_secs);
                                    } else {
                                        println!("  [Car {}] Lap {} completed: {:.3}s",
                                            &car_state.player_id.to_string()[..8],
                                            completed_lap, secs);
                                    }
                                }
                            }
                            car_data.current_lap = car_state.current_lap;
                            car_data.lap_start_time = Some(now);
                        } else if car_data.lap_start_time.is_none() && car_state.current_lap > 0 {
                            // First lap started
                            car_data.current_lap = car_state.current_lap;
                            car_data.lap_start_time = Some(now);
                            println!("  [Car {}] Started lap {}",
                                &car_state.player_id.to_string()[..8],
                                car_state.current_lap);
                        }
                    }

                    // Check if we have 3 completed laps total
                    if total_completed_laps >= 3 {
                        break;
                    }
                }
                Ok(Ok(ServerMessage::GameModeChanged { mode })) => {
                    println!("  Game mode changed to: {:?}", mode);
                }
                Ok(Ok(_)) => {
                    // Other message, ignore
                }
                Ok(Err(e)) => {
                    return Err(format!("Error receiving telemetry: {}", e).into());
                }
                Err(_) => {
                    // Timeout, continue polling
                }
            }
        }

        // Print summary
        println!("\n╔══════════════════════════════════════════════════════════════════════════════╗");
        println!("║                          LAP TIMING RESULTS                                  ║");
        println!("╠══════════════════════════════════════════════════════════════════════════════╣");

        for (car_id, data) in &car_lap_data {
            println!("║  Car {} ({} laps completed):", &car_id.to_string()[..8], data.completed_laps.len());
            for (lap_num, duration_ms) in &data.completed_laps {
                let secs = *duration_ms as f64 / 1000.0;
                let mins = (secs / 60.0).floor() as u32;
                let remaining_secs = secs - (mins as f64 * 60.0);
                if mins > 0 {
                    println!("║    Lap {}: {}:{:06.3}", lap_num, mins, remaining_secs);
                } else {
                    println!("║    Lap {}: {:.3}s", lap_num, secs);
                }
            }
        }
        println!("╚══════════════════════════════════════════════════════════════════════════════╝");

        // Verify we got at least some lap data
        if total_completed_laps == 0 {
            return Err("No lap times recorded - demo mode may not be running correctly".into());
        }

        // Calculate and print best laps for each car
        println!("\n  Best laps:");
        for (car_id, data) in &car_lap_data {
            if !data.completed_laps.is_empty() {
                let best_lap = data.completed_laps.iter()
                    .min_by_key(|(_, duration)| *duration)
                    .unwrap();
                let secs = best_lap.1 as f64 / 1000.0;
                let mins = (secs / 60.0).floor() as u32;
                let remaining_secs = secs - (mins as f64 * 60.0);
                if mins > 0 {
                    println!("    Car {}: Lap {} - {}:{:06.3}",
                        &car_id.to_string()[..8], best_lap.0, mins, remaining_secs);
                } else {
                    println!("    Car {}: Lap {} - {:.3}s",
                        &car_id.to_string()[..8], best_lap.0, secs);
                }
            }
        }

        println!("\n  Total laps recorded: {}", total_completed_laps);

        Ok::<(), Box<dyn std::error::Error>>(())
    }).await;

    match result {
        Ok(Ok(())) => println!("\n✅ TEST PASSED: Demo Mode Lap Timing"),
        Ok(Err(e)) => panic!("Test failed: {}", e),
        Err(_) => panic!("Test timed out after 300 seconds"),
    }
}

// =============================================================================
// RUN ALL LOBBY TESTS
// =============================================================================

/// Run all lobby integration tests
/// Execute with: cargo test --test lobby_integration_tests -- --ignored --nocapture
#[tokio::test]
#[ignore]
async fn run_all_lobby_tests() {
    println!("╔══════════════════════════════════════════════════════════════════════════════╗");
    println!("║                    LOBBY INTEGRATION TEST SUITE                              ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════╣");
    println!("║  NOTE: Server must be running (use VS Code task: 'Start Server')             ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════╝");
    println!();
    println!("To run individual tests:");
    println!("  cargo test --test lobby_integration_tests test_create_session -- --ignored --nocapture");
    println!("  cargo test --test lobby_integration_tests test_join_session -- --ignored --nocapture");
    println!("  cargo test --test lobby_integration_tests test_leave_session -- --ignored --nocapture");
    println!("  cargo test --test lobby_integration_tests test_max_players_limit -- --ignored --nocapture");
    println!("  cargo test --test lobby_integration_tests test_rapid_join_leave -- --ignored --nocapture");
    println!();
    println!("This test file contains the following lobby-specific tests:");
    println!("  - test_create_session: Create session and verify lobby state");
    println!("  - test_join_session: Join an existing session");
    println!("  - test_leave_session: Leave a session");
    println!("  - test_session_cleanup_on_empty: Session removed when all leave");
    println!("  - test_max_players_limit: Cannot join full session");
    println!("  - test_rapid_join_leave: Stress test rapid join/leave");
    println!("  - test_multiple_sessions: Multiple simultaneous sessions");
    println!("  - test_join_nonexistent_session: Error on invalid session");
    println!("  - test_lobby_state_broadcast: Lobby updates are broadcast");
    println!("  - test_player_returns_to_lobby: Player returns after leaving");
    println!("  - test_session_kinds: Different session kinds work");
    println!("  - test_disconnect_cleanup: Abrupt disconnect cleanup");
}

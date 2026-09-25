//! Protocol v2 integration tests: protocol-version gating on `Authenticate`,
//! the UDP handshake flow, compact telemetry over UDP, and player input
//! received via UDP — all against an in-process server.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpStream, UdpSocket};
use tokio::time::timeout;

use apexsim_server::network::{ClientMessage, ServerMessage, PROTOCOL_VERSION};

const TEST_TIMEOUT: Duration = Duration::from_secs(15);

struct ProtocolTestClient {
    tcp: TcpStream,
}

impl ProtocolTestClient {
    async fn connect(tcp_addr: SocketAddr) -> Self {
        let tcp = TcpStream::connect(tcp_addr).await.expect("tcp connect");
        Self { tcp }
    }

    async fn send(&mut self, msg: &ClientMessage) {
        let data = rmp_serde::to_vec_named(msg).expect("serialize");
        let len = (data.len() as u32).to_be_bytes();
        self.tcp.write_all(&len).await.expect("write len");
        self.tcp.write_all(&data).await.expect("write data");
        self.tcp.flush().await.expect("flush");
    }

    async fn recv(&mut self) -> ServerMessage {
        let mut len_buf = [0u8; 4];
        self.tcp.read_exact(&mut len_buf).await.expect("read len");
        let len = u32::from_be_bytes(len_buf) as usize;
        let mut buf = vec![0u8; len];
        self.tcp.read_exact(&mut buf).await.expect("read data");
        rmp_serde::from_slice(&buf).expect("deserialize server message")
    }

    /// Receive messages until `pred` matches, skipping unrelated broadcasts
    /// (lobby state, heartbeat acks, ...).
    async fn recv_until<F: Fn(&ServerMessage) -> bool>(&mut self, pred: F) -> ServerMessage {
        timeout(TEST_TIMEOUT, async {
            loop {
                let msg = self.recv().await;
                if pred(&msg) {
                    return msg;
                }
            }
        })
        .await
        .expect("timed out waiting for expected message")
    }

    async fn authenticate(&mut self, name: &str, protocol_version: u8) -> ServerMessage {
        self.send(&ClientMessage::Authenticate {
            token: "test-token".to_string(),
            player_name: name.to_string(),
            protocol_version,
        })
        .await;
        self.recv_until(|m| {
            matches!(
                m,
                ServerMessage::AuthSuccess(_) | ServerMessage::AuthFailure { .. }
            )
        })
        .await
    }
}

#[tokio::test]
async fn test_auth_rejected_on_protocol_version_mismatch() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;

    let response = client.authenticate("OldClient", PROTOCOL_VERSION - 1).await;
    match response {
        ServerMessage::AuthFailure { reason } => {
            assert!(
                reason.contains("protocol version"),
                "rejection reason should mention the protocol version, got: {}",
                reason
            );
        }
        other => panic!("expected AuthFailure, got {:?}", other),
    }

    server.shutdown().await;
}

#[tokio::test]
async fn test_auth_rejected_for_legacy_client_without_version() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;

    // protocol_version = 0 is what a pre-versioning client deserializes to.
    let response = client.authenticate("LegacyClient", 0).await;
    assert!(
        matches!(response, ServerMessage::AuthFailure { .. }),
        "legacy clients must be rejected, got {:?}",
        response
    );

    server.shutdown().await;
}

#[tokio::test]
async fn test_auth_success_carries_udp_binding_info() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;

    let response = client.authenticate("V2Client", PROTOCOL_VERSION).await;
    match response {
        ServerMessage::AuthSuccess(data) => {
            assert_eq!(data.protocol_version, PROTOCOL_VERSION);
            assert!(!data.udp_token.is_empty(), "udp_token must be issued");
            assert_eq!(data.udp_port, server.udp_addr.port());
        }
        other => panic!("expected AuthSuccess, got {:?}", other),
    }

    server.shutdown().await;
}

/// Binds a UDP socket to the authenticated connection: re-sends the
/// handshake until it is acked, since datagrams may race the bind.
async fn bind_udp(server_ip: std::net::IpAddr, udp_token: &str, udp_port: u16) -> UdpSocket {
    let udp = UdpSocket::bind("127.0.0.1:0").await.expect("bind udp");
    let server_udp: SocketAddr = SocketAddr::new(server_ip, udp_port);
    udp.connect(server_udp).await.expect("connect udp");

    let handshake = rmp_serde::to_vec_named(&ClientMessage::UdpHandshake {
        token: udp_token.to_string(),
    })
    .expect("serialize handshake");

    let mut buf = vec![0u8; 65_536];
    let acked = timeout(TEST_TIMEOUT, async {
        loop {
            udp.send(&handshake).await.expect("send handshake");
            match timeout(Duration::from_millis(250), udp.recv(&mut buf)).await {
                Ok(Ok(n)) => {
                    if let Ok(ServerMessage::UdpHandshakeAck) =
                        rmp_serde::from_slice::<ServerMessage>(&buf[..n])
                    {
                        return true;
                    }
                }
                _ => continue,
            }
        }
    })
    .await
    .unwrap_or(false);
    assert!(acked, "UDP handshake was never acked");
    udp
}

/// Selects the lobby's first car and track, creates a one-player practice
/// session and puts it in free practice, so the car is simulated.
async fn start_free_practice(client: &mut ProtocolTestClient) {
    client.send(&ClientMessage::RequestLobbyState).await;
    let (car_id, track_id) = match client
        .recv_until(|m| matches!(m, ServerMessage::LobbyState(_)))
        .await
    {
        ServerMessage::LobbyState(lobby) => (lobby.car_configs[0].id, lobby.track_configs[0].id),
        _ => unreachable!(),
    };
    client
        .send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await;
    client
        .send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 1,
            ai_count: 0,
            lap_limit: 2,
            session_kind: apexsim_server::data::SessionKind::Practice,
            allowed_assists: Default::default(),
            conditions: Default::default(),
        })
        .await;
    client
        .recv_until(|m| matches!(m, ServerMessage::SessionJoined(_)))
        .await;
    client
        .send(&ClientMessage::SetGameMode {
            mode: apexsim_server::data::GameMode::FreePractice,
        })
        .await;
    client
        .recv_until(|m| matches!(m, ServerMessage::GameModeChanged { .. }))
        .await;
}

/// The simulation keeps real time: a session advances the configured number
/// of ticks per wall-clock second. On Windows the 240 Hz loop used to wake
/// on the default 15.6 ms timer and ran 64 ticks a second, so every car
/// moved at 27% of real time while its reported speed and lap times looked
/// normal (see `timer_resolution.rs`).
#[tokio::test]
async fn test_session_ticks_at_the_configured_rate() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;
    assert!(matches!(
        client.authenticate("Clock", PROTOCOL_VERSION).await,
        ServerMessage::AuthSuccess(_)
    ));
    start_free_practice(&mut client).await;

    // Telemetry comes over TCP here (no UDP handshake); each frame carries
    // the session's tick, so dropped frames do not matter.
    let next_tick = |msg: ServerMessage| match msg {
        ServerMessage::TelemetryCompact(t) => t.server_tick,
        _ => unreachable!(),
    };
    let is_telemetry = |m: &ServerMessage| matches!(m, ServerMessage::TelemetryCompact(_));

    let first_tick = next_tick(client.recv_until(is_telemetry).await);
    let started = std::time::Instant::now();
    let mut last_tick = first_tick;
    while started.elapsed() < Duration::from_secs(3) {
        last_tick = next_tick(client.recv_until(is_telemetry).await);
    }
    let rate = (last_tick - first_tick) as f64 / started.elapsed().as_secs_f64();

    let target = apexsim_server::config::ServerConfig::default()
        .server
        .tick_rate_hz as f64;
    assert!(
        rate > 0.8 * target && rate < 1.1 * target,
        "the session ticked at {rate:.0} Hz of a configured {target} Hz"
    );

    server.shutdown().await;
}

/// Full UDP loopback: handshake binds the socket, input flows in over UDP,
/// compact telemetry flows back out over UDP, and the roster (TCP) maps the
/// compact car index to the player.
#[tokio::test]
async fn test_udp_handshake_input_and_telemetry_loopback() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;

    // --- Authenticate over TCP (v2) ---
    let (player_id, udp_token, udp_port) =
        match client.authenticate("UdpDriver", PROTOCOL_VERSION).await {
            ServerMessage::AuthSuccess(data) => (data.player_id, data.udp_token, data.udp_port),
            other => panic!("expected AuthSuccess, got {:?}", other),
        };

    // --- UDP handshake ---
    let udp = bind_udp(server.tcp_addr.ip(), &udp_token, udp_port).await;
    let mut buf = vec![0u8; 65_536];

    // --- Set up a session over TCP ---
    let car_id = {
        let msg = client
            .recv_until(|m| matches!(m, ServerMessage::LobbyState(_)))
            .await;
        match msg {
            ServerMessage::LobbyState(lobby) => lobby.car_configs[0].id,
            _ => unreachable!(),
        }
    };
    client
        .send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await;

    // Request fresh lobby state to learn a track id.
    client.send(&ClientMessage::RequestLobbyState).await;
    let track_id = {
        let msg = client
            .recv_until(|m| matches!(m, ServerMessage::LobbyState(_)))
            .await;
        match msg {
            ServerMessage::LobbyState(lobby) => lobby.track_configs[0].id,
            _ => unreachable!(),
        }
    };

    client
        .send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 2,
            ai_count: 0,
            lap_limit: 2,
            session_kind: apexsim_server::data::SessionKind::Practice,
            allowed_assists: Default::default(),
            conditions: Default::default(),
        })
        .await;
    client
        .recv_until(|m| matches!(m, ServerMessage::SessionJoined(_)))
        .await;

    // --- Roster must arrive over TCP (next tick after joining) and include
    // this player at index 0 ---
    let roster = client
        .recv_until(|m| matches!(m, ServerMessage::SessionRoster(_)))
        .await;
    match roster {
        ServerMessage::SessionRoster(data) => {
            assert!(
                data.entries
                    .iter()
                    .any(|e| e.player_id == player_id && e.player_name == "UdpDriver"),
                "roster must map the player's car index to their name"
            );
        }
        _ => unreachable!(),
    }

    client
        .send(&ClientMessage::SetGameMode {
            mode: apexsim_server::data::GameMode::FreePractice,
        })
        .await;
    client
        .recv_until(|m| matches!(m, ServerMessage::GameModeChanged { .. }))
        .await;

    // --- Drive over UDP, receive compact telemetry over UDP ---
    let input = rmp_serde::to_vec_named(&ClientMessage::PlayerInput {
        server_tick_ack: 0,
        throttle: 1.0,
        brake: 0.0,
        steering: 0.0,
        gear: Some(1),
        clutch: Some(1.0),
        drs: None,
    })
    .expect("serialize input");

    let moving = timeout(TEST_TIMEOUT, async {
        let mut telemetry_seen = 0u32;
        loop {
            // Keep driving (well under the 300/s per-address rate limit).
            udp.send(&input).await.expect("send input");
            match timeout(Duration::from_millis(100), udp.recv(&mut buf)).await {
                Ok(Ok(n)) => {
                    if let Ok(ServerMessage::TelemetryCompact(t)) =
                        rmp_serde::from_slice::<ServerMessage>(&buf[..n])
                    {
                        telemetry_seen += 1;
                        if let Some(car) = t.car_states.first() {
                            assert_eq!(car.car_index, 0);
                            if car.speed_mps > 0.5 && telemetry_seen > 3 {
                                return true;
                            }
                        }
                    }
                }
                _ => continue,
            }
        }
    })
    .await
    .unwrap_or(false);

    assert!(
        moving,
        "expected compact telemetry over UDP showing the car accelerating from UDP input"
    );

    server.shutdown().await;
}

/// Force feedback: the driver of a car receives its `DriverFeedback` over
/// UDP, one steering-torque sample per physics tick since the last message,
/// and the torque pushes back against the lock.
#[tokio::test]
async fn test_driver_feedback_reaches_the_driver_over_udp() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;

    let (udp_token, udp_port) = match client.authenticate("FfbDriver", PROTOCOL_VERSION).await {
        ServerMessage::AuthSuccess(data) => (data.udp_token, data.udp_port),
        other => panic!("expected AuthSuccess, got {:?}", other),
    };
    let udp = bind_udp(server.tcp_addr.ip(), &udp_token, udp_port).await;
    start_free_practice(&mut client).await;

    // Accelerate with a little left lock (positive is left).
    let input = rmp_serde::to_vec_named(&ClientMessage::PlayerInput {
        server_tick_ack: 0,
        throttle: 0.6,
        brake: 0.0,
        steering: 0.15,
        gear: Some(1),
        clutch: Some(1.0),
        drs: None,
    })
    .expect("serialize input");

    let mut buf = vec![0u8; 65_536];
    let default_divisor = apexsim_server::config::ServerConfig::default()
        .network
        .telemetry_divisor as usize;
    let feedback = timeout(TEST_TIMEOUT, async {
        let mut seen = 0u32;
        loop {
            udp.send(&input).await.expect("send input");
            let Ok(Ok(n)) = timeout(Duration::from_millis(100), udp.recv(&mut buf)).await else {
                continue;
            };
            if let Ok(ServerMessage::DriverFeedback(f)) =
                rmp_serde::from_slice::<ServerMessage>(&buf[..n])
            {
                seen += 1;
                assert!(
                    f.steer_torque.len() <= default_divisor,
                    "one sample per tick since the last message, got {}",
                    f.steer_torque.len()
                );
                // Once the car is rolling the fronts are working.
                if seen > 30 && f.steer_torque.iter().any(|&t| t < -0.01) {
                    return f;
                }
            }
        }
    })
    .await
    .expect("no DriverFeedback with a centring steering torque arrived over UDP");

    assert_eq!(
        feedback.steer_torque.len(),
        default_divisor,
        "a steady stream carries every tick"
    );
    assert!(
        feedback.steer_torque.iter().all(|t| *t <= 0.0),
        "a left turn pushes the wheel right: {:?}",
        feedback.steer_torque
    );

    server.shutdown().await;
}

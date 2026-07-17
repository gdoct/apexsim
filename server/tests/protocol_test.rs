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

    // --- UDP handshake (retry until acked; datagrams may race the bind) ---
    let udp = UdpSocket::bind("127.0.0.1:0").await.expect("bind udp");
    let server_udp: SocketAddr = SocketAddr::new(server.tcp_addr.ip(), udp_port);
    udp.connect(server_udp).await.expect("connect udp");

    let handshake = rmp_serde::to_vec_named(&ClientMessage::UdpHandshake {
        token: udp_token.clone(),
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

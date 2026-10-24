//! Protocol v2 integration tests: protocol-version gating on `Authenticate`,
//! the UDP handshake flow, compact telemetry over UDP, and player input
//! received via UDP — all against an in-process server.

use crate::common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpStream, UdpSocket};
use tokio::time::timeout;

use apexsim_server::network::{ClientMessage, ServerMessage, PROTOCOL_VERSION};
use apexsim_server::udp_seal;

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
            resume_token: None,
            driver_id: None,
            driver_key: None,
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

/// A UDP handshake that is not sealed under the connection's key is never
/// acked: a bare datagram (a client from before sealing, or anyone who
/// sniffed the token), one under the wrong key, and a replay of a genuine
/// one are all dropped, while the genuine one binds.
#[tokio::test]
async fn test_udp_handshake_needs_the_seal() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;
    let (udp_token, udp_key, udp_port) = match client.authenticate("Sealed", PROTOCOL_VERSION).await
    {
        ServerMessage::AuthSuccess(data) => (data.udp_token, data.udp_key, data.udp_port),
        other => panic!("expected AuthSuccess, got {:?}", other),
    };
    assert_eq!(udp_key.len(), 64, "udp_key is 32 bytes of hex");

    let socket = UdpSocket::bind("127.0.0.1:0").await.expect("bind udp");
    socket
        .connect(SocketAddr::new(server.tcp_addr.ip(), udp_port))
        .await
        .expect("connect udp");
    let handshake = rmp_serde::to_vec_named(&ClientMessage::UdpHandshake {
        token: udp_token.clone(),
    })
    .expect("serialize handshake");

    let mut buf = vec![0u8; 65_536];
    async fn acked(socket: &UdpSocket, datagram: Vec<u8>, buf: &mut [u8]) -> bool {
        for _ in 0..3 {
            socket.send(&datagram).await.expect("send");
            if let Ok(Ok(n)) = timeout(Duration::from_millis(150), socket.recv(buf)).await {
                if let Ok(ServerMessage::UdpHandshakeAck) =
                    rmp_serde::from_slice::<ServerMessage>(&buf[..n])
                {
                    return true;
                }
            }
        }
        false
    }

    assert!(
        !acked(&socket, handshake.clone(), &mut buf).await,
        "a bare handshake must not bind"
    );
    assert!(
        !acked(
            &socket,
            udp_seal::seal("not-the-key", 1, &handshake),
            &mut buf
        )
        .await,
        "a handshake under another key must not bind"
    );
    let genuine = udp_seal::seal(&udp_key, 5, &handshake);
    assert!(
        acked(&socket, genuine.clone(), &mut buf).await,
        "the sealed handshake binds"
    );
    assert!(
        !acked(&socket, genuine, &mut buf).await,
        "a replayed handshake must not be accepted again"
    );
    assert!(
        acked(&socket, udp_seal::seal(&udp_key, 6, &handshake), &mut buf).await,
        "the next sequence number is accepted"
    );

    server.shutdown().await;
}

/// A client's UDP socket that seals what it sends (`udp_seal`): every
/// datagram under the connection's key with a rising sequence number.
struct SealedUdp {
    socket: UdpSocket,
    key: String,
    seq: u64,
}

impl SealedUdp {
    async fn send(&mut self, payload: &[u8]) {
        self.seq += 1;
        self.socket
            .send(&udp_seal::seal(&self.key, self.seq, payload))
            .await
            .expect("send sealed datagram");
    }

    async fn recv(&self, buf: &mut [u8]) -> std::io::Result<usize> {
        self.socket.recv(buf).await
    }
}

/// Binds a UDP socket to the authenticated connection: re-sends the
/// handshake until it is acked, since datagrams may race the bind.
async fn bind_udp(
    server_ip: std::net::IpAddr,
    udp_token: &str,
    udp_key: &str,
    udp_port: u16,
) -> SealedUdp {
    let socket = UdpSocket::bind("127.0.0.1:0").await.expect("bind udp");
    let server_udp: SocketAddr = SocketAddr::new(server_ip, udp_port);
    socket.connect(server_udp).await.expect("connect udp");
    let mut udp = SealedUdp {
        socket,
        key: udp_key.to_string(),
        seq: 0,
    };

    let handshake = rmp_serde::to_vec_named(&ClientMessage::UdpHandshake {
        token: udp_token.to_string(),
    })
    .expect("serialize handshake");

    let mut buf = vec![0u8; 65_536];
    let acked = timeout(TEST_TIMEOUT, async {
        loop {
            udp.send(&handshake).await;
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
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: apexsim_server::data::QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
            ballast: Vec::new(),
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

    let target = apexsim_server::config::ServerConfig::default()
        .server
        .tick_rate_hz as f64;
    let in_band = |rate: f64| rate > 0.8 * target && rate < 1.1 * target;
    // Up to three 2 s windows, passing on the first in the band: the old
    // coarse timer put every window at a quarter of the rate, while a
    // loaded machine (the AI surveys beside a full `cargo test`) starves
    // one now and then.
    let mut rates = Vec::new();
    for _ in 0..3 {
        let first_tick = next_tick(client.recv_until(is_telemetry).await);
        let started = std::time::Instant::now();
        let mut last_tick = first_tick;
        while started.elapsed() < Duration::from_secs(2) {
            last_tick = next_tick(client.recv_until(is_telemetry).await);
        }
        rates.push((last_tick - first_tick) as f64 / started.elapsed().as_secs_f64());
        if in_band(*rates.last().unwrap()) {
            break;
        }
    }
    assert!(
        rates.iter().any(|&r| in_band(r)),
        "the session ticked at {rates:.0?} Hz of a configured {target} Hz"
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
    let (player_id, udp_token, udp_key, udp_port) =
        match client.authenticate("UdpDriver", PROTOCOL_VERSION).await {
            ServerMessage::AuthSuccess(data) => {
                (data.player_id, data.udp_token, data.udp_key, data.udp_port)
            }
            other => panic!("expected AuthSuccess, got {:?}", other),
        };

    // --- UDP handshake ---
    let mut udp = bind_udp(server.tcp_addr.ip(), &udp_token, &udp_key, udp_port).await;
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
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: apexsim_server::data::QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
            ballast: Vec::new(),
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
        headlights: None,
        flash: None,
        ers_mode: None,
        ers_boost: None,
        handbrake: None,
        seq: None,
    })
    .expect("serialize input");

    let moving = timeout(TEST_TIMEOUT, async {
        let mut telemetry_seen = 0u32;
        loop {
            // Keep driving (well under the 300/s per-address rate limit).
            udp.send(&input).await;
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

    let (udp_token, udp_key, udp_port) =
        match client.authenticate("FfbDriver", PROTOCOL_VERSION).await {
            ServerMessage::AuthSuccess(data) => (data.udp_token, data.udp_key, data.udp_port),
            other => panic!("expected AuthSuccess, got {:?}", other),
        };
    let mut udp = bind_udp(server.tcp_addr.ip(), &udp_token, &udp_key, udp_port).await;
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
        headlights: None,
        flash: None,
        ers_mode: None,
        ers_boost: None,
        handbrake: None,
        seq: None,
    })
    .expect("serialize input");

    let mut buf = vec![0u8; 65_536];
    let default_divisor = apexsim_server::config::ServerConfig::default()
        .network
        .telemetry_divisor as usize;
    let feedback = timeout(TEST_TIMEOUT, async {
        let mut seen = 0u32;
        loop {
            udp.send(&input).await;
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
    assert!(
        feedback.own_car.is_none() && feedback.own_state.is_none(),
        "a client that does not number its inputs gets no snapshot"
    );

    server.shutdown().await;
}

fn sequenced_input(seq: Option<u32>, throttle: f32) -> Vec<u8> {
    rmp_serde::to_vec_named(&ClientMessage::PlayerInput {
        server_tick_ack: 0,
        throttle,
        brake: 1.0 - throttle,
        steering: 0.0,
        gear: Some(1),
        clutch: Some(1.0),
        drs: None,
        headlights: None,
        flash: None,
        ers_mode: None,
        ers_boost: None,
        handbrake: None,
        seq,
    })
    .expect("serialize input")
}

/// Client prediction's wire (docs/proposals/client-prediction.md): an input
/// without a sequence still drives and gets no snapshot; once the client
/// numbers its inputs, each `DriverFeedback` carries the car at its tick
/// with the newest number applied by then, and the car's full state comes
/// in slices that make a blob the car step loads.
#[tokio::test]
async fn test_input_sequence_comes_back_with_the_own_car_snapshot() {
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;
    let (udp_token, udp_key, udp_port) =
        match client.authenticate("Predictor", PROTOCOL_VERSION).await {
            ServerMessage::AuthSuccess(data) => (data.udp_token, data.udp_key, data.udp_port),
            other => panic!("expected AuthSuccess, got {:?}", other),
        };
    let mut udp = bind_udp(server.tcp_addr.ip(), &udp_token, &udp_key, udp_port).await;
    start_free_practice(&mut client).await;
    let mut buf = vec![0u8; 65_536];

    // An old-style input drives the car, and no snapshot comes back.
    let old_style = sequenced_input(None, 1.0);
    let moving = timeout(TEST_TIMEOUT, async {
        loop {
            udp.send(&old_style).await;
            let Ok(Ok(n)) = timeout(Duration::from_millis(100), udp.recv(&mut buf)).await else {
                continue;
            };
            match rmp_serde::from_slice::<ServerMessage>(&buf[..n]) {
                Ok(ServerMessage::DriverFeedback(f)) => {
                    assert!(f.own_car.is_none() && f.own_state.is_none());
                }
                Ok(ServerMessage::TelemetryCompact(t))
                    if t.car_states.first().is_some_and(|c| c.speed_mps > 1.0) =>
                {
                    return;
                }
                _ => {}
            }
        }
    })
    .await;
    assert!(moving.is_ok(), "an input with no sequence drives the car");

    // Numbered inputs: braking up to `SWITCH`, full throttle from it on.
    const SWITCH: u32 = 40;
    let mut seq = 0u32;
    let mut blob: Option<(u32, Option<u32>, u16, Vec<u8>)> = None;
    let (mut braking_seen, mut throttle_seen, mut largest) = (0u32, 0u32, 0usize);
    let whole_blob = timeout(TEST_TIMEOUT, async {
        loop {
            seq += 1;
            let throttle = if seq >= SWITCH { 1.0 } else { 0.0 };
            udp.send(&sequenced_input(Some(seq), throttle)).await;
            let Ok(Ok(n)) = timeout(Duration::from_millis(20), udp.recv(&mut buf)).await else {
                continue;
            };
            let Ok(ServerMessage::DriverFeedback(f)) =
                rmp_serde::from_slice::<ServerMessage>(&buf[..n])
            else {
                continue;
            };
            largest = largest.max(n);
            let Some(own) = &f.own_car else {
                // A numbered input still in flight: the last one applied
                // had none.
                continue;
            };
            assert_eq!(
                own.tick, f.server_tick,
                "the snapshot is the car at its tick"
            );
            let applied = own.input_seq.expect("the applied input's number");
            assert!(applied <= seq, "{applied} applied, {seq} sent");
            // The throttle the car applied on that tick is that input's.
            let throttle_applied = own.controls[0];
            if applied >= SWITCH {
                assert_eq!(
                    throttle_applied, 1.0,
                    "input {applied} at tick {}",
                    own.tick
                );
                throttle_seen += 1;
            } else {
                assert_eq!(
                    throttle_applied, 0.0,
                    "input {applied} at tick {}",
                    own.tick
                );
                braking_seen += 1;
            }
            if let Some(chunk) = f.own_state {
                assert!(chunk.tick <= own.tick && chunk.input_seq <= own.input_seq);
                if chunk.index == 0 {
                    blob = Some((chunk.tick, chunk.input_seq, chunk.count, Vec::new()));
                }
                if let Some((tick, _, count, bytes)) = blob.as_mut() {
                    if *tick == chunk.tick && *count == chunk.count {
                        bytes.extend_from_slice(&chunk.bytes.0);
                        if chunk.index + 1 == *count && throttle_seen > 10 {
                            return blob.take().unwrap();
                        }
                    }
                }
            }
        }
    })
    .await
    .expect("no whole state blob arrived");
    assert!(braking_seen > 0 && throttle_seen > 0);
    assert!(largest <= 1200, "a {largest}-byte feedback datagram");

    let (tick, input_seq, _, bytes) = whole_blob;
    let state = apexsim_server::car_step::decode_state(&bytes).expect("the slices make a blob");
    assert!(
        input_seq.is_some_and(|s| s >= SWITCH),
        "blob of tick {tick}"
    );
    assert!(state.speed_mps > 0.0);

    server.shutdown().await;
}

/// While the debug stand-in drives a numbered client's car, the snapshots
/// still come, naming no input applied: the client predicts the server's
/// driver (`apexsim.net.PredictServerDriven`) or draws the puppet.
#[tokio::test]
async fn test_the_stand_ins_car_comes_back_with_no_input_applied() {
    let mut config = apexsim_server::config::ServerConfig::default();
    config.debug.stand_in_driver = true;
    let server = common::start_test_server_with_config(config).await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;
    let (udp_token, udp_key, udp_port) =
        match client.authenticate("Stood in", PROTOCOL_VERSION).await {
            ServerMessage::AuthSuccess(data) => (data.udp_token, data.udp_key, data.udp_port),
            other => panic!("expected AuthSuccess, got {:?}", other),
        };
    let mut udp = bind_udp(server.tcp_addr.ip(), &udp_token, &udp_key, udp_port).await;
    start_free_practice(&mut client).await;
    let mut buf = vec![0u8; 65_536];
    let mut seq = 0u32;
    let snapshots = timeout(TEST_TIMEOUT, async {
        let mut seen = 0;
        loop {
            seq += 1;
            udp.send(&sequenced_input(Some(seq), 1.0)).await;
            let Ok(Ok(n)) = timeout(Duration::from_millis(20), udp.recv(&mut buf)).await else {
                continue;
            };
            if let Ok(ServerMessage::DriverFeedback(f)) =
                rmp_serde::from_slice::<ServerMessage>(&buf[..n])
            {
                if let Some(own) = f.own_car {
                    assert_eq!(own.input_seq, None, "the stand-in drove tick {}", own.tick);
                    seen += 1;
                    if seen >= 10 {
                        return seen;
                    }
                }
            }
        }
    })
    .await;
    assert!(snapshots.is_ok(), "no own-car snapshot under the stand-in");

    server.shutdown().await;
}

/// A driver's `SessionJoined.Physics` names the track's physics bundle; the
/// chunks asked for one at a time make it whole, the files in it are the
/// track the server loaded (its YAML's checksum is the lobby's), and a
/// checksum the server does not have comes back as no chunks.
#[tokio::test]
async fn test_the_track_bundle_comes_in_chunks_on_request() {
    use apexsim_server::track_bundle::TrackFiles;
    let server = common::start_test_server().await;
    let mut client = ProtocolTestClient::connect(server.tcp_addr).await;
    client.authenticate("Bundler", PROTOCOL_VERSION).await;
    client.send(&ClientMessage::RequestLobbyState).await;
    let (car_id, track) = match client
        .recv_until(|m| matches!(m, ServerMessage::LobbyState(_)))
        .await
    {
        ServerMessage::LobbyState(lobby) => {
            (lobby.car_configs[0].id, lobby.track_configs[0].clone())
        }
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
            track_config_id: track.id,
            max_players: 1,
            ai_count: 0,
            lap_limit: 2,
            session_kind: apexsim_server::data::SessionKind::Practice,
            allowed_assists: Default::default(),
            conditions: Default::default(),
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: apexsim_server::data::QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
            ballast: Vec::new(),
        })
        .await;
    let physics = match client
        .recv_until(|m| matches!(m, ServerMessage::SessionJoined(_)))
        .await
    {
        ServerMessage::SessionJoined(joined) => joined.physics.expect("a driver gets Physics"),
        _ => unreachable!(),
    };
    assert_ne!(physics.bundle_crc, 0, "the track is served as a bundle");
    assert!(physics.bundle_chunks >= 1 && physics.bundle_bytes > 0);

    let mut bytes = Vec::new();
    for chunk in 0..physics.bundle_chunks {
        client
            .send(&ClientMessage::RequestTrackBundle {
                crc: physics.bundle_crc,
                chunk,
            })
            .await;
        match client
            .recv_until(|m| matches!(m, ServerMessage::TrackBundleChunk(_)))
            .await
        {
            ServerMessage::TrackBundleChunk(data) => {
                assert_eq!(
                    (data.crc, data.chunk, data.chunks),
                    (physics.bundle_crc, chunk, physics.bundle_chunks)
                );
                bytes.extend_from_slice(&data.bytes.0);
            }
            _ => unreachable!(),
        }
    }
    assert_eq!(bytes.len(), physics.bundle_bytes as usize);
    let files = TrackFiles::decode(&bytes, Some(physics.bundle_crc)).expect("the bundle decodes");
    assert_eq!(
        apexsim_server::content_crc::content_crc(&files.yaml),
        track.content_crc,
        "the bundle's YAML is the track the lobby lists"
    );
    assert_eq!(
        files.road.is_some(),
        physics.road_contact == 1,
        "the road mesh comes when the session drives on it"
    );
    assert!(TrackFiles::decode(&bytes, Some(physics.bundle_crc ^ 1)).is_err());

    client
        .send(&ClientMessage::RequestTrackBundle {
            crc: physics.bundle_crc ^ 1,
            chunk: 0,
        })
        .await;
    match client
        .recv_until(|m| matches!(m, ServerMessage::TrackBundleChunk(_)))
        .await
    {
        ServerMessage::TrackBundleChunk(data) => {
            assert_eq!(data.chunks, 0, "no such bundle");
            assert!(data.bytes.0.is_empty());
        }
        _ => unreachable!(),
    }

    server.shutdown().await;
}

/// A driver's own livery online (`apexsim_server::livery_store`): uploaded in
/// chunks from a session, named on their car in the roster the other driver
/// gets, fetched by that driver chunk by chunk to the same bytes; a package
/// that is not one, or an upload from outside a session, is refused, and a
/// livery picked from the car's own takes it off again.
#[tokio::test]
async fn test_an_own_livery_goes_to_the_other_drivers() {
    use apexsim_server::livery::{LiveryPackage, LIVERY_CHUNK_BYTES};
    use apexsim_server::livery_store::hash_of;
    use apexsim_server::network::RecordBytes;

    let server = common::start_test_server().await;
    let mut host = ProtocolTestClient::connect(server.tcp_addr).await;
    host.authenticate("Painter", PROTOCOL_VERSION).await;
    let mut guest = ProtocolTestClient::connect(server.tcp_addr).await;
    guest.authenticate("Watcher", PROTOCOL_VERSION).await;
    host.send(&ClientMessage::RequestLobbyState).await;
    let (car_id, track_id) = match host
        .recv_until(|m| matches!(m, ServerMessage::LobbyState(_)))
        .await
    {
        ServerMessage::LobbyState(lobby) => (lobby.car_configs[0].id, lobby.track_configs[0].id),
        _ => unreachable!(),
    };

    // A PNG header and padding: 300 kB, two chunks.
    let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13];
    png.extend_from_slice(b"IHDR");
    png.extend_from_slice(&1024u32.to_be_bytes());
    png.extend_from_slice(&1024u32.to_be_bytes());
    png.resize(300_000, 7);
    let package = rmp_serde::to_vec_named(&LiveryPackage {
        car: car_id.to_string(),
        name: "Mine".into(),
        paint: Some([0.6, 0.05, 0.04]),
        skin: Some(RecordBytes(png)),
        ..Default::default()
    })
    .unwrap();
    let hash = hash_of(&package);
    let chunks: Vec<&[u8]> = package.chunks(LIVERY_CHUNK_BYTES).collect();
    assert_eq!(chunks.len(), 2);
    let upload =
        |bytes: &[u8], hash: &str, chunk: usize, chunks: usize| ClientMessage::UploadLivery {
            hash: hash.to_string(),
            chunk: chunk as u32,
            chunks: chunks as u32,
            bytes: RecordBytes(bytes.to_vec()),
        };

    // Outside a session: refused.
    guest.send(&upload(chunks[0], &hash, 0, 2)).await;
    match guest
        .recv_until(|m| matches!(m, ServerMessage::Error { .. }))
        .await
    {
        ServerMessage::Error { code, .. } => assert_eq!(code, 409),
        _ => unreachable!(),
    }

    for client in [&mut host, &mut guest] {
        client
            .send(&ClientMessage::SelectCar {
                car_config_id: car_id,
                livery: 0,
            })
            .await;
    }
    host.send(&ClientMessage::CreateSession {
        track_config_id: track_id,
        max_players: 2,
        ai_count: 0,
        lap_limit: 2,
        session_kind: apexsim_server::data::SessionKind::Practice,
        allowed_assists: Default::default(),
        conditions: Default::default(),
        damage: Default::default(),
        ai_skill: None,
        race_seconds: None,
        grid_order: Vec::new(),
        qualifying_seconds: None,
        weekend: None,
        watch: None,
        qualifying_cutoff_pct: None,
        mandatory_stops: 0,
        compound_change: false,
        qualifying_format: apexsim_server::data::QualifyingFormat::Open,
        qualifying_lap_allowance: 0,
        race_rules: Default::default(),
        classes: Vec::new(),
        ballast: Vec::new(),
    })
    .await;
    let session_id = match host
        .recv_until(|m| matches!(m, ServerMessage::SessionJoined(_)))
        .await
    {
        ServerMessage::SessionJoined(joined) => joined.session_id,
        _ => unreachable!(),
    };
    guest.send(&ClientMessage::JoinSession { session_id }).await;
    guest
        .recv_until(|m| matches!(m, ServerMessage::SessionJoined(_)))
        .await;

    // Not a livery: refused, worn by no one.
    let junk = b"not a livery".to_vec();
    host.send(&upload(&junk, &hash_of(&junk), 0, 1)).await;
    match host
        .recv_until(|m| matches!(m, ServerMessage::Error { .. }))
        .await
    {
        ServerMessage::Error { code, .. } => assert_eq!(code, 400),
        _ => unreachable!(),
    }

    for (i, chunk) in chunks.iter().enumerate() {
        host.send(&upload(chunk, &hash, i, chunks.len())).await;
    }
    let worn = |m: &ServerMessage, hash: &str| match m {
        ServerMessage::SessionRoster(roster) => roster
            .entries
            .iter()
            .any(|e| !e.is_ai && e.player_name == "Painter" && e.own_livery == hash),
        _ => false,
    };
    guest.recv_until(|m| worn(m, &hash)).await;

    let mut fetched = Vec::new();
    let mut count = 1;
    let mut chunk = 0;
    while chunk < count {
        guest
            .send(&ClientMessage::RequestLivery {
                hash: hash.clone(),
                chunk,
            })
            .await;
        match guest
            .recv_until(|m| matches!(m, ServerMessage::LiveryChunk(_)))
            .await
        {
            ServerMessage::LiveryChunk(data) => {
                assert_eq!((data.hash.as_str(), data.chunk), (hash.as_str(), chunk));
                count = data.chunks;
                fetched.extend_from_slice(&data.bytes.0);
            }
            _ => unreachable!(),
        }
        chunk += 1;
    }
    assert_eq!(count, 2);
    assert_eq!(hash_of(&fetched), hash, "the same bytes come back");

    guest
        .send(&ClientMessage::RequestLivery {
            hash: "0".repeat(64),
            chunk: 0,
        })
        .await;
    match guest
        .recv_until(|m| matches!(m, ServerMessage::LiveryChunk(_)))
        .await
    {
        ServerMessage::LiveryChunk(data) => assert_eq!(data.chunks, 0, "no such livery"),
        _ => unreachable!(),
    }

    // The car's works livery picked: the own one comes off.
    host.send(&ClientMessage::SetLivery { livery: 0 }).await;
    guest.recv_until(|m| worn(m, "")).await;

    server.shutdown().await;
}

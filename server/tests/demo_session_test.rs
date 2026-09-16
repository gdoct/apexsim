//! `SessionKind::Demo`: the AI-only race a client plays behind its menu.
//! The creator watches as a spectator, the session is unlisted and
//! unjoinable, it counts itself into a race, and it goes away when the
//! spectator leaves.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{timeout, Instant};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage};

struct Client {
    stream: TcpStream,
    /// The server drops a connection that is silent for 5 s.
    last_heartbeat: Instant,
    heartbeat_tick: u32,
}

impl Client {
    async fn connect(name: &str, addr: SocketAddr) -> (Self, LobbyStateData) {
        let stream = TcpStream::connect(addr).await.expect("connect");
        let mut client = Self {
            stream,
            last_heartbeat: Instant::now(),
            heartbeat_tick: 0,
        };
        client
            .send(&ClientMessage::Authenticate {
                token: format!("test_token_{}", name),
                player_name: name.to_string(),
                protocol_version: apexsim_server::network::PROTOCOL_VERSION,
            })
            .await;
        assert!(matches!(client.recv().await, ServerMessage::AuthSuccess(_)));
        let lobby = client
            .recv_until(|msg| match msg {
                ServerMessage::LobbyState(data) => Some(data),
                _ => None,
            })
            .await;
        (client, lobby)
    }

    async fn send(&mut self, msg: &ClientMessage) {
        let data = rmp_serde::to_vec_named(msg).expect("encode");
        self.stream
            .write_all(&(data.len() as u32).to_be_bytes())
            .await
            .expect("write length");
        self.stream.write_all(&data).await.expect("write body");
        self.stream.flush().await.expect("flush");
    }

    async fn recv(&mut self) -> ServerMessage {
        let mut len = [0u8; 4];
        timeout(Duration::from_secs(5), self.stream.read_exact(&mut len))
            .await
            .expect("timed out waiting for a message")
            .expect("read length");
        let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
        self.stream.read_exact(&mut buf).await.expect("read body");
        rmp_serde::from_slice(&buf).expect("decode")
    }

    /// Skip messages until `pick` accepts one (20 s budget).
    async fn recv_until<T>(&mut self, mut pick: impl FnMut(ServerMessage) -> Option<T>) -> T {
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            assert!(Instant::now() < deadline, "expected message never arrived");
            if self.last_heartbeat.elapsed() > Duration::from_secs(1) {
                self.last_heartbeat = Instant::now();
                self.heartbeat_tick += 1;
                let client_tick = self.heartbeat_tick;
                self.send(&ClientMessage::Heartbeat { client_tick }).await;
            }
            if let Some(found) = pick(self.recv().await) {
                return found;
            }
        }
    }
}

#[tokio::test]
async fn test_demo_session_is_a_private_ai_race_watched_by_its_creator() {
    let server = common::start_test_server().await;

    // No SelectCar: a demo needs no car of the host's.
    let (mut host, lobby) = Client::connect("DemoHost", server.tcp_addr).await;
    let track = lobby.track_configs.first().expect("a track").id;
    host.send(&ClientMessage::CreateSession {
        track_config_id: track,
        max_players: 8,
        ai_count: 3,
        lap_limit: 3,
        session_kind: SessionKind::Demo,
        allowed_assists: Default::default(),
        conditions: Default::default(),
    })
    .await;

    let joined = host
        .recv_until(|msg| match msg {
            ServerMessage::SessionJoined(data) => Some(data),
            ServerMessage::Error { message, .. } => panic!("demo creation failed: {message}"),
            _ => None,
        })
        .await;
    assert_eq!(joined.session_kind, SessionKind::Demo);
    assert_eq!(
        joined.your_grid_position, 0,
        "the host watches, it does not drive"
    );
    let session_id = joined.session_id;

    {
        let state = server.state.read().await;
        let session = &state.sessions[&session_id].session;
        assert_eq!(
            session.participants.len(),
            3,
            "only the AI field is on the grid"
        );
        assert!(!session.participants.contains_key(&session.host_player_id));
        assert_eq!(
            session.state,
            SessionState::Countdown,
            "counted in without a start"
        );
    }
    // A debug build ticks well under real time; skip most of the grid.
    if let Some(demo) = server.state.write().await.sessions.get_mut(&session_id) {
        demo.session.countdown_ticks_remaining = Some(10);
    }

    // The spectator gets the roster and the telemetry (TCP fallback: this
    // client never did the UDP handshake), through to the race going green.
    let roster = host
        .recv_until(|msg| match msg {
            ServerMessage::SessionRoster(data) => Some(data),
            _ => None,
        })
        .await;
    assert_eq!(roster.entries.len(), 3);
    assert!(roster.entries.iter().all(|e| e.is_ai));
    host.recv_until(|msg| match msg {
        ServerMessage::TelemetryCompact(t) if t.session_state == SessionState::Racing => Some(()),
        _ => None,
    })
    .await;

    // Nobody else sees it or can take a seat in it.
    let (mut other, other_lobby) = Client::connect("Bystander", server.tcp_addr).await;
    assert!(
        other_lobby
            .available_sessions
            .iter()
            .all(|s| s.id != session_id),
        "a demo session is unlisted"
    );
    other
        .send(&ClientMessage::SelectCar {
            car_config_id: lobby.car_configs.first().expect("a car").id,
        })
        .await;
    other.send(&ClientMessage::JoinSession { session_id }).await;
    other
        .recv_until(|msg| match msg {
            ServerMessage::Error { .. } => Some(()),
            ServerMessage::SessionJoined(_) => panic!("joined somebody's demo session"),
            _ => None,
        })
        .await;

    // It outlives ticks with no human participant, then ends with its spectator.
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(server.state.read().await.sessions.contains_key(&session_id));
    host.send(&ClientMessage::LeaveSession).await;
    host.recv_until(|msg| match msg {
        ServerMessage::SessionLeft => Some(()),
        _ => None,
    })
    .await;
    assert!(
        !server.state.read().await.sessions.contains_key(&session_id),
        "the demo session is removed when its spectator leaves"
    );
}

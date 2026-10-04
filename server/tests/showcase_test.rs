//! The showcase endpoint (`crate::showcase`, docs/SPECTATOR.md): a rendered
//! race played in a loop to any number of viewers. Two clients watch one
//! channel on one clock, a late joiner gets the header and roster first, the
//! end of the content bumps the epoch, and leaving (asked for, or implied by
//! creating a session) takes a viewer off.

mod common;

use std::net::SocketAddr;
use std::path::Path;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{timeout, Instant};

use apexsim_server::config::ServerConfig;
use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage, SpectatorKind};
use apexsim_server::replay_tools::{render_stream, RenderOptions, SimulateOptions};
use apexsim_server::server::{run_server, ServerHandle};
use apexsim_server::spectator::{self, Record};

struct Client {
    stream: TcpStream,
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

    async fn recv_until<T>(&mut self, mut pick: impl FnMut(ServerMessage) -> Option<T>) -> T {
        let deadline = Instant::now() + Duration::from_secs(30);
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

    /// The next stream records, decoded (one TCP message holds a run).
    async fn recv_records(&mut self) -> Vec<Record> {
        self.recv_until(|msg| match msg {
            ServerMessage::SpectatorRecord(run) => Some(
                spectator::split_framed(&run.0)
                    .expect("framed")
                    .iter()
                    .map(|body| Record::decode(body).expect("a record"))
                    .collect(),
            ),
            _ => None,
        })
        .await
    }

    /// Records until `pick` accepts one.
    async fn recv_record<T>(&mut self, mut pick: impl FnMut(&Record) -> Option<T>) -> T {
        loop {
            for record in self.recv_records().await {
                if let Some(found) = pick(&record) {
                    return found;
                }
            }
        }
    }
}

/// A three-second race of three cars, rendered into `dir`.
fn render_into(dir: &Path, name: &str) -> std::path::PathBuf {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let opts = RenderOptions {
        race: SimulateOptions {
            track_path: root.join("content/tracks/default/Zandvoort.yaml"),
            cars_dir: root.join("content/cars/default"),
            host_car: "yotota-lmp2".into(),
            same_car: false,
            ai_count: 3,
            laps: 1,
            max_seconds: 2.0,
            countdown_seconds: 1,
            conditions: SessionConditions::DEFAULT,
            tick_rate: 240,
            record_hz: 30,
            seed: Some(3),
        },
        tail_seconds: 0.0,
        from_tick: None,
        to_tick: None,
    };
    let path = dir.join(format!("{name}.apxs"));
    render_stream(&opts)
        .expect("renders")
        .content
        .write_file(&path)
        .expect("writes");
    path
}

async fn start_server_with_showcase(dir: &Path) -> ServerHandle {
    let mut config = ServerConfig::default();
    config.network.tcp_bind = "127.0.0.1:0".to_string();
    config.network.udp_bind = "127.0.0.1:0".to_string();
    config.network.health_bind = "127.0.0.1:0".to_string();
    config.network.require_tls = false;
    config.content.skip_imported_tracks = cfg!(debug_assertions);
    config.showcase.dir = dir.to_string_lossy().into_owned();
    run_server(config).await.expect("server starts")
}

async fn http_get(addr: SocketAddr, path: &str) -> String {
    let mut stream = TcpStream::connect(addr).await.expect("connect failed");
    let request = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    stream.write_all(request.as_bytes()).await.unwrap();
    let mut response = Vec::new();
    stream.read_to_end(&mut response).await.unwrap();
    String::from_utf8_lossy(&response).to_string()
}

#[tokio::test]
async fn a_showcase_is_played_to_everyone_on_one_clock() {
    let dir = tempfile::TempDir::new().unwrap();
    render_into(dir.path(), "Zandvoort.lmp2.test");
    let server = start_server_with_showcase(dir.path()).await;

    let (mut first, lobby) = Client::connect("First", server.tcp_addr).await;
    assert!(lobby.showcase_available, "the lobby says there is one");

    first.send(&ClientMessage::ListShowcases).await;
    let listed = first
        .recv_until(|msg| match msg {
            ServerMessage::Showcases(data) => Some(data),
            _ => None,
        })
        .await;
    assert_eq!(listed.entries.len(), 1);
    let entry = &listed.entries[0];
    assert_eq!(entry.id, "Zandvoort.lmp2.test");
    assert_eq!(entry.class, "LMP2");
    assert_eq!(entry.cars, 3);
    assert_eq!(entry.viewers, 0);
    assert!(
        entry.duration_s > 2.0 && entry.duration_s < 4.0,
        "{}",
        entry.duration_s
    );

    // Join: SpectatorJoined, then the preamble as one run of records.
    first
        .send(&ClientMessage::SpectateShowcase { id: None })
        .await;
    let joined = first
        .recv_until(|msg| match msg {
            ServerMessage::SpectatorJoined(data) => Some(data),
            ServerMessage::Error { message, .. } => panic!("refused: {message}"),
            _ => None,
        })
        .await;
    assert_eq!(joined.kind, SpectatorKind::Showcase);
    assert_eq!(joined.showcase_id, "Zandvoort.lmp2.test");
    let preamble = first.recv_records().await;
    let Record::Header(header) = &preamble[0] else {
        panic!("the header comes first, not {:?}", preamble[0]);
    };
    assert_eq!(header.epoch, 1, "the first epoch a server hands out");
    assert_eq!(header.stream_id, joined.stream_id);
    assert_eq!(header.track.stem, "Zandvoort");
    assert!(matches!(&preamble[1], Record::Roster(r) if r.entries.len() == 3 && r.epoch == 1));
    assert!(matches!(&preamble[2], Record::Path(_)));
    assert!(matches!(
        &preamble[3],
        Record::Event(e) if matches!(e.kind, spectator::EventKind::TrackSectors { .. })
    ));
    // Frames follow on TCP (no UDP handshake here), from the start.
    let frame = first
        .recv_record(|r| match r {
            Record::Frame(f) => Some(f.clone()),
            _ => None,
        })
        .await;
    assert_eq!(frame.epoch, 1);
    assert_eq!(frame.tick, header.start_tick);
    assert_eq!(frame.cars(header.row_size as usize).count(), 3);

    // A second viewer a moment later: the header first, then frames from
    // where the channel is now, not from the start.
    tokio::time::sleep(Duration::from_millis(600)).await;
    let (mut second, _) = Client::connect("Second", server.tcp_addr).await;
    second
        .send(&ClientMessage::SpectateShowcase {
            id: Some("zandvoort.LMP2.test".into()),
        })
        .await;
    second
        .recv_until(|msg| match msg {
            ServerMessage::SpectatorJoined(_) => Some(()),
            ServerMessage::Error { message, .. } => panic!("refused: {message}"),
            _ => None,
        })
        .await;
    let preamble = second.recv_records().await;
    assert!(matches!(&preamble[0], Record::Header(h) if h.epoch == 1));
    let late = second
        .recv_record(|r| match r {
            Record::Frame(f) => Some(f.clone()),
            _ => None,
        })
        .await;
    assert!(
        late.tick > header.start_tick + 60,
        "a late joiner sees the race where it is ({} vs {})",
        late.tick,
        header.start_tick
    );
    let listed = {
        first.send(&ClientMessage::ListShowcases).await;
        first
            .recv_until(|msg| match msg {
                ServerMessage::Showcases(data) => Some(data),
                _ => None,
            })
            .await
    };
    assert_eq!(listed.entries[0].viewers, 2);

    // The content ends in a few seconds; the channel starts over under
    // the next epoch, header first, and both viewers see it.
    let looped = first
        .recv_record(|r| match r {
            Record::Header(h) if h.epoch == 2 => Some(h.clone()),
            _ => None,
        })
        .await;
    assert_eq!(looped.stream_id, header.stream_id);
    let restarted = first
        .recv_record(|r| match r {
            Record::Frame(f) if f.epoch == 2 => Some(f.clone()),
            _ => None,
        })
        .await;
    assert_eq!(restarted.tick, header.start_tick);
    second
        .recv_record(|r| match r {
            Record::Header(h) if h.epoch == 2 => Some(()),
            _ => None,
        })
        .await;

    // Leaving, asked for and implied.
    second.send(&ClientMessage::LeaveSpectate).await;
    first
        .send(&ClientMessage::SelectCar {
            car_config_id: lobby.car_configs.first().expect("a car").id,
            livery: 0,
        })
        .await;
    first
        .send(&ClientMessage::CreateSession {
            track_config_id: header.track.track_id,
            max_players: 4,
            ai_count: 0,
            lap_limit: 1,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: Default::default(),
            conditions: Default::default(),
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
        })
        .await;
    first
        .recv_until(|msg| match msg {
            ServerMessage::SessionJoined(_) => Some(()),
            ServerMessage::Error { message, .. } => panic!("create failed: {message}"),
            _ => None,
        })
        .await;
    tokio::time::sleep(Duration::from_millis(1200)).await;
    let state = server.state.read().await;
    assert!(state.showcase.summaries()[0].viewers == 0, "everyone left");
    assert!(!state.showcase.has_viewers());
    drop(state);

    // In a session, a showcase is refused.
    first
        .send(&ClientMessage::SpectateShowcase { id: None })
        .await;
    let code = first
        .recv_until(|msg| match msg {
            ServerMessage::Error { code, .. } => Some(code),
            ServerMessage::SpectatorJoined(_) => panic!("watched from inside a session"),
            _ => None,
        })
        .await;
    assert_eq!(code, 400);

    // The HTTP side lists the channel and counts viewers.
    let body = http_get(server.health_addr, "/showcase").await;
    assert!(body.starts_with("HTTP/1.1 200"), "{body}");
    assert!(body.contains(r#""id":"Zandvoort.lmp2.test""#), "{body}");
    let metrics = http_get(server.health_addr, "/metrics").await;
    assert!(
        metrics.contains(r#"apexsim_showcase_viewers{showcase="Zandvoort.lmp2.test"} 0"#),
        "{metrics}"
    );
    assert!(metrics.contains("apexsim_showcase_frames_sent"));

    // An unknown channel.
    let (mut third, _) = Client::connect("Third", server.tcp_addr).await;
    third
        .send(&ClientMessage::SpectateShowcase {
            id: Some("Nowhere".into()),
        })
        .await;
    let code = third
        .recv_until(|msg| match msg {
            ServerMessage::Error { code, .. } => Some(code),
            _ => None,
        })
        .await;
    assert_eq!(code, 404);
}

#[tokio::test]
async fn a_server_without_showcases_says_so() {
    let server = common::start_test_server().await;
    let (mut client, lobby) = Client::connect("Nobody", server.tcp_addr).await;
    assert!(!lobby.showcase_available);
    client
        .send(&ClientMessage::SpectateShowcase { id: None })
        .await;
    let code = client
        .recv_until(|msg| match msg {
            ServerMessage::Error { code, .. } => Some(code),
            _ => None,
        })
        .await;
    assert_eq!(code, 404);
    let body = http_get(server.health_addr, "/showcase").await;
    assert!(body.contains(r#"{"showcases":[]}"#), "{body}");
}

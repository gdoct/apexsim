//! A live session watched as a spectator stream (`SpectateSession`): the
//! viewer is sent the preamble and what has happened so far in one message,
//! then the stream's records as the session ticks; and a session under way
//! is being recorded as an `.apxs` replay.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{
    ClientMessage, LobbyStateData, ServerMessage, SessionJoinedData, SpectatorKind,
};
use apexsim_server::spectator::{self, Record};

const TEST_TIMEOUT: Duration = Duration::from_secs(40);

struct Client {
    tcp: TcpStream,
}

impl Client {
    async fn connect(
        name: &str,
        addr: SocketAddr,
    ) -> Result<(Self, LobbyStateData), Box<dyn std::error::Error>> {
        let mut client = Client {
            tcp: TcpStream::connect(addr).await?,
        };
        client
            .send(&ClientMessage::Authenticate {
                token: format!("test_token_{name}"),
                player_name: name.to_string(),
                protocol_version: apexsim_server::network::PROTOCOL_VERSION,
                resume_token: None,
            })
            .await?;
        match client.recv().await? {
            ServerMessage::AuthSuccess(_) => {}
            other => return Err(format!("expected AuthSuccess, got {other:?}").into()),
        }
        match client.recv().await? {
            ServerMessage::LobbyState(lobby) => Ok((client, lobby)),
            other => Err(format!("expected LobbyState, got {other:?}").into()),
        }
    }

    async fn send(&mut self, msg: &ClientMessage) -> Result<(), Box<dyn std::error::Error>> {
        let data = rmp_serde::to_vec_named(msg)?;
        self.tcp
            .write_all(&(data.len() as u32).to_be_bytes())
            .await?;
        self.tcp.write_all(&data).await?;
        self.tcp.flush().await?;
        Ok(())
    }

    async fn recv(&mut self) -> Result<ServerMessage, Box<dyn std::error::Error>> {
        let mut len = [0u8; 4];
        self.tcp.read_exact(&mut len).await?;
        let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
        self.tcp.read_exact(&mut buf).await?;
        Ok(rmp_serde::from_slice(&buf)?)
    }

    async fn wait_joined(&mut self) -> Result<SessionJoinedData, Box<dyn std::error::Error>> {
        for _ in 0..200 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::SessionJoined(data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no SessionJoined".into())
    }
}

#[tokio::test]
async fn a_live_session_is_watched_as_a_stream_and_recorded() {
    let replays = tempfile::TempDir::new().unwrap();
    let result = timeout(TEST_TIMEOUT, async {
        let mut config = apexsim_server::config::ServerConfig::default();
        config.records.replay_dir = replays.path().to_string_lossy().into_owned();
        let server = common::start_test_server_with_config(config).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;
        host.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 4,
            ai_count: 3,
            lap_limit: 4,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
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
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
        })
        .await?;
        let joined = host.wait_joined().await?;
        host.send(&ClientMessage::StartCountdown {
            countdown_seconds: 1,
            next_mode: GameMode::Race,
        })
        .await?;
        sleep(Duration::from_millis(2500)).await;

        // An unknown session is refused.
        let (mut watcher, _) = Client::connect("Watcher", server.tcp_addr).await?;
        watcher
            .send(&ClientMessage::SpectateSession {
                session_id: SessionId::nil(),
            })
            .await?;
        loop {
            match timeout(Duration::from_secs(5), watcher.recv()).await?? {
                ServerMessage::Error { code, .. } => {
                    assert_eq!(code, 404);
                    break;
                }
                ServerMessage::SpectatorJoined(_) => {
                    return Err("the nil session was joined".into())
                }
                _ => {}
            }
        }

        watcher
            .send(&ClientMessage::SpectateSession {
                session_id: joined.session_id,
            })
            .await?;
        let mut kinds = Vec::new();
        let mut header = None;
        let mut roster = None;
        let mut has_state = false;
        let mut frame_cars = 0usize;
        let mut sectors = false;
        let mut live = false;
        for _ in 0..600 {
            match timeout(Duration::from_secs(5), watcher.recv()).await?? {
                ServerMessage::SpectatorJoined(data) => {
                    live = data.kind == SpectatorKind::Live;
                    assert_eq!(data.stream_id, joined.session_id);
                }
                ServerMessage::SpectatorRecord(run) => {
                    for body in spectator::split_framed(&run.0)? {
                        kinds.push(spectator::record_type(&body));
                        match Record::decode(&body)? {
                            Record::Header(h) => header = Some(h),
                            Record::Roster(r) => roster = Some(r),
                            Record::State(_) => has_state = true,
                            Record::Frame(f) => frame_cars += f.cars(spectator::ROW_SIZE).count(),
                            Record::Event(e) => {
                                sectors |=
                                    matches!(e.kind, spectator::EventKind::TrackSectors { .. })
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
            if header.is_some() && roster.is_some() && has_state && frame_cars > 0 && sectors {
                break;
            }
        }
        assert!(live, "the join says the stream is live");
        let header = header.ok_or("no header")?;
        assert_eq!(header.session_kind, SessionKind::Multiplayer);
        assert!(header.track.length_m > 1000.0);
        assert_eq!(roster.ok_or("no roster")?.entries.len(), 4);
        assert!(has_state, "the slow state came with the catch-up or after");
        assert!(sectors, "the sector lines came with the preamble");
        assert!(
            frame_cars >= 4,
            "a frame of the field followed: {frame_cars}"
        );
        // The preamble opened the message: header, roster, path.
        assert_eq!(
            &kinds[..3],
            &[
                Some(spectator::RECORD_HEADER),
                Some(spectator::RECORD_ROSTER),
                Some(spectator::RECORD_PATH)
            ]
        );

        // The race is being recorded: its blocks are on disk as it runs.
        {
            let state = server.state.read().await;
            assert!(state.replay.is_recording(&joined.session_id));
        }
        let names: Vec<String> = std::fs::read_dir(replays.path())?
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        assert!(
            names.iter().any(|n| n.starts_with(".recording_")),
            "{names:?}"
        );

        // Leaving ends it, and the server forgets the viewer.
        watcher.send(&ClientMessage::LeaveSpectate).await?;
        sleep(Duration::from_millis(200)).await;
        assert!(server.state.read().await.live.is_empty());

        host.send(&ClientMessage::Disconnect).await?;
        watcher.send(&ClientMessage::Disconnect).await?;
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

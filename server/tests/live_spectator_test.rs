//! A spectator who joins a session already under way is given what the
//! client needs to draw it: the roster (without which every telemetry frame
//! is dropped, since none of its cars can be placed), the sector lines, and
//! the race distance in the lobby's listing.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage, SessionJoinedData};

const TEST_TIMEOUT: Duration = Duration::from_secs(30);

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

    /// The next `SessionJoined`, skipping whatever else the server sends.
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
async fn a_spectator_joining_mid_race_gets_the_roster_and_the_sectors() {
    let result = timeout(TEST_TIMEOUT, async {
        let server = common::start_test_server().await;
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
        })
        .await?;
        let joined = host.wait_joined().await?;
        host.send(&ClientMessage::StartCountdown {
            countdown_seconds: 1,
            next_mode: GameMode::Race,
        })
        .await?;
        // Long enough for the host's roster to have gone out and the session
        // to be racing: the spectator arrives after the roster was sent.
        sleep(Duration::from_millis(1500)).await;
        let (expected_cars, racing) = {
            let state = server.state.read().await;
            let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
            (
                session.session.participants.len(),
                session.session.state == SessionState::Racing,
            )
        };
        assert_eq!(expected_cars, 4, "the host and three AI");
        assert!(racing, "the race is under way before the spectator comes");

        let (mut spectator, lobby) = Client::connect("Watcher", server.tcp_addr).await?;
        let listed = lobby
            .available_sessions
            .iter()
            .find(|s| s.id == joined.session_id)
            .ok_or("session not listed")?;
        assert_eq!(listed.lap_limit, 4, "the listing carries the race distance");
        assert_eq!(listed.state, SessionState::Racing, "and that it is racing");

        spectator
            .send(&ClientMessage::JoinAsSpectator {
                session_id: joined.session_id,
            })
            .await?;
        let seat = spectator.wait_joined().await?;
        assert_eq!(seat.session_id, joined.session_id);
        assert_eq!(seat.your_grid_position, 0, "0 is a spectator");

        let mut sectors = None;
        let mut roster = None;
        for _ in 0..400 {
            match timeout(Duration::from_secs(5), spectator.recv()).await?? {
                ServerMessage::TrackSectors(data) => sectors = Some(data),
                ServerMessage::SessionRoster(data) => roster = Some(data),
                _ => {}
            }
            if sectors.is_some() && roster.is_some() {
                break;
            }
        }
        let sectors = sectors.ok_or("no TrackSectors for the spectator")?;
        assert_eq!(sectors.session_id, joined.session_id);
        assert!(sectors.track_length_m > 0.0);
        assert_eq!(sectors.boundaries_m.len(), 2, "three sectors");
        let roster = roster.ok_or("no SessionRoster for the spectator")?;
        assert_eq!(roster.session_id, joined.session_id);
        assert_eq!(roster.entries.len(), expected_cars);

        host.send(&ClientMessage::Disconnect).await?;
        spectator.send(&ClientMessage::Disconnect).await?;
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

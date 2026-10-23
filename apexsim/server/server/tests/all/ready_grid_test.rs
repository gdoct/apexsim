//! The race's grid before the countdown, over the wire: only the host puts
//! it up, every driver hears it, the roster carries who is ready, and the
//! countdown starts once everyone is.

use crate::common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{
    ClientMessage, LobbyStateData, ServerMessage, SessionJoinedData, SessionRosterData,
};

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
                resume_token: None,
                driver_id: None,
                driver_key: None,
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

    /// The first message `pick` takes, skipping whatever else comes.
    async fn wait_for<T>(
        &mut self,
        what: &str,
        mut pick: impl FnMut(ServerMessage) -> Option<T>,
    ) -> Result<T, Box<dyn std::error::Error>> {
        for _ in 0..400 {
            if let Some(found) = pick(timeout(Duration::from_secs(5), self.recv()).await??) {
                return Ok(found);
            }
        }
        Err(format!("no {what}").into())
    }

    async fn wait_joined(&mut self) -> Result<SessionJoinedData, Box<dyn std::error::Error>> {
        self.wait_for("SessionJoined", |m| match m {
            ServerMessage::SessionJoined(data) => Some(data),
            _ => None,
        })
        .await
    }

    async fn wait_roster(
        &mut self,
        mut ok: impl FnMut(&SessionRosterData) -> bool,
    ) -> Result<SessionRosterData, Box<dyn std::error::Error>> {
        self.wait_for("SessionRoster", |m| match m {
            ServerMessage::SessionRoster(data) if ok(&data) => Some(data),
            _ => None,
        })
        .await
    }
}

#[tokio::test]
async fn the_grid_waits_for_every_driver_then_counts_into_the_race() {
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
            max_players: 6,
            ai_count: 1,
            lap_limit: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
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
        let (mut guest, _) = Client::connect("Guest", server.tcp_addr).await?;
        guest
            .send(&ClientMessage::SelectCar {
                car_config_id: car_id,
                livery: 0,
            })
            .await?;
        sleep(Duration::from_millis(50)).await;
        guest
            .send(&ClientMessage::JoinSession {
                session_id: joined.session_id,
            })
            .await?;
        guest.wait_joined().await?;

        // Only the host puts the grid up.
        guest
            .send(&ClientMessage::OpenGrid {
                countdown_seconds: 3,
            })
            .await?;
        let code = guest
            .wait_for("Error", |m| match m {
                ServerMessage::Error { code, .. } => Some(code),
                _ => None,
            })
            .await?;
        assert_eq!(code, 403);

        host.send(&ClientMessage::OpenGrid {
            countdown_seconds: 3,
        })
        .await?;
        for client in [&mut host, &mut guest] {
            client
                .wait_for("GameModeChanged Grid", |m| match m {
                    ServerMessage::GameModeChanged {
                        mode: GameMode::Grid,
                    } => Some(()),
                    _ => None,
                })
                .await?;
        }

        // The host is ready: the guest's roster says so, and the grid waits.
        host.send(&ClientMessage::SetReady { ready: true }).await?;
        let roster = guest
            .wait_roster(|r| r.entries.iter().any(|e| e.ready))
            .await?;
        let ready: Vec<&str> = roster
            .entries
            .iter()
            .filter(|e| e.ready)
            .map(|e| e.player_name.as_str())
            .collect();
        assert_eq!(ready, ["Host"]);
        sleep(Duration::from_millis(200)).await;
        {
            let state = server.state.read().await;
            let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
            assert_eq!(session.session.game_mode, GameMode::Grid);
        }

        // The guest repaints and is ready too: the countdown starts.
        guest.send(&ClientMessage::SetLivery { livery: 1 }).await?;
        guest.send(&ClientMessage::SetReady { ready: true }).await?;
        sleep(Duration::from_millis(300)).await;
        let state = server.state.read().await;
        let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
        assert_eq!(session.session.game_mode, GameMode::Countdown);
        assert_eq!(session.session.next_mode, Some(GameMode::Race));
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

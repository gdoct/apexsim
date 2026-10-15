//! A race weekend over the real protocol: the format echoed on join, the
//! weekend's state to every member, the host's `AdvanceWeekend` through
//! practice and a two-round knockout into the race, and the refusals.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{
    ClientMessage, LobbyStateData, ServerMessage, SessionJoinedData, WeekendStateData,
};
use apexsim_server::weekend::WeekendFormat;

const TEST_TIMEOUT: Duration = Duration::from_secs(60);

type TestResult<T> = Result<T, Box<dyn std::error::Error>>;

struct Client {
    tcp: TcpStream,
}

impl Client {
    async fn connect(name: &str, addr: SocketAddr) -> TestResult<(Self, LobbyStateData)> {
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

    async fn send(&mut self, msg: &ClientMessage) -> TestResult<()> {
        let data = rmp_serde::to_vec_named(msg)?;
        self.tcp
            .write_all(&(data.len() as u32).to_be_bytes())
            .await?;
        self.tcp.write_all(&data).await?;
        self.tcp.flush().await?;
        Ok(())
    }

    async fn recv(&mut self) -> TestResult<ServerMessage> {
        let mut len = [0u8; 4];
        self.tcp.read_exact(&mut len).await?;
        let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
        self.tcp.read_exact(&mut buf).await?;
        Ok(rmp_serde::from_slice(&buf)?)
    }

    /// The next `SessionJoined`, skipping whatever else the server sends.
    async fn wait_joined(&mut self) -> TestResult<SessionJoinedData> {
        for _ in 0..200 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::SessionJoined(data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no SessionJoined".into())
    }

    /// The next `WeekendState` that `pred` accepts.
    async fn wait_weekend(
        &mut self,
        pred: impl Fn(&WeekendStateData) -> bool,
    ) -> TestResult<WeekendStateData> {
        for _ in 0..2000 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::WeekendState(data) if pred(&data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no such WeekendState".into())
    }

    /// The next `Error`'s code.
    async fn wait_error(&mut self) -> TestResult<u16> {
        for _ in 0..2000 {
            if let ServerMessage::Error { code, .. } =
                timeout(Duration::from_secs(5), self.recv()).await??
            {
                return Ok(code);
            }
        }
        Err("no Error".into())
    }
}

#[tokio::test]
async fn a_weekend_runs_from_the_lobby_to_the_race_over_the_wire() {
    let result = timeout(TEST_TIMEOUT, async {
        let records_dir =
            std::env::temp_dir().join(format!("apexsim-weekend-{}", uuid::Uuid::new_v4()));
        let server = common::start_test_server_with_records(&records_dir).await;
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
            ai_count: 2,
            lap_limit: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            // Ignored beside a weekend.
            qualifying_seconds: Some(300),
            weekend: Some(WeekendFormat {
                practice_seconds: 10,
                qualifying_seconds: vec![600, 600, 600, 600],
                practice_conditions: None,
                qualifying_conditions: None,
            }),
            watch: None,
            qualifying_cutoff_pct: None,
        })
        .await?;
        let joined = host.wait_joined().await?;
        let format = WeekendFormat {
            practice_seconds: 60,
            qualifying_seconds: vec![600, 600, 600],
            practice_conditions: None,
            qualifying_conditions: None,
        };
        assert_eq!(joined.weekend, Some(format.clone()));
        assert_eq!(joined.qualifying_seconds, None);
        let lobby_state = host.wait_weekend(|_| true).await?;
        assert_eq!(
            (lobby_state.phase, lobby_state.round, lobby_state.rounds),
            (0, 0, 3)
        );
        let session_id = joined.session_id;

        let (mut guest, _) = Client::connect("Guest", server.tcp_addr).await?;
        guest
            .send(&ClientMessage::SelectCar {
                car_config_id: car_id,
                livery: 0,
            })
            .await?;
        sleep(Duration::from_millis(50)).await;
        guest
            .send(&ClientMessage::JoinSession { session_id })
            .await?;
        let guest_joined = guest.wait_joined().await?;
        assert_eq!(guest_joined.weekend, Some(format));
        assert_eq!(guest.wait_weekend(|_| true).await?.phase, 0);

        // Only the host advances.
        guest.send(&ClientMessage::AdvanceWeekend).await?;
        assert_eq!(guest.wait_error().await?, 403);

        // Practice, which may not be skipped while it runs.
        host.send(&ClientMessage::AdvanceWeekend).await?;
        let practice = host.wait_weekend(|w| w.phase == 1).await?;
        assert!(!practice.finished);
        guest.wait_weekend(|w| w.phase == 1).await?;
        host.send(&ClientMessage::AdvanceWeekend).await?;
        assert_eq!(host.wait_error().await?, 409);
        {
            let mut state = server.state.write().await;
            let session = state.sessions.get_mut(&session_id).ok_or("no session")?;
            assert_eq!(session.session.game_mode, GameMode::Practice);
            session.session.state = SessionState::Finished;
        }
        host.wait_weekend(|w| w.phase == 1 && w.finished).await?;

        // Q1: four cars, a knockout to three, then to two.
        host.send(&ClientMessage::AdvanceWeekend).await?;
        let q1 = host.wait_weekend(|w| w.phase == 2).await?;
        assert_eq!((q1.round, q1.rounds, q1.advancing), (1, 3, 3));
        let (host_id, guest_id) = {
            let state = server.state.read().await;
            let session = state.sessions.get(&session_id).ok_or("no session")?;
            assert_eq!(session.session.game_mode, GameMode::Qualification);
            let host_id = session.session.host_player_id;
            let guest_id = *session
                .session
                .participants
                .keys()
                .find(|id| **id != host_id && !session.session.ai_player_ids.contains(id))
                .ok_or("no guest")?;
            (host_id, guest_id)
        };

        // Nobody new drives while a round runs.
        let (mut late, _) = Client::connect("Late", server.tcp_addr).await?;
        late.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;
        late.send(&ClientMessage::JoinSession { session_id })
            .await?;
        assert_eq!(late.wait_error().await?, 409);

        // The AI set no time: the later-seated one goes out first.
        {
            let mut state = server.state.write().await;
            let session = state.sessions.get_mut(&session_id).ok_or("no session")?;
            session.note_qualifying_lap(host_id, 80_000);
            session.note_qualifying_lap(guest_id, 81_000);
            session.session.state = SessionState::Finished;
        }
        let q1_over = host.wait_weekend(|w| w.phase == 2 && w.finished).await?;
        assert_eq!(q1_over.eliminated.len(), 1);
        assert_eq!(q1_over.eliminated[0].round, 1);
        assert_eq!(q1_over.eliminated[0].grid_position, 4);
        assert_eq!(q1_over.eliminated[0].lap_time_ms, 0);

        // Between rounds the weekend takes the late driver: no part in
        // the knockout, the back of the grid.
        late.send(&ClientMessage::JoinSession { session_id })
            .await?;
        let late_joined = late.wait_joined().await?;
        assert_eq!(late_joined.session_id, session_id);
        let late_state = late.wait_weekend(|w| w.eliminated.len() == 2).await?;
        assert_eq!(late_state.eliminated[1].grid_position, 5);
        assert!(late_state.eliminated[1].late && !late_state.eliminated[0].late);

        host.send(&ClientMessage::AdvanceWeekend).await?;
        let q2 = host.wait_weekend(|w| w.phase == 2 && w.round == 2).await?;
        assert_eq!((q2.advancing, q2.finished), (2, false));
        {
            let mut state = server.state.write().await;
            let session = state.sessions.get_mut(&session_id).ok_or("no session")?;
            session.note_qualifying_lap(guest_id, 79_000);
            session.note_qualifying_lap(host_id, 79_500);
            session.session.state = SessionState::Finished;
        }
        let q2_over = host.wait_weekend(|w| w.round == 2 && w.finished).await?;
        assert_eq!(q2_over.eliminated.len(), 3);
        assert_eq!(q2_over.eliminated[0].round, 2);
        assert_eq!(q2_over.eliminated[0].grid_position, 3);

        host.send(&ClientMessage::AdvanceWeekend).await?;
        let q3 = host.wait_weekend(|w| w.round == 3).await?;
        assert_eq!(q3.advancing, 0);
        {
            let mut state = server.state.write().await;
            let session = state.sessions.get_mut(&session_id).ok_or("no session")?;
            session.note_qualifying_lap(guest_id, 78_000);
            session.note_qualifying_lap(host_id, 78_200);
            session.session.state = SessionState::Finished;
        }
        host.wait_weekend(|w| w.round == 3 && w.finished).await?;

        // The race: its grid goes up (tyres, setup and livery after
        // qualifying), gridded by the weekend.
        host.send(&ClientMessage::AdvanceWeekend).await?;
        let race = host.wait_weekend(|w| w.phase == 3).await?;
        assert_eq!(race.eliminated.len(), 3);
        {
            let state = server.state.read().await;
            let session = state.sessions.get(&session_id).ok_or("no session")?;
            assert_eq!(session.session.game_mode, GameMode::Grid);
            assert_eq!(session.session.next_mode, Some(GameMode::Race));
            let slot = |id: &PlayerId| session.session.participants[id].grid_position;
            assert_eq!((slot(&guest_id), slot(&host_id)), (1, 2));
        }
        host.send(&ClientMessage::AdvanceWeekend).await?;
        assert_eq!(host.wait_error().await?, 409);
        let _ = std::fs::remove_dir_all(&records_dir);
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

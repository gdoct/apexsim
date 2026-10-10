//! Session state seen from outside: who may join a session under way and
//! what the lobby lists for it (a race under way takes no driver, a
//! spectator may watch it, a running practice or hotlap takes drivers),
//! `StartSession` counting into a race, and the heartbeat's server tick.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage, SessionJoinedData};
use apexsim_server::server::ServerHandle;

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
        for _ in 0..50 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::SessionJoined(data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no SessionJoined".into())
    }
}

/// The next `Error` code, skipping whatever else arrives.
async fn wait_error(client: &mut Client) -> Result<u16, Box<dyn std::error::Error>> {
    for _ in 0..50 {
        match timeout(Duration::from_secs(5), client.recv()).await?? {
            ServerMessage::Error { code, .. } => return Ok(code),
            ServerMessage::SessionJoined(_) => return Err("joined".into()),
            _ => continue,
        }
    }
    Err("no Error".into())
}

/// A host in a session with one AI car: the host, the session and the car.
async fn host_session(
    server: &ServerHandle,
) -> Result<(Client, SessionId, CarConfigId), Box<dyn std::error::Error>> {
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
    })
    .await?;
    let joined = host.wait_joined().await?;
    Ok((host, joined.session_id, car_id))
}

/// A second player with a car picked, in the lobby.
async fn guest(
    server: &ServerHandle,
    car_id: CarConfigId,
) -> Result<Client, Box<dyn std::error::Error>> {
    let (mut guest, _) = Client::connect("Guest", server.tcp_addr).await?;
    guest
        .send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
    sleep(Duration::from_millis(50)).await;
    Ok(guest)
}

/// What the session browser lists as the session's state.
async fn listed_state(server: &ServerHandle, session_id: SessionId) -> Option<SessionState> {
    let state = server.state.read().await;
    state
        .lobby
        .get_available_sessions()
        .await
        .into_iter()
        .find(|s| s.id == session_id)
        .map(|s| s.state)
}

type TestResult = Result<Result<(), Box<dyn std::error::Error>>, tokio::time::error::Elapsed>;

fn finish(result: TestResult) {
    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

#[tokio::test]
async fn a_race_under_way_takes_spectators_but_no_drivers() {
    finish(
        timeout(TEST_TIMEOUT, async {
            let server = common::start_test_server().await;
            let (mut host, session_id, car_id) = host_session(&server).await?;
            assert_eq!(
                listed_state(&server, session_id).await,
                Some(SessionState::Lobby)
            );

            host.send(&ClientMessage::StartCountdown {
                countdown_seconds: 1,
                next_mode: GameMode::Race,
            })
            .await?;
            sleep(Duration::from_millis(1600)).await;
            assert_eq!(
                listed_state(&server, session_id).await,
                Some(SessionState::Racing),
                "the lobby lists the race as racing"
            );

            let mut driver = guest(&server, car_id).await?;
            driver
                .send(&ClientMessage::JoinSession { session_id })
                .await?;
            assert_eq!(wait_error(&mut driver).await?, 409);
            {
                let state = server.state.read().await;
                let session = state.sessions.get(&session_id).ok_or("no session")?;
                assert_eq!(session.session.participants.len(), 2, "host and AI only");
            }

            driver
                .send(&ClientMessage::JoinAsSpectator { session_id })
                .await?;
            let watching = driver.wait_joined().await?;
            assert_eq!(watching.your_grid_position, 0);
            Ok(())
        })
        .await,
    );
}

#[tokio::test]
async fn a_running_practice_or_hotlap_takes_drivers() {
    finish(
        timeout(TEST_TIMEOUT, async {
            let server = common::start_test_server().await;
            for mode in [GameMode::FreePractice, GameMode::Hotlap] {
                let (mut host, session_id, car_id) = host_session(&server).await?;
                host.send(&ClientMessage::StartCountdown {
                    countdown_seconds: 0,
                    next_mode: mode,
                })
                .await?;
                sleep(Duration::from_millis(300)).await;
                assert_eq!(
                    listed_state(&server, session_id).await,
                    Some(SessionState::Racing),
                    "{mode:?}"
                );

                let mut driver = guest(&server, car_id).await?;
                driver
                    .send(&ClientMessage::JoinSession { session_id })
                    .await?;
                let joined = driver.wait_joined().await?;
                assert!(joined.your_grid_position > 0, "{mode:?}");

                let state = server.state.read().await;
                let session = state.sessions.get(&session_id).ok_or("no session")?;
                let guest_car = session
                    .session
                    .participants
                    .values()
                    .find(|c| {
                        c.player_id != session.session.host_player_id
                            && !session.session.ai_player_ids.contains(&c.player_id)
                    })
                    .ok_or("no guest car")?;
                // A hotlap driver arrives in the garage, as the host did.
                assert_eq!(guest_car.in_garage, mode == GameMode::Hotlap, "{mode:?}");
            }
            Ok(())
        })
        .await,
    );
}

#[tokio::test]
async fn start_session_counts_into_a_race() {
    finish(
        timeout(TEST_TIMEOUT, async {
            let server = common::start_test_server().await;
            let (mut host, session_id, _) = host_session(&server).await?;
            host.send(&ClientMessage::StartSession).await?;
            loop {
                match timeout(Duration::from_secs(5), host.recv()).await?? {
                    ServerMessage::SessionStarting { countdown_seconds } => {
                        assert_eq!(countdown_seconds, 5);
                        break;
                    }
                    ServerMessage::Error { message, .. } => return Err(message.into()),
                    _ => continue,
                }
            }
            {
                let state = server.state.read().await;
                let session = state.sessions.get(&session_id).ok_or("no session")?;
                assert_eq!(session.session.game_mode, GameMode::Countdown);
                assert_eq!(session.session.next_mode, Some(GameMode::Race));
            }

            // A second press, with the session under way, is refused.
            host.send(&ClientMessage::StartSession).await?;
            assert_eq!(wait_error(&mut host).await?, 409);
            Ok(())
        })
        .await,
    );
}

#[tokio::test]
async fn the_heartbeat_ack_carries_the_servers_tick() {
    finish(
        timeout(TEST_TIMEOUT, async {
            let server = common::start_test_server().await;
            let (mut client, _) = Client::connect("Beat", server.tcp_addr).await?;
            sleep(Duration::from_millis(200)).await;
            let mut ticks = Vec::new();
            for _ in 0..2 {
                client
                    .send(&ClientMessage::Heartbeat { client_tick: 1 })
                    .await?;
                loop {
                    match timeout(Duration::from_secs(5), client.recv()).await?? {
                        ServerMessage::HeartbeatAck { server_tick } => {
                            ticks.push(server_tick);
                            break;
                        }
                        _ => continue,
                    }
                }
                sleep(Duration::from_millis(100)).await;
            }
            assert!(ticks[0] > 0, "the loop has ticked");
            assert!(ticks[1] > ticks[0], "and goes on ticking: {ticks:?}");
            Ok(())
        })
        .await,
    );
}

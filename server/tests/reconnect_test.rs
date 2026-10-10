//! Reconnecting to a session: a lost connection holds the driver's seat (the
//! server drives the car), the resume token from `AuthSuccess` gets the same
//! player back with a `RejoinAvailable` offer, `JoinSession` takes the seat
//! back, and a session whose drivers are all gone ends after
//! `[server] reconnect_grace_seconds`.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout, Instant};

use apexsim_server::config::ServerConfig;
use apexsim_server::data::*;
use apexsim_server::network::{
    ClientMessage, LobbyStateData, RejoinData, ServerMessage, SessionJoinedData,
};
use apexsim_server::server::ServerHandle;

type Res<T> = Result<T, Box<dyn std::error::Error>>;

const TEST_TIMEOUT: Duration = Duration::from_secs(60);

struct Client {
    tcp: TcpStream,
    player_id: PlayerId,
    resume_token: String,
}

impl Client {
    async fn connect(
        name: &str,
        addr: SocketAddr,
        resume: Option<&str>,
    ) -> Res<(Self, LobbyStateData)> {
        let mut tcp = TcpStream::connect(addr).await?;
        send(
            &mut tcp,
            &ClientMessage::Authenticate {
                token: format!("test_token_{name}"),
                player_name: name.to_string(),
                protocol_version: apexsim_server::network::PROTOCOL_VERSION,
                resume_token: resume.map(str::to_string),
            },
        )
        .await?;
        let auth = match recv(&mut tcp).await? {
            ServerMessage::AuthSuccess(auth) => auth,
            other => return Err(format!("expected AuthSuccess, got {other:?}").into()),
        };
        let lobby = match recv(&mut tcp).await? {
            ServerMessage::LobbyState(lobby) => lobby,
            other => return Err(format!("expected LobbyState, got {other:?}").into()),
        };
        Ok((
            Client {
                tcp,
                player_id: auth.player_id,
                resume_token: auth.resume_token,
            },
            lobby,
        ))
    }

    async fn send(&mut self, msg: &ClientMessage) -> Res<()> {
        send(&mut self.tcp, msg).await
    }

    /// The next message matching `pick`, skipping the rest.
    async fn wait_for<T>(&mut self, mut pick: impl FnMut(ServerMessage) -> Option<T>) -> Res<T> {
        let deadline = Instant::now() + Duration::from_secs(10);
        while Instant::now() < deadline {
            let msg = timeout(Duration::from_secs(10), recv(&mut self.tcp)).await??;
            if let ServerMessage::Error { message, .. } = &msg {
                return Err(message.clone().into());
            }
            if let Some(found) = pick(msg) {
                return Ok(found);
            }
        }
        Err("timed out waiting for a message".into())
    }

    async fn wait_joined(&mut self) -> Res<SessionJoinedData> {
        self.wait_for(|m| match m {
            ServerMessage::SessionJoined(data) => Some(data),
            _ => None,
        })
        .await
    }

    async fn wait_rejoin(&mut self) -> Res<RejoinData> {
        self.wait_for(|m| match m {
            ServerMessage::RejoinAvailable(data) => Some(data),
            _ => None,
        })
        .await
    }
}

async fn send(tcp: &mut TcpStream, msg: &ClientMessage) -> Res<()> {
    let data = rmp_serde::to_vec_named(msg)?;
    tcp.write_all(&(data.len() as u32).to_be_bytes()).await?;
    tcp.write_all(&data).await?;
    tcp.flush().await?;
    Ok(())
}

async fn recv(tcp: &mut TcpStream) -> Res<ServerMessage> {
    let mut len = [0u8; 4];
    tcp.read_exact(&mut len).await?;
    let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
    tcp.read_exact(&mut buf).await?;
    Ok(rmp_serde::from_slice(&buf)?)
}

async fn start_server(grace_seconds: u32) -> ServerHandle {
    let mut config = ServerConfig::default();
    config.server.reconnect_grace_seconds = grace_seconds;
    common::start_test_server_with_config(config).await
}

/// `name` selects the lobby's first car and creates a race with one AI on
/// its first track, or joins `join` when given.
async fn seat(
    client: &mut Client,
    lobby: &LobbyStateData,
    join: Option<SessionId>,
) -> Res<SessionId> {
    let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
    let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
    client
        .send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
    sleep(Duration::from_millis(50)).await;
    match join {
        Some(session_id) => {
            client
                .send(&ClientMessage::JoinSession { session_id })
                .await?
        }
        None => {
            client
                .send(&ClientMessage::CreateSession {
                    track_config_id: track_id,
                    max_players: 4,
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
                .await?
        }
    }
    Ok(client.wait_joined().await?.session_id)
}

/// Polls the server until `check` holds, or fails after `within`.
async fn eventually(
    server: &ServerHandle,
    within: Duration,
    what: &str,
    check: impl Fn(&apexsim_server::server::ServerState) -> bool,
) -> Res<()> {
    let deadline = Instant::now() + within;
    while Instant::now() < deadline {
        if check(&*server.state.read().await) {
            return Ok(());
        }
        sleep(Duration::from_millis(50)).await;
    }
    Err(format!("never: {what}").into())
}

fn is_away(
    state: &apexsim_server::server::ServerState,
    session: SessionId,
    player: PlayerId,
) -> bool {
    state
        .sessions
        .get(&session)
        .is_some_and(|s| s.session.participants.contains_key(&player) && s.is_away(&player))
}

#[tokio::test]
async fn a_lost_connection_holds_the_seat_until_the_driver_rejoins() {
    timeout(TEST_TIMEOUT, async {
        let server = start_server(60).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr, None).await?;
        let session_id = seat(&mut host, &lobby, None).await?;
        let (mut guest, lobby) = Client::connect("Guest", server.tcp_addr, None).await?;
        seat(&mut guest, &lobby, Some(session_id)).await?;
        assert!(
            !guest.resume_token.is_empty(),
            "AuthSuccess hands out a resume token"
        );

        host.send(&ClientMessage::StartCountdown {
            countdown_seconds: 1,
            next_mode: GameMode::Race,
        })
        .await?;

        // The guest's connection drops mid-race: the car stays, the server's.
        let (guest_id, token) = (guest.player_id, guest.resume_token.clone());
        drop(guest);
        eventually(&server, Duration::from_secs(5), "the guest is away", |s| {
            is_away(s, session_id, guest_id)
        })
        .await?;

        // Back with the token: the same player, offered the seat.
        let (mut back, _) = Client::connect("Guest", server.tcp_addr, Some(&token)).await?;
        assert_eq!(
            back.player_id, guest_id,
            "the resume token keeps the player id"
        );
        let offer = back.wait_rejoin().await?;
        assert_eq!(offer.session_id, session_id);
        assert_eq!(offer.session_kind, SessionKind::Multiplayer);

        back.send(&ClientMessage::JoinSession { session_id })
            .await?;
        let joined = back.wait_joined().await?;
        assert_eq!(joined.session_id, session_id);
        let state = server.state.read().await;
        let session = state.sessions.get(&session_id).ok_or("session gone")?;
        assert!(session.session.participants.contains_key(&guest_id));
        assert!(
            !session.is_away(&guest_id),
            "the rejoined driver has their car back"
        );
        assert_eq!(
            session.session.participants.len(),
            3,
            "no second car for the rejoined driver"
        );
        Res::Ok(())
    })
    .await
    .expect("test timed out")
    .unwrap();
}

#[tokio::test]
async fn a_resume_before_the_old_connection_times_out_takes_it_over() {
    timeout(TEST_TIMEOUT, async {
        let server = start_server(60).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr, None).await?;
        let session_id = seat(&mut host, &lobby, None).await?;
        let host_id = host.player_id;

        // A second connection with the token while the first is still open.
        let (mut again, _) =
            Client::connect("Host", server.tcp_addr, Some(&host.resume_token)).await?;
        assert_eq!(again.player_id, host_id);
        let offer = again.wait_rejoin().await?;
        assert_eq!(offer.session_id, session_id);

        // The old connection is closed by the server.
        let mut buf = [0u8; 4096];
        let closed = timeout(Duration::from_secs(5), async {
            loop {
                match host.tcp.read(&mut buf).await {
                    Ok(0) | Err(_) => break,
                    Ok(_) => continue,
                }
            }
        })
        .await;
        assert!(closed.is_ok(), "the old connection is dropped");

        again
            .send(&ClientMessage::JoinSession { session_id })
            .await?;
        again.wait_joined().await?;
        // The old connection's end, handled after the takeover, leaves the seat alone.
        sleep(Duration::from_millis(300)).await;
        let state = server.state.read().await;
        let session = state.sessions.get(&session_id).ok_or("session gone")?;
        assert!(session.session.participants.contains_key(&host_id));
        assert!(!session.is_away(&host_id));
        Res::Ok(())
    })
    .await
    .expect("test timed out")
    .unwrap();
}

#[tokio::test]
async fn a_session_whose_drivers_are_all_gone_ends_after_the_grace() {
    timeout(TEST_TIMEOUT, async {
        let server = start_server(1).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr, None).await?;
        let session_id = seat(&mut host, &lobby, None).await?;
        let host_id = host.player_id;
        drop(host);

        eventually(&server, Duration::from_secs(5), "the host is away", |s| {
            is_away(s, session_id, host_id)
        })
        .await?;
        eventually(&server, Duration::from_secs(5), "the session ends", |s| {
            !s.sessions.contains_key(&session_id)
        })
        .await?;
        Res::Ok(())
    })
    .await
    .expect("test timed out")
    .unwrap();
}

#[tokio::test]
async fn a_driver_back_within_the_grace_keeps_the_session() {
    timeout(TEST_TIMEOUT, async {
        let server = start_server(2).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr, None).await?;
        let session_id = seat(&mut host, &lobby, None).await?;
        let (host_id, token) = (host.player_id, host.resume_token.clone());
        drop(host);
        eventually(&server, Duration::from_secs(5), "the host is away", |s| {
            is_away(s, session_id, host_id)
        })
        .await?;

        let (mut back, _) = Client::connect("Host", server.tcp_addr, Some(&token)).await?;
        back.wait_rejoin().await?;
        back.send(&ClientMessage::JoinSession { session_id })
            .await?;
        back.wait_joined().await?;

        // Well past the grace: still there.
        sleep(Duration::from_secs(4)).await;
        assert!(server.state.read().await.sessions.contains_key(&session_id));
        Res::Ok(())
    })
    .await
    .expect("test timed out")
    .unwrap();
}

#[tokio::test]
async fn leaving_the_server_on_purpose_gives_the_seat_up() {
    timeout(TEST_TIMEOUT, async {
        let server = start_server(60).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr, None).await?;
        let session_id = seat(&mut host, &lobby, None).await?;
        let (mut guest, lobby) = Client::connect("Guest", server.tcp_addr, None).await?;
        seat(&mut guest, &lobby, Some(session_id)).await?;
        let guest_id = guest.player_id;

        guest.send(&ClientMessage::Disconnect).await?;
        eventually(
            &server,
            Duration::from_secs(5),
            "the guest's car is gone",
            |s| {
                s.sessions
                    .get(&session_id)
                    .is_some_and(|gs| !gs.session.participants.contains_key(&guest_id))
            },
        )
        .await?;
        Res::Ok(())
    })
    .await
    .expect("test timed out")
    .unwrap();
}

#[tokio::test]
async fn an_unknown_resume_token_is_a_new_player() {
    timeout(TEST_TIMEOUT, async {
        let server = start_server(60).await;
        let (client, _) = Client::connect("Someone", server.tcp_addr, Some("not-a-token")).await?;
        assert_ne!(client.resume_token, "not-a-token");
        assert!(!client.resume_token.is_empty());
        Res::Ok(())
    })
    .await
    .expect("test timed out")
    .unwrap();
}

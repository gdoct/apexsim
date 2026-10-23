//! Rewind in offline practice end to end (`crate::rewind`): a practice
//! session its one human drives keeps a ring of snapshots; `Rewind` puts
//! every car back to the one nearest the time asked for, strikes the laps
//! in progress, holds the cars still, tells the session (`Rewound`) and cuts
//! the recording there. A session with more than one human, or not of the
//! practice kind, refuses it.

use crate::common;

use std::net::SocketAddr;
use std::time::Duration;

use apexsim_server::config::ServerConfig;
use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout, Instant};

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

    /// The next message `pick` accepts, skipping whatever else arrives.
    async fn wait_for<T>(
        &mut self,
        pick: impl Fn(ServerMessage) -> Option<T>,
    ) -> Result<T, Box<dyn std::error::Error>> {
        for _ in 0..2000 {
            if let Some(found) = pick(timeout(Duration::from_secs(20), self.recv()).await??) {
                return Ok(found);
            }
        }
        Err("message never arrived".into())
    }

    /// Create a session of `kind` on the lobby's first track and join it.
    async fn create(
        &mut self,
        lobby: &LobbyStateData,
        kind: SessionKind,
        ai_count: u8,
    ) -> Result<SessionId, Box<dyn std::error::Error>> {
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
        self.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;
        self.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 4,
            ai_count,
            lap_limit: 0,
            session_kind: kind,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::default(),
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
            classes: Vec::new(),
        })
        .await?;
        let joined = self
            .wait_for(|m| match m {
                ServerMessage::SessionJoined(d) => Some(d),
                _ => None,
            })
            .await?;
        // Counted in, so the session is recorded from its first tick racing.
        self.send(&ClientMessage::StartCountdown {
            countdown_seconds: 1,
            next_mode: GameMode::FreePractice,
        })
        .await?;
        Ok(joined.session_id)
    }

    /// A heartbeat: a driver in a session who says nothing for the server's
    /// heartbeat timeout (5 s) is dropped, and the countdown alone can run
    /// that long on a loaded machine.
    async fn beat(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        self.send(&ClientMessage::Heartbeat {
            client_tick: 0,
            ping_ms: None,
        })
        .await
    }

    async fn drive(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        self.beat().await?;
        self.send(&ClientMessage::PlayerInput {
            server_tick_ack: 0,
            throttle: 0.7,
            brake: 0.0,
            steering: 0.0,
            gear: Some(1),
            clutch: None,
            drs: None,
            headlights: None,
            flash: None,
            ers_mode: None,
            ers_boost: None,
            handbrake: None,
            seq: None,
        })
        .await
    }
}

/// The session's tick and the human's car: where it is and whether its
/// lap in progress is struck.
async fn car_now(
    server: &apexsim_server::server::ServerHandle,
    session_id: SessionId,
) -> (u32, [f32; 2], bool) {
    let state = server.state.read().await;
    let gs = &state.sessions[&session_id];
    let car = gs
        .session
        .participants
        .iter()
        .find(|(id, _)| !gs.session.ai_player_ids.contains(id))
        .map(|(_, car)| car)
        .expect("the human's car");
    (
        gs.session.current_tick,
        [car.pos_x, car.pos_y],
        car.laps.invalid,
    )
}

/// The session is under way in free practice.
async fn racing(server: &apexsim_server::server::ServerHandle, session_id: SessionId) -> bool {
    let state = server.state.read().await;
    let gs = &state.sessions[&session_id];
    gs.session.game_mode == GameMode::FreePractice && gs.session.state == SessionState::Racing
}

#[tokio::test]
async fn a_practice_driven_alone_rewinds_and_cuts_its_recording() {
    let result = timeout(Duration::from_secs(120), async {
        let replays = tempfile::tempdir()?;
        let records = tempfile::tempdir()?;
        let mut config = ServerConfig::default();
        config.records.replay_dir = replays.path().to_string_lossy().into_owned();
        config.records.dir = records.path().to_string_lossy().into_owned();
        let server = common::start_test_server_with_config(config).await;
        let rate = server.state.read().await.config.server.tick_rate_hz as u32;

        let (mut host, lobby) = Client::connect("Rewinder", server.tcp_addr).await?;
        // One AI car beside the driver: still a session driven alone.
        let session_id = host.create(&lobby, SessionKind::Practice, 1).await?;

        // Drive three seconds of session time and more, once it races.
        let deadline = Instant::now() + Duration::from_secs(30);
        while !racing(&server, session_id).await {
            assert!(Instant::now() < deadline, "the countdown never ended");
            host.beat().await?;
            sleep(Duration::from_millis(20)).await;
        }
        let start = car_now(&server, session_id).await.0;
        let deadline = Instant::now() + Duration::from_secs(60);
        loop {
            host.drive().await?;
            sleep(Duration::from_millis(30)).await;
            if car_now(&server, session_id).await.0 >= start + 3 * rate + rate / 2 {
                break;
            }
            assert!(Instant::now() < deadline, "the session never got going");
        }
        let (asked_at, before, _) = car_now(&server, session_id).await;

        host.send(&ClientMessage::Rewind { seconds: 2 }).await?;
        let rewound = host
            .wait_for(|m| match m {
                ServerMessage::Rewound(d) => Some(d),
                _ => None,
            })
            .await?;
        assert_eq!(rewound.hold_ms, apexsim_server::rewind::HOLD_MS);
        assert!(rewound.from_tick >= asked_at);
        assert_eq!(rewound.tick % rate, 0, "rewound onto a snapshot");
        let back = rewound.from_tick - rewound.tick;
        assert!(
            back.abs_diff(2 * rate) <= rate / 2 + 1,
            "two seconds back to the nearest snapshot: {back} ticks at {rate} Hz"
        );

        // Held there, the car where it was then, its lap struck.
        let (tick, there, invalid) = car_now(&server, session_id).await;
        assert_eq!(tick, rewound.tick, "held on the tick rewound to");
        assert!(invalid, "the lap through the rewind cannot be a best");
        let moved = ((before[0] - there[0]).powi(2) + (before[1] - there[1]).powi(2)).sqrt();
        assert!(moved > 0.5, "the car went back {moved:.2} m");
        sleep(Duration::from_millis(500)).await;
        assert_eq!(car_now(&server, session_id).await.0, rewound.tick);

        // The run up to the rewind was written as a replay of its own.
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            let written = std::fs::read_dir(replays.path())?
                .filter_map(|e| e.ok())
                .any(|e| e.path().extension().is_some_and(|x| x == "apxs"));
            if written {
                break;
            }
            assert!(
                Instant::now() < deadline,
                "the cut recording was never written"
            );
            host.beat().await?;
            sleep(Duration::from_millis(100)).await;
        }

        // Then the session runs on, recording afresh.
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            host.drive().await?;
            sleep(Duration::from_millis(30)).await;
            if car_now(&server, session_id).await.0 > rewound.tick {
                break;
            }
            assert!(Instant::now() < deadline, "the hold never ended");
        }
        sleep(Duration::from_millis(100)).await;
        assert!(
            server.state.read().await.replay.is_recording(&session_id),
            "a new recording runs from the rewind"
        );

        // A multiplayer session is not driven alone.
        let (mut other, lobby) = Client::connect("Online", server.tcp_addr).await?;
        let online = other.create(&lobby, SessionKind::Multiplayer, 0).await?;
        let deadline = Instant::now() + Duration::from_secs(30);
        while !racing(&server, online).await {
            assert!(Instant::now() < deadline, "the countdown never ended");
            other.beat().await?;
            sleep(Duration::from_millis(20)).await;
        }
        other.send(&ClientMessage::Rewind { seconds: 5 }).await?;
        let code = other
            .wait_for(|m| match m {
                ServerMessage::Error { code, .. } => Some(code),
                _ => None,
            })
            .await?;
        assert_eq!(code, 403);
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;
    result.expect("timed out").expect("test failed");
}

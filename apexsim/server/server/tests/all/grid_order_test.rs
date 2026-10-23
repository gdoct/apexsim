//! The host's start order: asked for on create, applied when the race
//! counts in, with the drivers it leaves out behind. And the stored
//! qualifying results a client can ask for.

use crate::common;

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
        Self::connect_as(name, None, addr).await
    }

    /// Connect under a driver profile's id.
    async fn connect_as(
        name: &str,
        driver_id: Option<uuid::Uuid>,
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
                driver_id,
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

#[tokio::test]
async fn the_hosts_start_order_grids_the_race() {
    let result = timeout(TEST_TIMEOUT, async {
        let records_dir =
            std::env::temp_dir().join(format!("apexsim-grid-{}", uuid::Uuid::new_v4()));
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
            // The second AI on pole, then the host; the guest and the first
            // AI are left out and start behind in the order they were seated.
            grid_order: vec!["@ai:2".into(), "@host".into()],
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
            ballast: Vec::new(),
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
        // What the lobby's roster announces before anyone is lined up.
        let announced = server
            .state
            .read()
            .await
            .sessions
            .get(&joined.session_id)
            .ok_or("no session")?
            .starting_slots();

        host.send(&ClientMessage::StartCountdown {
            countdown_seconds: 5,
            next_mode: GameMode::Race,
        })
        .await?;
        sleep(Duration::from_millis(300)).await;

        let state = server.state.read().await;
        let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
        let slot = |id| session.session.participants[id].grid_position;
        let ai = &session.session.ai_player_ids;
        assert_eq!(ai.len(), 2);
        assert_eq!(slot(&ai[1]), 1, "the asked-for pole");
        assert_eq!(slot(&session.session.host_player_id), 2);
        let behind = [slot(&ai[0])];
        let guest_id = session
            .session
            .participants
            .keys()
            .find(|id| **id != session.session.host_player_id && !ai.contains(id))
            .ok_or("no guest")?;
        let guest_slot = slot(guest_id);
        assert!(behind[0] >= 3 && guest_slot >= 3 && behind[0] != guest_slot);
        for (id, car) in &session.session.participants {
            assert_eq!(
                announced.get(id).copied(),
                Some(car.grid_position),
                "the lobby announced the grid the race used"
            );
        }
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

/// A driver who joins under a profile id takes the slot a `@driver:`
/// reference holds for that id, whatever name they race under.
#[tokio::test]
async fn a_driver_reference_grids_the_profile() {
    let result = timeout(TEST_TIMEOUT, async {
        let records_dir =
            std::env::temp_dir().join(format!("apexsim-grid-{}", uuid::Uuid::new_v4()));
        let server = common::start_test_server_with_records(&records_dir).await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
        let profile = uuid::Uuid::from_u128(0x5eed);

        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;
        host.send(&ClientMessage::CreateSession {
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
            grid_order: vec![
                apexsim_server::grid_order::driver_ref(profile),
                "@host".into(),
            ],
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
            ballast: Vec::new(),
        })
        .await?;
        let joined = host.wait_joined().await?;

        // Renamed since the order was made: the id still finds them.
        let (mut guest, _) = Client::connect_as("Renamed", Some(profile), server.tcp_addr).await?;
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
        host.send(&ClientMessage::StartCountdown {
            countdown_seconds: 5,
            next_mode: GameMode::Race,
        })
        .await?;
        sleep(Duration::from_millis(300)).await;

        let state = server.state.read().await;
        let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
        let ai = &session.session.ai_player_ids;
        let guest_id = *session
            .session
            .participants
            .keys()
            .find(|id| **id != session.session.host_player_id && !ai.contains(id))
            .ok_or("no guest")?;
        assert_eq!(session.driver_id(&guest_id), Some(profile));
        assert_eq!(session.session.participants[&guest_id].grid_position, 1);
        assert_eq!(
            session.session.participants[&session.session.host_player_id].grid_position,
            2
        );
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

/// A track with no qualifying session behind it answers with an empty list,
/// and the stored results a track has come back newest first.
#[tokio::test]
async fn stored_qualifying_results_are_listed_per_track() {
    let result = timeout(TEST_TIMEOUT, async {
        let records_dir =
            std::env::temp_dir().join(format!("apexsim-quali-{}", uuid::Uuid::new_v4()));
        let server = common::start_test_server_with_records(&records_dir).await;
        let (mut client, lobby) = Client::connect("Host", server.tcp_addr).await?;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;

        client
            .send(&ClientMessage::RequestQualifyingResults {
                track_config_id: track_id,
            })
            .await?;
        let empty = loop {
            if let ServerMessage::QualifyingResults(data) =
                timeout(Duration::from_secs(5), client.recv()).await??
            {
                break data;
            }
        };
        assert_eq!(empty.track_id, track_id);
        assert!(empty.results.is_empty());

        let records = server.state.read().await.records.clone();
        for name in ["Old", "New"] {
            records.submit_qualifying(apexsim_server::records::QualifyingResult {
                id: uuid::Uuid::new_v4(),
                track_id,
                class: "GT3".into(),
                recorded_at: String::new(),
                entries: vec![apexsim_server::records::QualifyingEntry {
                    name: name.into(),
                    driver_id: None,
                    car_config_id: car_id,
                    lap_time_ms: 90_000,
                    is_ai: false,
                }],
            });
        }
        client
            .send(&ClientMessage::RequestQualifyingResults {
                track_config_id: track_id,
            })
            .await?;
        let listed = loop {
            if let ServerMessage::QualifyingResults(data) =
                timeout(Duration::from_secs(5), client.recv()).await??
            {
                break data;
            }
        };
        assert_eq!(listed.results.len(), 2);
        assert_eq!(listed.results[0].entries[0].name, "New");
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

/// Ballast by driver on create: the host's and the first AI's cars carry
/// it, the second AI's does not, and `SessionJoined` echoes the list.
#[tokio::test]
async fn ballast_by_driver_weighs_the_cars_it_names() {
    let result = timeout(TEST_TIMEOUT, async {
        let records_dir =
            std::env::temp_dir().join(format!("apexsim-grid-{}", uuid::Uuid::new_v4()));
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
        let ballast = vec![
            DriverBallast {
                driver: "@host".into(),
                kg: 30,
            },
            DriverBallast {
                driver: "@ai:1".into(),
                kg: 15,
            },
        ];
        host.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 4,
            ai_count: 2,
            lap_limit: 3,
            session_kind: SessionKind::Practice,
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
            classes: Vec::new(),
            ballast: ballast.clone(),
        })
        .await?;
        let joined = host.wait_joined().await?;
        assert_eq!(joined.ballast, ballast, "the list echoed");

        let state = server.state.read().await;
        let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
        let mass = |id| {
            session
                .simulated_config_for(id)
                .map(|c| c.mass_kg)
                .unwrap_or_default()
        };
        let base = state.car_configs.get(&car_id).ok_or("no car")?.mass_kg;
        let ai = &session.session.ai_player_ids;
        assert_eq!(mass(&session.session.host_player_id), base + 30.0);
        assert_eq!(session.ballast_kg(&ai[0]), 15);
        assert_eq!(session.ballast_kg(&ai[1]), 0);
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

//! Weather and time of day are chosen when a session is created, echoed to
//! everyone who joins it, listed in the session browser, and baked into the
//! grip of the session's own copy of the track.

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
        for _ in 0..50 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::SessionJoined(data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no SessionJoined".into())
    }

    async fn lobby_state(&mut self) -> Result<LobbyStateData, Box<dyn std::error::Error>> {
        self.send(&ClientMessage::RequestLobbyState).await?;
        for _ in 0..50 {
            if let ServerMessage::LobbyState(data) =
                timeout(Duration::from_secs(5), self.recv()).await??
            {
                return Ok(data);
            }
        }
        Err("no LobbyState".into())
    }
}

#[tokio::test]
async fn rainy_night_session_is_echoed_listed_and_baked_into_grip() {
    let result = timeout(TEST_TIMEOUT, async {
        let server = common::start_test_server().await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;

        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;

        let asked = SessionConditions {
            weather: Weather::HeavyRain,
            // Past midnight, wrapped by the server onto the day: 25:30 -> 01:30.
            time_of_day_minutes: 25 * 60 + 30,
        };
        host.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 4,
            ai_count: 0,
            lap_limit: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: asked,
        })
        .await?;
        let joined = host.wait_joined().await?;
        let expected = SessionConditions {
            weather: Weather::HeavyRain,
            time_of_day_minutes: 90,
        };
        assert_eq!(
            joined.conditions, expected,
            "SessionJoined echoes the clamped conditions"
        );

        // The browser lists them.
        let lobby = host.lobby_state().await?;
        let summary = lobby
            .available_sessions
            .iter()
            .find(|s| s.id == joined.session_id)
            .ok_or("session not listed")?;
        assert_eq!(summary.conditions, expected);

        // A second driver joining gets the same sky.
        let (mut guest, _) = Client::connect("Guest", server.tcp_addr).await?;
        guest
            .send(&ClientMessage::SelectCar {
                car_config_id: car_id,
            })
            .await?;
        sleep(Duration::from_millis(50)).await;
        guest
            .send(&ClientMessage::JoinSession {
                session_id: joined.session_id,
            })
            .await?;
        assert_eq!(guest.wait_joined().await?.conditions, expected);

        // And the session's own track carries the wet grip while the shared
        // track (what a dry session would clone) is untouched.
        {
            let state = server.state.read().await;
            let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
            let shared = state.track_configs.get(&track_id).ok_or("no track")?;
            let wet = &session.track_config.track_surface;
            let dry = &shared.track_surface;
            let road = Weather::HeavyRain.road_grip_factor();
            assert!((wet.base_grip - dry.base_grip * road).abs() < 1e-6);
            assert!(
                wet.curb_grip < dry.curb_grip * road,
                "curbs lose more than the road"
            );
            assert!(wet.off_track_grip < dry.off_track_grip * road);
            let (wet_pt, dry_pt) = (&session.track_config.centerline[0], &shared.centerline[0]);
            assert!((wet_pt.grip_modifier - dry_pt.grip_modifier * road).abs() < 1e-6);
            assert_eq!(session.session.conditions, expected);
        }

        host.send(&ClientMessage::Disconnect).await?;
        guest.send(&ClientMessage::Disconnect).await?;
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

#[tokio::test]
async fn a_session_created_without_conditions_is_a_sunny_afternoon() {
    let result = timeout(TEST_TIMEOUT, async {
        let server = common::start_test_server().await;
        let (mut host, lobby) = Client::connect("Legacy", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;

        // The message as a client from before conditions sends it.
        #[derive(serde::Serialize)]
        struct Envelope<T: serde::Serialize> {
            r#type: &'static str,
            data: T,
        }
        #[derive(serde::Serialize)]
        struct OldCreate {
            track_config_id: String,
            max_players: u8,
            ai_count: u8,
            lap_limit: u8,
        }
        let bytes = rmp_serde::to_vec_named(&Envelope {
            r#type: "CreateSession",
            data: OldCreate {
                track_config_id: track_id.to_string(),
                max_players: 2,
                ai_count: 0,
                lap_limit: 1,
            },
        })?;
        host.tcp
            .write_all(&(bytes.len() as u32).to_be_bytes())
            .await?;
        host.tcp.write_all(&bytes).await?;
        host.tcp.flush().await?;

        let joined = host.wait_joined().await?;
        assert_eq!(joined.conditions, SessionConditions::DEFAULT);
        let state = server.state.read().await;
        let session = state.sessions.get(&joined.session_id).ok_or("no session")?;
        let shared = state.track_configs.get(&track_id).ok_or("no track")?;
        assert_eq!(
            session.track_config.track_surface.base_grip,
            shared.track_surface.base_grip
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

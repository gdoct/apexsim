//! Tracks start as catalog entries and load their sidecars when the first
//! session on them is created (`track_content`, `game_loop::track_loads`):
//! the session drives on the same track a full load gives, and a client's
//! messages are handled in the order it sent them while the track loads.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::config::RoadContactMode;
use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage};
use apexsim_server::track_loader::TrackLoader;

const TEST_TIMEOUT: Duration = Duration::from_secs(60);

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
}

/// Monza with its sidecars baked, or `None` (they are generated, not
/// checked in) and the test is skipped.
fn baked_monza() -> Option<std::path::PathBuf> {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../content/tracks/default/Monza.yaml");
    apexsim_server::walls::Walls::sidecar_path(&path)
        .exists()
        .then_some(path)
}

#[tokio::test]
async fn the_first_session_on_a_track_loads_its_sidecars_and_keeps_message_order() {
    let Some(monza) = baked_monza() else {
        eprintln!("Monza's sidecars are not baked; skipping");
        return;
    };
    let result = timeout(TEST_TIMEOUT, async {
        let server = common::start_test_server().await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;

        // Startup read the track and none of its sidecars.
        let track_id = {
            let state = server.state.read().await;
            let (id, catalog) = state
                .track_configs
                .iter()
                .find(|(_, t)| t.source_path.as_deref() == Some("tracks/default/Monza.yaml"))
                .ok_or("Monza is not in the catalog")?;
            assert!(catalog.walls.is_none() && catalog.road_mesh.is_none());
            assert!(state.track_content.needs_loading(*id));
            *id
        };

        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;

        // The create waits for the track to load; the leave sent straight
        // after it must wait behind it rather than overtake it.
        host.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 2,
            ai_count: 0,
            lap_limit: 1,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: Default::default(),
        })
        .await?;
        host.send(&ClientMessage::LeaveSession).await?;

        let mut seen = Vec::new();
        while !seen.contains(&"left") {
            match timeout(Duration::from_secs(20), host.recv()).await?? {
                ServerMessage::SessionJoined(_) => seen.push("joined"),
                ServerMessage::SessionLeft => seen.push("left"),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => {}
            }
        }
        assert_eq!(seen, vec!["joined", "left"], "handled in the order sent");

        let state = server.state.read().await;
        assert!(
            state.sessions.is_empty(),
            "the leave emptied the session it followed"
        );
        assert!(!state.track_content.needs_loading(track_id));

        // What the session drove on is what a full load of the file gives.
        let full = TrackLoader::load_from_file_with(&monza, RoadContactMode::Mesh)?;
        let catalog = &state.track_configs[&track_id];
        let complete = state.track_content.complete(catalog);
        assert_eq!(
            complete.walls.as_ref().map(|w| w.len()),
            full.walls.as_ref().map(|w| w.len())
        );
        assert_eq!(
            complete.road_mesh.as_ref().map(|m| m.triangle_count()),
            full.road_mesh.as_ref().map(|m| m.triangle_count())
        );
        assert_eq!(complete.pit_lane.is_some(), full.pit_lane.is_some());
        assert_eq!(complete.ground.is_some(), full.ground.is_some());
        assert_eq!(complete.curbs.is_some(), full.curbs.is_some());
        let seats = |t: &TrackConfig| t.start_positions.iter().map(|s| s.z).collect::<Vec<_>>();
        assert_eq!(seats(&complete), seats(&full));
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;

    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

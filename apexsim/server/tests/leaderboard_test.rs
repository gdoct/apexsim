//! A track's leaderboard on request: the stored laps, best first, all cars or
//! one car.

mod common;

use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::timeout;

use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage};

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
}

#[tokio::test]
async fn a_leaderboard_lists_the_stored_laps_best_first() {
    let records = tempfile::TempDir::new().unwrap();
    let result = timeout(TEST_TIMEOUT, async {
        let server = common::start_test_server_with_records(records.path()).await;
        let (mut client, lobby) = Client::connect("Fan", server.tcp_addr).await?;
        let track = lobby.track_configs.first().ok_or("no tracks")?.id;
        let car_a = lobby.car_configs[0].id;
        let car_b = lobby.car_configs[1].id;
        {
            let state = server.state.read().await;
            state.records.submit(
                "Ayrton",
                track,
                car_a,
                82_000,
                [27_000, 28_000, 27_000],
                None,
            );
            state.records.submit(
                "Alain",
                track,
                car_a,
                81_000,
                [26_000, 28_000, 27_000],
                None,
            );
            state.records.submit(
                "Ayrton",
                track,
                car_b,
                80_000,
                [26_000, 27_000, 27_000],
                None,
            );
        }
        client
            .send(&ClientMessage::RequestLeaderboard {
                track_config_id: track,
                car_config_id: None,
            })
            .await?;
        let board = loop {
            if let ServerMessage::Leaderboard(data) =
                timeout(Duration::from_secs(5), client.recv()).await??
            {
                break data;
            }
        };
        assert_eq!(board.track_id, track);
        assert_eq!(board.car_config_id, "");
        let times: Vec<u32> = board.entries.iter().map(|e| e.lap_time_ms).collect();
        assert_eq!(times, [80_000, 81_000, 82_000]);
        assert_eq!(board.entries[1].player_name, "Alain");
        assert_eq!(board.entries[1].splits_ms, vec![26_000, 28_000, 27_000]);

        client
            .send(&ClientMessage::RequestLeaderboard {
                track_config_id: track,
                car_config_id: Some(car_a.to_string()),
            })
            .await?;
        let board = loop {
            if let ServerMessage::Leaderboard(data) =
                timeout(Duration::from_secs(5), client.recv()).await??
            {
                break data;
            }
        };
        assert_eq!(board.car_config_id, car_a.to_string());
        assert_eq!(board.entries.len(), 2);
        assert!(board.entries.iter().all(|e| e.car_config_id == car_a));
        client.send(&ClientMessage::Disconnect).await?;
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;
    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

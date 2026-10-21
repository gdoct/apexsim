//! The health port hands out the server's recordings of finished sessions:
//! `/recordings` lists them, `/recordings/<file>` is one's stream, and the
//! lobby names the port (`LobbyState.recordings_port`).

use crate::common;

use apexsim_server::config::ServerConfig;
use apexsim_server::replay::ReplayMetadata;
use std::net::SocketAddr;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use uuid::Uuid;

/// A plain HTTP/1.1 GET: the status code and the body.
async fn get(addr: SocketAddr, path: &str) -> (u16, Vec<u8>) {
    let mut stream = TcpStream::connect(addr).await.expect("connect");
    let request = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    stream.write_all(request.as_bytes()).await.unwrap();
    let mut raw = Vec::new();
    stream.read_to_end(&mut raw).await.unwrap();
    let split = raw
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .expect("headers");
    let head = String::from_utf8_lossy(&raw[..split]).into_owned();
    let status = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    (status, raw[split + 4..].to_vec())
}

#[tokio::test]
async fn test_the_recordings_are_listed_and_handed_out() {
    let dir = tempfile::tempdir().unwrap();
    let mut config = ServerConfig::default();
    config.records.replay_dir = dir.path().to_string_lossy().into_owned();
    let server = common::start_test_server_with_config(config).await;
    assert_eq!(
        server.state.read().await.recordings_port,
        Some(server.health_addr.port()),
        "the lobby says where"
    );

    let session = Uuid::from_u128(77);
    let stem = format!("replay_{session}_1760000000");
    let meta = ReplayMetadata {
        session_id: session,
        track_config_id: Uuid::nil(),
        track_name: "Monzarella".into(),
        recorded_at: 1_760_000_000,
        duration_ticks: 4200,
        tick_rate: 420,
        participants: vec![],
        conditions: Default::default(),
        race_start_tick: Some(10),
        track_stem: Some("Monzarella".into()),
        track_length_m: 5793.0,
    };
    std::fs::write(
        dir.path().join(format!("{stem}.meta.json")),
        serde_json::to_vec(&meta).unwrap(),
    )
    .unwrap();
    std::fs::write(dir.path().join(format!("{stem}.apxs")), b"APXS-test-bytes").unwrap();
    std::fs::write(dir.path().join("lap_records.json"), b"{}").unwrap();

    let (status, body) = get(server.health_addr, "/recordings").await;
    assert_eq!(status, 200);
    let list: serde_json::Value = serde_json::from_slice(&body).unwrap();
    let rows = list["sessions"].as_array().unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0]["file"], format!("{stem}.apxs"));
    assert_eq!(rows[0]["track"], "Monzarella");
    assert_eq!(rows[0]["kind"], "Race");

    let (status, body) = get(server.health_addr, &format!("/recordings/{stem}.apxs")).await;
    assert_eq!(status, 200);
    assert_eq!(body, b"APXS-test-bytes");

    // Nothing but the recorder's own streams leaves the folder.
    for path in [
        "/recordings/lap_records.json",
        "/recordings/..%2Fserver.toml",
        "/recordings/../server.toml",
        "/recordings/replay_missing.apxs",
    ] {
        assert_eq!(get(server.health_addr, path).await.0, 404, "{path}");
    }
}

#[tokio::test]
async fn test_a_server_that_keeps_its_recordings_says_nothing() {
    let mut config = ServerConfig::default();
    config.records.serve_replays = false;
    let server = common::start_test_server_with_config(config).await;
    assert_eq!(server.state.read().await.recordings_port, None);
    assert_eq!(get(server.health_addr, "/recordings").await.0, 404);
}

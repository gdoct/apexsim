//! Hosts a race on a running server and keeps it going, so something can be
//! watched: the game's `-ApexWatchSession`, a spectator-stream check, a
//! screenshot run that needs a field on the track.
//!
//! ```text
//! cargo run --example host_race -- [host:port] [ai_count] [laps] [seconds_to_stay]
//! ```
//!
//! Defaults: 127.0.0.1:9000, 5 AI, 5 laps, 600 s. The host car is the lobby's
//! first, the track the lobby's first (set `APEXSIM_TRACK` to a name to pick
//! one by substring). The session is the host's, so it ends when this does.

use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage};

async fn send(
    tcp: &mut (impl AsyncWriteExt + Unpin),
    msg: &ClientMessage,
) -> Result<(), Box<dyn std::error::Error>> {
    let data = rmp_serde::to_vec_named(msg)?;
    tcp.write_all(&(data.len() as u32).to_be_bytes()).await?;
    tcp.write_all(&data).await?;
    tcp.flush().await?;
    Ok(())
}

async fn recv(tcp: &mut TcpStream) -> Result<ServerMessage, Box<dyn std::error::Error>> {
    let mut len = [0u8; 4];
    tcp.read_exact(&mut len).await?;
    let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
    tcp.read_exact(&mut buf).await?;
    Ok(rmp_serde::from_slice(&buf)?)
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let addr = args.get(1).map_or("127.0.0.1:9000", |s| s.as_str());
    let ai_count: u8 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(5);
    let laps: u8 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(5);
    let stay: u64 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(600);
    let wanted_track = std::env::var("APEXSIM_TRACK").ok();

    let mut tcp = TcpStream::connect(addr).await?;
    send(
        &mut tcp,
        &ClientMessage::Authenticate {
            token: "host_race".into(),
            player_name: "Host".into(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
            resume_token: None,
            driver_id: None,
            driver_key: None,
        },
    )
    .await?;
    let lobby = loop {
        if let ServerMessage::LobbyState(lobby) =
            timeout(Duration::from_secs(10), recv(&mut tcp)).await??
        {
            break lobby;
        }
    };
    let car = lobby.car_configs.first().ok_or("the server has no cars")?;
    let track = match &wanted_track {
        Some(name) => lobby
            .track_configs
            .iter()
            .find(|t| t.name.to_lowercase().contains(&name.to_lowercase()))
            .ok_or("no such track")?,
        None => lobby
            .track_configs
            .first()
            .ok_or("the server has no tracks")?,
    };
    println!(
        "hosting {} laps of {} in {} with {ai_count} AI",
        laps, track.name, car.name
    );
    send(
        &mut tcp,
        &ClientMessage::SelectCar {
            car_config_id: car.id,
            livery: 0,
        },
    )
    .await?;
    sleep(Duration::from_millis(100)).await;
    send(
        &mut tcp,
        &ClientMessage::CreateSession {
            track_config_id: track.id,
            max_players: 8,
            ai_count,
            lap_limit: laps,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
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
            ballast: Vec::new(),
        },
    )
    .await?;
    loop {
        if let ServerMessage::SessionJoined(joined) =
            timeout(Duration::from_secs(20), recv(&mut tcp)).await??
        {
            println!("session {}", joined.session_id);
            break;
        }
    }
    send(
        &mut tcp,
        &ClientMessage::StartCountdown {
            countdown_seconds: 3,
            next_mode: GameMode::Race,
        },
    )
    .await?;
    // The host sends no input, so its car sits on the grid; the AI race.
    // Keep reading so the server's queue to this connection drains.
    let (mut reader, mut writer) = tcp.into_split();
    tokio::spawn(async move {
        let _ = tokio::io::copy(&mut reader, &mut tokio::io::sink()).await;
    });
    sleep(Duration::from_secs(stay)).await;
    send(&mut writer, &ClientMessage::Disconnect).await?;
    Ok(())
}

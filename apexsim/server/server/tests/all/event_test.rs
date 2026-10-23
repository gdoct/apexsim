//! A race event across a server restart, run from the dashboard: created
//! and opened, drivers entered by their proved driver ids (a driver
//! without one refused), practice and qualifying run and recorded, the
//! server stopped and started again on the same records, the event's
//! session opened again at its race with the same entries, gridded from
//! the stored qualifying, and the race recorded.

use std::net::SocketAddr;
use std::time::Duration;

use apexsim_server::config::ServerConfig;
use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage};
use apexsim_server::server::{run_server_from, ServerHandle};
use ring::signature::KeyPair;
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

const TOKEN: &str = "event-token-0123456789";

async fn start(dir: &std::path::Path) -> (ServerHandle, SocketAddr) {
    let mut config = ServerConfig::default();
    config.network.tcp_bind = "127.0.0.1:0".into();
    config.network.udp_bind = "127.0.0.1:0".into();
    config.network.health_bind = "127.0.0.1:0".into();
    config.network.require_tls = false;
    config.content.skip_imported_tracks = cfg!(debug_assertions);
    config.records.dir = dir.join("records").to_string_lossy().into_owned();
    config.records.replay_dir = dir.join("replays").to_string_lossy().into_owned();
    config.admin.http_bind = "127.0.0.1:0".into();
    config.admin.https_bind = String::new();
    config.admin.token = TOKEN.into();
    let file = |name: &str| dir.join(name).to_string_lossy().into_owned();
    config.admin.bans_file = file("bans.json");
    config.admin.content_file = file("content.json");
    config.admin.schedule_file = file("schedule.json");
    let server = run_server_from(config, None).await.unwrap();
    let http = server.admin.clone().unwrap().http_addr.unwrap();
    (server, http)
}

async fn api(addr: SocketAddr, method: &str, path: &str, body: Option<&Value>) -> (u16, Value) {
    let mut stream = TcpStream::connect(addr).await.unwrap();
    let body = body.map(|b| b.to_string()).unwrap_or_default();
    let req = format!(
        "{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nAuthorization: Bearer {TOKEN}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
        body.len()
    );
    stream.write_all(req.as_bytes()).await.unwrap();
    let mut raw = Vec::new();
    timeout(Duration::from_secs(10), stream.read_to_end(&mut raw))
        .await
        .expect("an answer in time")
        .ok();
    let text = String::from_utf8_lossy(&raw).into_owned();
    let (head, body) = text.split_once("\r\n\r\n").expect("a full response");
    let status = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap();
    (status, serde_json::from_str(body).unwrap_or(Value::Null))
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

struct Driver {
    tcp: TcpStream,
    player_id: PlayerId,
}

impl Driver {
    /// Connect as `name`, proving `key` for `driver_id` when one is given.
    async fn connect(
        addr: SocketAddr,
        name: &str,
        driver: Option<(uuid::Uuid, &ring::signature::Ed25519KeyPair)>,
    ) -> Self {
        let tcp = TcpStream::connect(addr).await.unwrap();
        let mut d = Driver {
            tcp,
            player_id: PlayerId::nil(),
        };
        d.send(&ClientMessage::Authenticate {
            token: "dev".into(),
            player_name: name.into(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
            resume_token: None,
            driver_id: driver.map(|(id, _)| id),
            driver_key: driver.map(|(_, k)| hex(k.public_key().as_ref())),
        })
        .await;
        let auth = d
            .until(|m| match m {
                ServerMessage::AuthSuccess(a) => Some(a.clone()),
                _ => None,
            })
            .await
            .expect("AuthSuccess");
        d.player_id = auth.player_id;
        if let Some((id, key)) = driver {
            let message = apexsim_server::driver_keys::proof_message(&auth.key_challenge, id);
            d.send(&ClientMessage::ProveDriverKey {
                signature: hex(key.sign(&message).as_ref()),
            })
            .await;
        }
        d
    }

    async fn send(&mut self, msg: &ClientMessage) {
        let data = rmp_serde::to_vec_named(msg).unwrap();
        self.tcp
            .write_all(&(data.len() as u32).to_be_bytes())
            .await
            .unwrap();
        self.tcp.write_all(&data).await.unwrap();
    }

    async fn recv(&mut self) -> Option<ServerMessage> {
        let mut len = [0u8; 4];
        timeout(Duration::from_secs(5), self.tcp.read_exact(&mut len))
            .await
            .ok()?
            .ok()?;
        let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
        self.tcp.read_exact(&mut buf).await.ok()?;
        rmp_serde::from_slice(&buf).ok()
    }

    async fn until<T>(&mut self, pick: impl Fn(&ServerMessage) -> Option<T>) -> Option<T> {
        for _ in 0..500 {
            let msg = self.recv().await?;
            if let Some(found) = pick(&msg) {
                return Some(found);
            }
        }
        None
    }

    /// Pick a car and join; `Ok` on `SessionJoined`, the error code else.
    async fn join(&mut self, session: SessionId, car: CarConfigId, livery: u8) -> Result<(), u16> {
        self.send(&ClientMessage::SelectCar {
            car_config_id: car,
            livery,
        })
        .await;
        sleep(Duration::from_millis(50)).await;
        self.send(&ClientMessage::JoinSession {
            session_id: session,
        })
        .await;
        self.until(|m| match m {
            ServerMessage::SessionJoined(_) => Some(Ok(())),
            ServerMessage::Error { code, .. } => Some(Err(*code)),
            _ => None,
        })
        .await
        .expect("an answer to the join")
    }
}

async fn wait_event(addr: SocketAddr, what: &str, pick: impl Fn(&Value) -> bool) -> Value {
    for _ in 0..150 {
        let (status, event) = api(addr, "GET", "/api/events/autumn-cup", None).await;
        if status == 200 && pick(&event) {
            return event;
        }
        sleep(Duration::from_millis(100)).await;
    }
    panic!("timed out waiting for {what}");
}

/// Run a timed phase's clock out: with every car in its garage the phase
/// is over on the loop's next tick.
async fn run_out_the_clock(server: &ServerHandle, session: SessionId) {
    let mut st = server.state.write().await;
    let gs = st.sessions.get_mut(&session).unwrap();
    let length = gs.timed_length_ticks().expect("a timed phase") as u32;
    gs.session.current_tick = gs.timed_start_tick.unwrap() + length - 1;
}

fn session_of(event: &Value) -> SessionId {
    event["session"].as_str().unwrap().parse().unwrap()
}

#[tokio::test]
async fn an_event_resumes_at_its_race_after_a_restart() {
    timeout(Duration::from_secs(120), async {
        let dir = tempfile::tempdir().unwrap();
        let (server, http) = start(dir.path()).await;
        let (track, car, other_car) = {
            let st = server.state.read().await;
            let mut tracks: Vec<_> = st
                .track_configs
                .values()
                .map(|t| (t.name.clone(), t.id))
                .collect();
            tracks.sort();
            let mut cars: Vec<_> = st
                .car_configs
                .values()
                .map(|c| (c.name.clone(), c.id))
                .collect();
            cars.sort();
            (tracks[0].1, cars[0].1, cars[cars.len() - 1].1)
        };

        let (status, created) = api(
            http,
            "POST",
            "/api/events",
            Some(&serde_json::json!({
                "name": "Autumn Cup",
                "spec": {
                    "track_id": track, "car_id": car, "player_slots": 3,
                    "ai_count": 0, "lap_limit": 2,
                    "weekend": { "practice_seconds": 60, "qualifying_seconds": [60] },
                },
            })),
        )
        .await;
        assert_eq!(status, 200, "{created}");
        assert_eq!(created["event"]["id"], "autumn-cup");
        assert_eq!(created["event"]["phase"], "practice");
        let (status, opened) = api(http, "POST", "/api/events/autumn-cup/open", None).await;
        assert_eq!(status, 200, "{opened}");
        let session: SessionId = opened["session"].as_str().unwrap().parse().unwrap();

        // Two drivers by their keys; one without a key is refused.
        let (id_a, id_b) = (uuid::Uuid::from_u128(0xA), uuid::Uuid::from_u128(0xB));
        let rng = ring::rand::SystemRandom::new();
        let key = || {
            ring::signature::Ed25519KeyPair::from_pkcs8(
                ring::signature::Ed25519KeyPair::generate_pkcs8(&rng)
                    .unwrap()
                    .as_ref(),
            )
            .unwrap()
        };
        let (key_a, key_b) = (key(), key());
        let mut a = Driver::connect(server.tcp_addr, "Ada", Some((id_a, &key_a))).await;
        let mut b = Driver::connect(server.tcp_addr, "Bea", Some((id_b, &key_b))).await;
        let mut c = Driver::connect(server.tcp_addr, "Cy", None).await;
        sleep(Duration::from_millis(100)).await;
        assert_eq!(a.join(session, car, 1).await, Ok(()));
        assert_eq!(b.join(session, car, 0).await, Ok(()));
        assert_eq!(c.join(session, car, 0).await, Err(403), "no proved id");
        // The livery is the entry's for the event.
        a.send(&ClientMessage::SetLivery { livery: 2 }).await;
        let code = a
            .until(|m| match m {
                ServerMessage::Error { code, .. } => Some(*code),
                _ => None,
            })
            .await;
        assert_eq!(code, Some(403));

        // Practice, run and recorded.
        let (status, started) = api(http, "POST", "/api/events/autumn-cup/start", None).await;
        assert_eq!(status, 200, "{started}");
        assert_eq!(started["phase"], "practice");
        run_out_the_clock(&server, session).await;
        let event = wait_event(http, "practice recorded", |e| e["phase"] == "qualifying").await;
        assert_eq!(event["results"][0]["phase"], "practice");
        assert_eq!(event["results"][0]["lines"].as_array().unwrap().len(), 2);

        // Qualifying: Bea quicker than Ada.
        let (status, q) = api(http, "POST", "/api/events/autumn-cup/advance", None).await;
        assert_eq!(
            (status, q["phase"].as_str()),
            (200, Some("qualifying")),
            "{q}"
        );
        {
            let mut st = server.state.write().await;
            let gs = st.sessions.get_mut(&session).unwrap();
            gs.note_qualifying_lap(b.player_id, 80_000);
            gs.note_qualifying_lap(a.player_id, 81_500);
        }
        run_out_the_clock(&server, session).await;
        let event = wait_event(http, "qualifying recorded", |e| e["phase"] == "race").await;
        let lines = &event["results"][1]["lines"];
        assert_eq!(lines[0]["driver_id"], id_b.to_string());
        assert_eq!(lines[0]["best_lap_ms"], 80_000);
        assert_eq!(lines[1]["driver_id"], id_a.to_string());
        let entries = event["entries"].as_array().unwrap();
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0]["livery"], 1);

        // The server goes down and comes back on the same records.
        drop((a, b, c));
        server.shutdown().await;
        sleep(Duration::from_millis(1500)).await;
        let (server, http) = start(dir.path()).await;
        let event = wait_event(http, "the event's session again", |e| {
            e["session"].is_string()
        })
        .await;
        assert_eq!(event["phase"], "race");
        let session = session_of(&event);
        assert_eq!(
            server
                .state
                .read()
                .await
                .lobby
                .get_available_sessions()
                .await
                .into_iter()
                .find(|s| s.id == session)
                .map(|s| s.host_name),
            Some("Autumn Cup".to_string())
        );

        // The same drivers, in their entries' cars whatever they pick now.
        let mut a = Driver::connect(server.tcp_addr, "Ada", Some((id_a, &key_a))).await;
        let mut b = Driver::connect(server.tcp_addr, "Bea", Some((id_b, &key_b))).await;
        let mut d = Driver::connect(
            server.tcp_addr,
            "Dee",
            Some((uuid::Uuid::from_u128(0xD), &key())),
        )
        .await;
        sleep(Duration::from_millis(100)).await;
        assert_eq!(a.join(session, other_car, 0).await, Ok(()));
        assert_eq!(b.join(session, car, 0).await, Ok(()));
        // Entries close: a new driver is refused.
        let (status, _) = api(
            http,
            "POST",
            "/api/events/autumn-cup/entries",
            Some(&serde_json::json!({ "open": false })),
        )
        .await;
        assert_eq!(status, 200);
        assert_eq!(d.join(session, car, 0).await, Err(403), "entries closed");
        {
            let st = server.state.read().await;
            let gs = st.sessions.get(&session).unwrap();
            assert_eq!(gs.session.participants[&a.player_id].car_config_id, car);
            // Gridded from the stored qualifying: Bea on pole.
            let slots = gs.starting_slots();
            assert_eq!((slots[&b.player_id], slots[&a.player_id]), (1, 2));
        }

        // Straight to the race's grid: practice and qualifying are done.
        let (status, raced) = api(http, "POST", "/api/events/autumn-cup/start", None).await;
        assert_eq!(
            (status, raced["phase"].as_str()),
            (200, Some("race")),
            "{raced}"
        );
        {
            let mut st = server.state.write().await;
            let gs = st.sessions.get_mut(&session).unwrap();
            assert_eq!(gs.session.game_mode, GameMode::Grid);
            assert_eq!(gs.session.participants[&b.player_id].grid_position, 1);
            // The race run and over.
            gs.session.game_mode = GameMode::Race;
            gs.session.state = SessionState::Finished;
        }
        let event = wait_event(http, "the race recorded", |e| e["phase"] == "done").await;
        assert_eq!(event["opened"], false);
        let phases: Vec<&str> = event["results"]
            .as_array()
            .unwrap()
            .iter()
            .map(|r| r["phase"].as_str().unwrap())
            .collect();
        assert_eq!(phases, ["practice", "qualifying", "race"]);
        assert_eq!(event["results"][2]["lines"].as_array().unwrap().len(), 2);
        // The file is what the dashboard showed.
        let stored: Value = serde_json::from_slice(
            &std::fs::read(dir.path().join("records/events/autumn-cup.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(stored["phase"], "done");
        drop((a, b, d));
        server.shutdown().await;
    })
    .await
    .expect("in time");
}

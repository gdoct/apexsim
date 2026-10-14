//! The web dashboard end to end: both listeners, the login, the API over a
//! live session, kick and ban of a connected client, and the config editor.
//! Plain sockets throughout, so the tests see exactly what a browser would.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use apexsim_server::config::ServerConfig;
use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage};
use apexsim_server::server::{run_server_from, ServerHandle};
use serde_json::Value;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

const TOKEN: &str = "test-token-0123456789";

struct Dashboard {
    server: ServerHandle,
    dir: tempfile::TempDir,
    http: SocketAddr,
    https: SocketAddr,
}

async fn start_dashboard() -> Dashboard {
    start_dashboard_with(|_| {}, None).await
}

async fn start_dashboard_with(
    tweak: impl FnOnce(&mut ServerConfig),
    config_path: Option<std::path::PathBuf>,
) -> Dashboard {
    let dir = tempfile::tempdir().unwrap();
    let mut config = ServerConfig::default();
    config.network.tcp_bind = "127.0.0.1:0".into();
    config.network.udp_bind = "127.0.0.1:0".into();
    config.network.health_bind = "127.0.0.1:0".into();
    config.network.require_tls = false;
    config.content.skip_imported_tracks = cfg!(debug_assertions);
    config.records.dir = dir.path().join("records").to_string_lossy().into_owned();
    config.admin.http_bind = "127.0.0.1:0".into();
    config.admin.https_bind = "127.0.0.1:0".into();
    config.admin.token = TOKEN.into();
    // The ban list and the generated certificate live beside it.
    config.admin.bans_file = dir.path().join("bans.json").to_string_lossy().into_owned();
    config.admin.content_file = dir
        .path()
        .join("content.json")
        .to_string_lossy()
        .into_owned();
    config.admin.schedule_file = dir
        .path()
        .join("schedule.json")
        .to_string_lossy()
        .into_owned();
    tweak(&mut config);
    let server = run_server_from(config, config_path).await.unwrap();
    let admin = server.admin.clone().expect("dashboard enabled");
    Dashboard {
        http: admin.http_addr.unwrap(),
        https: admin.https_addr.unwrap(),
        server,
        dir,
    }
}

struct Reply {
    status: u16,
    headers: HashMap<String, String>,
    body: String,
}

impl Reply {
    fn json(&self) -> Value {
        serde_json::from_str(&self.body).unwrap_or_else(|e| panic!("not JSON ({e}): {}", self.body))
    }
}

async fn exchange<S: AsyncRead + AsyncWrite + Unpin>(
    mut stream: S,
    method: &str,
    path: &str,
    headers: &[(&str, &str)],
    body: Option<&str>,
) -> Reply {
    let mut req = format!("{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
    for (k, v) in headers {
        req.push_str(&format!("{k}: {v}\r\n"));
    }
    if let Some(b) = body {
        req.push_str(&format!(
            "Content-Type: application/json\r\nContent-Length: {}\r\n",
            b.len()
        ));
    }
    req.push_str("\r\n");
    if let Some(b) = body {
        req.push_str(b);
    }
    stream.write_all(req.as_bytes()).await.unwrap();
    let mut raw = Vec::new();
    timeout(Duration::from_secs(10), stream.read_to_end(&mut raw))
        .await
        .expect("response in time")
        .ok();
    let text = String::from_utf8_lossy(&raw).into_owned();
    let (head, body) = text.split_once("\r\n\r\n").expect("a full response");
    let mut lines = head.lines();
    let status = lines
        .next()
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|s| s.parse().ok())
        .expect("status line");
    let headers = lines
        .filter_map(|l| l.split_once(": "))
        .map(|(k, v)| (k.to_ascii_lowercase(), v.to_string()))
        .collect();
    Reply {
        status,
        headers,
        body: body.to_string(),
    }
}

async fn http(
    addr: SocketAddr,
    method: &str,
    path: &str,
    headers: &[(&str, &str)],
    body: Option<&str>,
) -> Reply {
    exchange(
        TcpStream::connect(addr).await.unwrap(),
        method,
        path,
        headers,
        body,
    )
    .await
}

fn bearer() -> [(&'static str, &'static str); 1] {
    [("Authorization", "Bearer test-token-0123456789")]
}

async fn get(d: &Dashboard, path: &str) -> Reply {
    http(d.http, "GET", path, &bearer(), None).await
}

// ---------------------------------------------------------------- game client

struct Racer {
    tcp: TcpStream,
}

impl Racer {
    async fn connect(addr: SocketAddr, name: &str) -> Result<(Self, ServerMessage), String> {
        let tcp = TcpStream::connect(addr).await.map_err(|e| e.to_string())?;
        let mut racer = Racer { tcp };
        racer
            .send(&ClientMessage::Authenticate {
                token: "x".into(),
                player_name: name.into(),
                protocol_version: apexsim_server::network::PROTOCOL_VERSION,
                resume_token: None,
            })
            .await;
        let first = racer.recv().await.ok_or("closed before answering")?;
        Ok((racer, first))
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

    /// Read until `pick` accepts a message or the server closes the stream.
    async fn until<T>(&mut self, pick: impl Fn(&ServerMessage) -> Option<T>) -> Option<T> {
        for _ in 0..200 {
            let msg = self.recv().await?;
            if let Some(found) = pick(&msg) {
                return Some(found);
            }
        }
        None
    }
}

// ---------------------------------------------------------------- tests

#[tokio::test]
async fn the_page_is_public_and_the_api_is_not() {
    let d = start_dashboard().await;

    let page = http(d.http, "GET", "/", &[], None).await;
    assert_eq!(page.status, 200);
    assert!(page.headers["content-type"].starts_with("text/html"));
    assert!(page.body.contains("ApexSim Server"));
    assert!(page.headers["content-security-policy"].contains("default-src 'self'"));
    assert_eq!(page.headers["x-frame-options"], "DENY");

    for asset in [
        "/app.js",
        "/app.css",
        "/fonts/barlow-700.woff2",
        "/fonts/plex-400.woff2",
    ] {
        assert_eq!(
            http(d.http, "GET", asset, &[], None).await.status,
            200,
            "{asset}"
        );
    }
    assert_eq!(http(d.http, "GET", "/nope", &[], None).await.status, 404);

    for path in ["/api/overview", "/api/players", "/api/config", "/api/logs"] {
        assert_eq!(
            http(d.http, "GET", path, &[], None).await.status,
            401,
            "{path}"
        );
    }
    let wrong = [("Authorization", "Bearer not-the-token")];
    assert_eq!(
        http(d.http, "GET", "/api/overview", &wrong, None)
            .await
            .status,
        401
    );
    assert_eq!(get(&d, "/api/overview").await.status, 200);
}

#[tokio::test]
async fn login_trades_the_token_for_a_cookie_and_locks_out_guessing() {
    let d = start_dashboard().await;

    let bad = http(
        d.http,
        "POST",
        "/api/login",
        &[],
        Some(r#"{"token":"guess"}"#),
    )
    .await;
    assert_eq!(bad.status, 401);
    assert!(!bad.headers.contains_key("set-cookie"));

    let good = http(
        d.http,
        "POST",
        "/api/login",
        &[],
        Some(&format!(r#"{{"token":"{TOKEN}"}}"#)),
    )
    .await;
    assert_eq!(good.status, 200);
    let cookie = &good.headers["set-cookie"];
    assert!(cookie.contains("HttpOnly") && cookie.contains("SameSite=Strict"));
    assert!(!cookie.contains("Secure"), "plain HTTP must not set Secure");
    let session = cookie.split(';').next().unwrap().to_string();
    assert!(!session.contains(TOKEN), "the cookie is not the token");

    let me = http(
        d.http,
        "GET",
        "/api/overview",
        &[("Cookie", &session)],
        None,
    )
    .await;
    assert_eq!(me.status, 200);
    assert_eq!(me.json()["status"], "online");

    let out = http(
        d.http,
        "POST",
        "/api/logout",
        &[("Cookie", &session)],
        Some("{}"),
    )
    .await;
    assert_eq!(out.status, 200);
    assert_eq!(
        http(
            d.http,
            "GET",
            "/api/overview",
            &[("Cookie", &session)],
            None
        )
        .await
        .status,
        401,
        "a signed-out cookie is dead"
    );

    // Five wrong tokens and the address is told to wait, even for the right one.
    for _ in 0..5 {
        http(
            d.http,
            "POST",
            "/api/login",
            &[],
            Some(r#"{"token":"guess"}"#),
        )
        .await;
    }
    let locked = http(
        d.http,
        "POST",
        "/api/login",
        &[],
        Some(&format!(r#"{{"token":"{TOKEN}"}}"#)),
    )
    .await;
    assert_eq!(locked.status, 429);
}

#[tokio::test]
async fn a_write_from_another_origin_is_refused() {
    let d = start_dashboard().await;
    let evil = http(
        d.http,
        "POST",
        "/api/bans/remove",
        &[("Origin", "https://evil.example"), bearer()[0]],
        Some("{}"),
    )
    .await;
    assert_eq!(evil.status, 403);
    // The page's own origin is fine (this one finds no such ban).
    let own = http(
        d.http,
        "POST",
        "/api/bans/remove",
        &[("Origin", "http://localhost"), bearer()[0]],
        Some("{}"),
    )
    .await;
    assert_eq!(own.status, 404);
}

#[tokio::test]
async fn https_serves_the_same_dashboard_with_a_secure_cookie() {
    let d = start_dashboard().await;

    // The listener made a self-signed certificate beside the ban list; trust it.
    let pem = std::fs::read(d.dir.path().join("admin-selfsigned.crt")).expect("generated cert");
    let mut roots = rustls::RootCertStore::empty();
    for cert in rustls_pemfile::certs(&mut pem.as_slice()) {
        roots.add(cert.unwrap()).unwrap();
    }
    let config = rustls::ClientConfig::builder()
        .with_root_certificates(roots)
        .with_no_client_auth();
    let connector = tokio_rustls::TlsConnector::from(Arc::new(config));
    let tls = |addr| {
        let connector = connector.clone();
        async move {
            let tcp = TcpStream::connect(addr).await.unwrap();
            connector
                .connect("localhost".try_into().unwrap(), tcp)
                .await
                .expect("TLS handshake with the generated certificate")
        }
    };

    let page = exchange(tls(d.https).await, "GET", "/", &[], None).await;
    assert_eq!(page.status, 200);
    assert!(page.headers.contains_key("strict-transport-security"));

    let login = exchange(
        tls(d.https).await,
        "POST",
        "/api/login",
        &[],
        Some(&format!(r#"{{"token":"{TOKEN}"}}"#)),
    )
    .await;
    assert_eq!(login.status, 200);
    assert!(login.headers["set-cookie"].contains("Secure"));

    let overview = exchange(tls(d.https).await, "GET", "/api/overview", &bearer(), None).await;
    assert_eq!(overview.json()["https_port"], d.https.port());
    assert_eq!(overview.json()["http_port"], d.http.port());
}

#[tokio::test]
async fn http_can_redirect_to_https() {
    let d = start_dashboard_with(|c| c.admin.redirect_http_to_https = true, None).await;
    let r = http(d.http, "GET", "/api/overview?x=1", &[], None).await;
    assert_eq!(r.status, 308);
    assert_eq!(
        r.headers["location"],
        format!("https://localhost:{}/api/overview?x=1", d.https.port())
    );
}

#[tokio::test]
async fn either_listener_can_be_switched_off() {
    let dir = tempfile::tempdir().unwrap();
    for (http_bind, https_bind) in [("127.0.0.1:0", ""), ("", "127.0.0.1:0")] {
        let mut config = ServerConfig::default();
        config.network.tcp_bind = "127.0.0.1:0".into();
        config.network.udp_bind = "127.0.0.1:0".into();
        config.network.health_bind = "127.0.0.1:0".into();
        config.network.require_tls = false;
        config.content.skip_imported_tracks = cfg!(debug_assertions);
        config.records.dir = dir.path().to_string_lossy().into_owned();
        config.admin.http_bind = http_bind.into();
        config.admin.https_bind = https_bind.into();
        config.admin.token = TOKEN.into();
        config.admin.bans_file = dir.path().join("b.json").to_string_lossy().into_owned();
        let server = run_server_from(config, None).await.unwrap();
        let admin = server.admin.as_ref().unwrap();
        assert_eq!(admin.http_addr.is_some(), !http_bind.is_empty());
        assert_eq!(admin.https_addr.is_some(), !https_bind.is_empty());
    }
}

#[tokio::test]
async fn a_generated_token_is_used_when_none_is_configured() {
    let d = start_dashboard_with(|c| c.admin.token = String::new(), None).await;
    let token = d.server.admin.as_ref().unwrap().token.clone();
    assert_eq!(token.len(), 64);
    let auth = format!("Bearer {token}");
    let ok = http(
        d.http,
        "GET",
        "/api/overview",
        &[("Authorization", &auth)],
        None,
    )
    .await;
    assert_eq!(ok.status, 200);
    assert_eq!(
        get(&d, "/api/overview").await.status,
        401,
        "the empty token is not the token"
    );
}

#[tokio::test]
async fn disabled_means_no_listeners() {
    let dir = tempfile::tempdir().unwrap();
    let mut config = ServerConfig::default();
    config.network.tcp_bind = "127.0.0.1:0".into();
    config.network.udp_bind = "127.0.0.1:0".into();
    config.network.health_bind = "127.0.0.1:0".into();
    config.network.require_tls = false;
    config.content.skip_imported_tracks = cfg!(debug_assertions);
    config.records.dir = dir.path().to_string_lossy().into_owned();
    config.admin.enabled = false;
    let server = run_server_from(config, None).await.unwrap();
    assert!(server.admin.is_none());
}

#[tokio::test]
async fn kick_and_ban_remove_a_connected_client_and_keep_it_out() {
    let d = start_dashboard().await;

    let (mut racer, first) = Racer::connect(d.server.tcp_addr, "Racer One")
        .await
        .unwrap();
    assert!(matches!(first, ServerMessage::AuthSuccess(_)), "{first:?}");

    // It shows up in the player list.
    let mut id = String::new();
    for _ in 0..40 {
        let players = get(&d, "/api/players").await.json();
        if let Some(p) = players["players"]
            .as_array()
            .unwrap()
            .iter()
            .find(|p| p["name"] == "Racer One")
        {
            assert_eq!(p["role"], "LOBBY");
            assert_eq!(p["address"], "127.0.0.1");
            id = p["id"].as_str().unwrap().to_string();
            break;
        }
        sleep(Duration::from_millis(50)).await;
    }
    assert!(!id.is_empty(), "the connected client is listed");
    assert_eq!(get(&d, "/api/overview").await.json()["players"], 1);

    // Kicking tells it why and closes the connection.
    let kick = http(
        d.http,
        "POST",
        &format!("/api/players/{id}/kick"),
        &bearer(),
        Some(r#"{"reason":"too fast"}"#),
    )
    .await;
    assert_eq!(kick.status, 200, "{}", kick.body);
    let why = racer
        .until(|m| match m {
            ServerMessage::Error { code, message } => Some((*code, message.clone())),
            _ => None,
        })
        .await
        .expect("the kick notice");
    assert_eq!(why.0, 403);
    assert!(why.1.contains("too fast"), "{}", why.1);
    assert!(racer.recv().await.is_none(), "the stream is closed");

    // The kicked player may come straight back, and is then banned.
    let (_again, second) = Racer::connect(d.server.tcp_addr, "Racer One")
        .await
        .unwrap();
    assert!(matches!(second, ServerMessage::AuthSuccess(_)));
    let mut id2 = String::new();
    for _ in 0..40 {
        let players = get(&d, "/api/players").await.json();
        if let Some(p) = players["players"]
            .as_array()
            .unwrap()
            .iter()
            .find(|p| p["name"] == "Racer One")
        {
            id2 = p["id"].as_str().unwrap().to_string();
            break;
        }
        sleep(Duration::from_millis(50)).await;
    }
    assert_ne!(id2, id, "a new connection is a new player id");
    let ban = http(
        d.http,
        "POST",
        &format!("/api/players/{id2}/ban"),
        &bearer(),
        Some(r#"{"reason":"cheating"}"#),
    )
    .await;
    assert_eq!(ban.status, 200, "{}", ban.body);

    let bans = get(&d, "/api/bans").await.json();
    assert_eq!(bans["bans"][0]["name"], "Racer One");
    assert_eq!(bans["bans"][0]["reason"], "cheating");
    assert_eq!(bans["bans"][0]["ip"], "127.0.0.1");

    // Banned: refused at login, whatever name it gives (the address is banned).
    let (_denied, refusal) = Racer::connect(d.server.tcp_addr, "Someone Else")
        .await
        .unwrap();
    match refusal {
        ServerMessage::AuthFailure { reason } => assert!(reason.contains("banned"), "{reason}"),
        other => panic!("expected a refusal, got {other:?}"),
    }

    // The list survives on disk, and lifting the ban lets the player in.
    let on_disk = std::fs::read_to_string(d.dir.path().join("bans.json")).unwrap();
    assert!(on_disk.contains("Racer One"));
    let lift = http(
        d.http,
        "POST",
        "/api/bans/remove",
        &bearer(),
        Some(r#"{"name":"Racer One","ip":"127.0.0.1"}"#),
    )
    .await;
    assert_eq!(lift.status, 200);
    let (_back, welcome) = Racer::connect(d.server.tcp_addr, "Racer One")
        .await
        .unwrap();
    assert!(matches!(welcome, ServerMessage::AuthSuccess(_)));

    // An unknown player is a 404, not a crash.
    let ghost = http(
        d.http,
        "POST",
        "/api/players/00000000-0000-0000-0000-000000000000/kick",
        &bearer(),
        Some("{}"),
    )
    .await;
    assert_eq!(ghost.status, 404);
}

#[tokio::test]
async fn a_live_session_is_listed_with_cars_timing_and_a_track_outline() {
    let d = start_dashboard().await;
    let (mut host, first) = Racer::connect(d.server.tcp_addr, "Host").await.unwrap();
    assert!(matches!(first, ServerMessage::AuthSuccess(_)));
    let lobby = host
        .until(|m| match m {
            ServerMessage::LobbyState(l) => Some((
                l.car_configs.first().map(|c| c.id),
                l.track_configs.first().map(|t| t.id),
            )),
            _ => None,
        })
        .await
        .expect("lobby state");
    let (car_id, track_id) = (lobby.0.unwrap(), lobby.1.unwrap());
    host.send(&ClientMessage::SelectCar {
        car_config_id: car_id,
        livery: 0,
    })
    .await;
    host.send(&ClientMessage::CreateSession {
        track_config_id: track_id,
        max_players: 4,
        ai_count: 3,
        lap_limit: 3,
        session_kind: SessionKind::Multiplayer,
        allowed_assists: AllowedAssists::ALL,
        conditions: SessionConditions::default(),
        damage: Default::default(),
        ai_skill: None,
        race_seconds: None,
        grid_order: Vec::new(),
        qualifying_seconds: None,
        weekend: None,
        watch: None,
    })
    .await;
    host.until(|m| matches!(m, ServerMessage::SessionJoined(_)).then_some(()))
        .await
        .expect("session joined");
    host.send(&ClientMessage::StartCountdown {
        countdown_seconds: 1,
        next_mode: GameMode::Race,
    })
    .await;

    let list = get(&d, "/api/sessions").await.json();
    let sessions = list["sessions"].as_array().unwrap();
    assert_eq!(sessions.len(), 1);
    assert_eq!(sessions[0]["humans"], 1);
    assert_eq!(sessions[0]["ai"], 3);
    assert_eq!(sessions[0]["laps"], 3);
    let id = sessions[0]["id"].as_str().unwrap();

    let detail = get(&d, &format!("/api/sessions/{id}")).await.json();
    assert_eq!(detail["laps"], 3);
    assert!(detail["track_length_m"].as_f64().unwrap() > 500.0);
    assert!(detail["outline"].as_array().unwrap().len() > 20);
    let cars = detail["cars"].as_array().unwrap();
    assert_eq!(cars.len(), 4);
    let positions: Vec<u64> = cars.iter().map(|c| c["pos"].as_u64().unwrap()).collect();
    assert_eq!(positions, vec![1, 2, 3, 4]);
    assert_eq!(cars.iter().filter(|c| c["ai"] == false).count(), 1);
    assert!(cars.iter().any(|c| c["name"] == "Host"));
    for car in cars {
        assert_eq!(car["sectors"].as_array().unwrap().len(), 3);
        assert!(car["x"].is_number() && car["speed_kph"].is_number());
    }

    // The players view lists the host (as host) and the AI.
    let players = get(&d, "/api/players").await.json();
    let rows = players["players"].as_array().unwrap();
    assert!(rows
        .iter()
        .any(|p| p["name"] == "Host" && p["role"] == "HOST"));
    assert_eq!(rows.iter().filter(|p| p["role"] == "AI").count(), 3);

    // Other endpoints answer for a server in this state.
    let content = get(&d, "/api/content").await.json();
    assert!(!content["tracks"].as_array().unwrap().is_empty());
    assert!(!content["cars"].as_array().unwrap().is_empty());
    let perf = get(&d, "/api/perf").await.json();
    assert_eq!(perf["target_hz"], 420);
    assert_eq!(get(&d, "/api/history").await.status, 200);

    assert_eq!(get(&d, "/api/sessions/not-a-uuid").await.status, 400);
    assert_eq!(
        get(&d, "/api/sessions/00000000-0000-0000-0000-000000000000")
            .await
            .status,
        404
    );
}

async fn post(d: &Dashboard, path: &str, body: &serde_json::Value) -> Reply {
    http(d.http, "POST", path, &bearer(), Some(&body.to_string())).await
}

/// The first track and car the dashboard offers.
async fn some_track_and_car(d: &Dashboard) -> (String, String) {
    let content = get(d, "/api/content").await.json();
    let first = |k: &str| {
        content[k]
            .as_array()
            .unwrap()
            .iter()
            .find(|x| x["enabled"] == true)
            .map(|x| x["id"].as_str().unwrap().to_string())
            .unwrap()
    };
    (first("tracks"), first("cars"))
}

async fn wait_for(mut check: impl AsyncFnMut() -> bool, what: &str) {
    for _ in 0..100 {
        if check().await {
            return;
        }
        sleep(Duration::from_millis(100)).await;
    }
    panic!("timed out waiting for {what}");
}

/// The roadmap's check: a session run from the dashboard alone. Opened with
/// no player in it, it stays; a player finds it in the lobby and joins; the
/// dashboard sees their ping, talks to them, takes an AI car out, starts
/// it and ends it.
#[tokio::test]
async fn a_session_is_run_from_the_dashboard_alone() {
    let d = start_dashboard().await;
    let (track, car) = some_track_and_car(&d).await;
    let spec = serde_json::json!({
        "name": "Sunday Cup", "track_id": track, "car_id": car,
        "player_slots": 2, "ai_count": 2, "lap_limit": 2,
    });
    let opened = post(&d, "/api/sessions", &spec).await;
    assert_eq!(opened.status, 200, "{}", opened.body);
    let id = opened.json()["id"].as_str().unwrap().to_string();

    // Nobody is in it, and it stays.
    sleep(Duration::from_millis(400)).await;
    let list = get(&d, "/api/sessions").await.json();
    let row = &list["sessions"][0];
    assert_eq!(row["id"], id.as_str());
    assert_eq!(
        (row["hosted"].clone(), row["restartable"].clone()),
        (true.into(), true.into())
    );
    assert_eq!(
        (
            row["state"].as_str(),
            row["humans"].as_u64(),
            row["ai"].as_u64()
        ),
        (Some("lobby"), Some(0), Some(2))
    );

    // A player sees it in the lobby, hosted by the server, and joins.
    let (mut racer, _) = Racer::connect(d.server.tcp_addr, "Racer").await.unwrap();
    let host = racer
        .until(|m| match m {
            ServerMessage::LobbyState(l) => l
                .available_sessions
                .iter()
                .find(|s| s.id.to_string() == id)
                .map(|s| s.host_name.clone()),
            _ => None,
        })
        .await
        .expect("listed in the lobby");
    assert_eq!(host, "Sunday Cup");
    racer
        .send(&ClientMessage::SelectCar {
            car_config_id: car.parse().unwrap(),
            livery: 0,
        })
        .await;
    racer
        .send(&ClientMessage::JoinSession {
            session_id: id.parse().unwrap(),
        })
        .await;
    racer
        .until(|m| matches!(m, ServerMessage::SessionJoined(_)).then_some(()))
        .await
        .expect("joined");

    // Its ping, as the game reports it.
    racer
        .send(&ClientMessage::Heartbeat {
            client_tick: 1,
            ping_ms: Some(37),
        })
        .await;
    wait_for(
        async || {
            get(&d, "/api/players").await.json()["players"]
                .as_array()
                .unwrap()
                .iter()
                .any(|p| p["name"] == "Racer" && p["ping_ms"] == 37)
        },
        "the reported ping",
    )
    .await;

    // A message to the session reaches the player.
    let sent = post(
        &d,
        "/api/broadcast",
        &serde_json::json!({ "message": "Green in two minutes", "session": id }),
    )
    .await
    .json();
    assert_eq!(sent["sent"], 1);
    let notice = racer
        .until(|m| match m {
            ServerMessage::ServerNotice { message } => Some(message.clone()),
            _ => None,
        })
        .await;
    assert_eq!(notice.as_deref(), Some("Green in two minutes"));

    // One AI car out.
    let players = get(&d, "/api/players").await.json();
    let ai = players["players"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["role"] == "AI" && p["session"] == id.as_str())
        .unwrap()["id"]
        .as_str()
        .unwrap()
        .to_string();
    let removed = post(
        &d,
        &format!("/api/sessions/{id}/ai/{ai}/remove"),
        &serde_json::json!({}),
    )
    .await;
    assert_eq!(removed.status, 200, "{}", removed.body);
    let detail = get(&d, &format!("/api/sessions/{id}")).await.json();
    assert_eq!(detail["cars"].as_array().unwrap().len(), 2);
    assert_eq!(
        post(
            &d,
            &format!("/api/sessions/{id}/ai/{ai}/remove"),
            &serde_json::json!({})
        )
        .await
        .status,
        409,
        "gone already"
    );

    // Start: the player hears the countdown.
    let started = post(
        &d,
        &format!("/api/sessions/{id}/start"),
        &serde_json::json!({}),
    )
    .await;
    assert_eq!(started.status, 200, "{}", started.body);
    assert_eq!(started.json()["phase"], "race");
    racer
        .until(|m| matches!(m, ServerMessage::SessionStarting { .. }).then_some(()))
        .await
        .expect("SessionStarting");
    assert_eq!(
        post(
            &d,
            &format!("/api/sessions/{id}/start"),
            &serde_json::json!({})
        )
        .await
        .status,
        409
    );

    // Stop: the player is told why and sent back to the menu.
    let stopped = post(
        &d,
        &format!("/api/sessions/{id}/stop"),
        &serde_json::json!({ "reason": "That's all for today" }),
    )
    .await;
    assert_eq!(stopped.status, 200, "{}", stopped.body);
    let reason = racer
        .until(|m| match m {
            ServerMessage::ServerNotice { message } => Some(message.clone()),
            _ => None,
        })
        .await;
    assert_eq!(reason.as_deref(), Some("That's all for today"));
    racer
        .until(|m| matches!(m, ServerMessage::SessionLeft).then_some(()))
        .await
        .expect("SessionLeft");
    let left = get(&d, "/api/sessions").await.json();
    assert!(left["sessions"].as_array().unwrap().is_empty());
}

#[tokio::test]
async fn a_hosted_session_restarts_with_its_settings() {
    let d = start_dashboard().await;
    let (track, car) = some_track_and_car(&d).await;
    let spec =
        serde_json::json!({ "track_id": track, "car_id": car, "ai_count": 3, "lap_limit": 4 });
    let opened = post(&d, "/api/sessions", &spec).await.json();
    let id = opened["id"].as_str().unwrap().to_string();
    let id = id.as_str();

    // A driver and a spectator in it.
    let (mut driver, _) = Racer::connect(d.server.tcp_addr, "Driver").await.unwrap();
    driver
        .send(&ClientMessage::SelectCar {
            car_config_id: car.parse().unwrap(),
            livery: 0,
        })
        .await;
    driver
        .send(&ClientMessage::JoinSession {
            session_id: id.parse().unwrap(),
        })
        .await;
    let seat = driver
        .until(|m| match m {
            ServerMessage::SessionJoined(j) => Some(j.your_grid_position),
            _ => None,
        })
        .await
        .expect("the driver joins");
    assert!(seat > 0);
    let (mut watcher, _) = Racer::connect(d.server.tcp_addr, "Watcher").await.unwrap();
    watcher
        .send(&ClientMessage::JoinAsSpectator {
            session_id: id.parse().unwrap(),
        })
        .await;
    watcher
        .until(|m| matches!(m, ServerMessage::SessionJoined(_)).then_some(()))
        .await
        .expect("the spectator joins");
    let started = post(
        &d,
        &format!("/api/sessions/{id}/start"),
        &serde_json::json!({}),
    )
    .await;
    assert_eq!(started.status, 200, "{}", started.body);

    let again = post(
        &d,
        &format!("/api/sessions/{id}/restart"),
        &serde_json::json!({}),
    )
    .await;
    assert_eq!(again.status, 200, "{}", again.body);
    let new_id = again.json()["id"].as_str().unwrap().to_string();
    assert_ne!(new_id, id);

    // Both are in the new one at once, with no SessionLeft in between: the
    // driver in a car, the spectator watching.
    let moved = |m: &ServerMessage| match m {
        ServerMessage::SessionLeft => Some(None),
        ServerMessage::SessionJoined(j) => {
            Some(Some((j.session_id.to_string(), j.your_grid_position)))
        }
        _ => None,
    };
    let (driven, seat) = driver
        .until(moved)
        .await
        .flatten()
        .expect("the driver is seated again");
    assert_eq!(driven, new_id);
    assert!(seat > 0);
    let (watched, none) = watcher
        .until(moved)
        .await
        .flatten()
        .expect("the spectator watches again");
    assert_eq!((watched.as_str(), none), (new_id.as_str(), 0));
    let list = get(&d, "/api/sessions").await.json();
    let rows = list["sessions"].as_array().unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0]["id"], new_id.as_str());
    assert_eq!(
        (
            rows[0]["state"].as_str(),
            rows[0]["ai"].as_u64(),
            rows[0]["laps"].as_u64()
        ),
        (Some("lobby"), Some(3), Some(4))
    );
    assert_eq!(rows[0]["humans"].as_u64(), Some(1));

    // Unknown ones are refused.
    let nobody = "00000000-0000-0000-0000-000000000001";
    assert_eq!(
        post(
            &d,
            &format!("/api/sessions/{nobody}/restart"),
            &serde_json::json!({})
        )
        .await
        .status,
        409
    );
    assert_eq!(
        post(
            &d,
            &format!("/api/sessions/{nobody}/stop"),
            &serde_json::json!({})
        )
        .await
        .status,
        409
    );
    assert_eq!(
        post(
            &d,
            "/api/sessions",
            &serde_json::json!({ "track_id": nobody, "car_id": car })
        )
        .await
        .status,
        409
    );
    assert_eq!(
        post(&d, "/api/sessions", &serde_json::json!({ "laps": 3 }))
            .await
            .status,
        400
    );
}

#[tokio::test]
async fn switched_off_content_is_not_offered() {
    let d = start_dashboard().await;
    let (track, car) = some_track_and_car(&d).await;
    let off = post(
        &d,
        "/api/content/toggle",
        &serde_json::json!({ "id": car, "enabled": false }),
    )
    .await;
    assert_eq!(off.status, 200, "{}", off.body);
    let content = get(&d, "/api/content").await.json();
    let row = content["cars"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["id"] == car.as_str())
        .unwrap()
        .clone();
    assert_eq!(row["enabled"], false);
    let kept = std::fs::read_to_string(d.dir.path().join("content.json")).unwrap();
    assert!(kept.contains(&car), "kept in [admin] content_file");

    // Not in the lobby, refused on select and on a hosted create.
    let (mut racer, _) = Racer::connect(d.server.tcp_addr, "Racer").await.unwrap();
    let offered = racer
        .until(|m| match m {
            ServerMessage::LobbyState(l) => {
                Some(l.car_configs.iter().any(|c| c.id.to_string() == car))
            }
            _ => None,
        })
        .await;
    assert_eq!(offered, Some(false));
    racer
        .send(&ClientMessage::SelectCar {
            car_config_id: car.parse().unwrap(),
            livery: 0,
        })
        .await;
    let code = racer
        .until(|m| match m {
            ServerMessage::Error { code, .. } => Some(*code),
            _ => None,
        })
        .await;
    assert_eq!(code, Some(403));
    let refused = post(
        &d,
        "/api/sessions",
        &serde_json::json!({ "track_id": track, "car_id": car }),
    )
    .await;
    assert_eq!(refused.status, 409);

    let on = post(
        &d,
        "/api/content/toggle",
        &serde_json::json!({ "id": car, "enabled": true }),
    )
    .await;
    assert_eq!(on.status, 200);
    assert!(!d
        .server
        .state
        .read()
        .await
        .disabled_content
        .contains(&car.parse().unwrap()));
}

#[tokio::test]
async fn a_scheduled_session_opens_ahead_and_starts_on_time() {
    let d = start_dashboard().await;
    let (track, car) = some_track_and_car(&d).await;
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let entry = serde_json::json!({
        "spec": { "name": "Nightly", "track_id": track, "car_id": car, "ai_count": 2 },
        "start_at": now + 3, "open_minutes": 1, "repeat": "daily",
    });
    let saved = post(&d, "/api/schedule", &entry).await;
    assert_eq!(saved.status, 200, "{}", saved.body);

    // Inside its open window: the session is in the lobby before its start.
    wait_for(
        async || get(&d, "/api/schedule").await.json()["entries"][0]["status"] == "open",
        "the entry to open its session",
    )
    .await;
    let list = get(&d, "/api/sessions").await.json();
    assert_eq!(list["sessions"][0]["state"], "lobby");
    assert_eq!(list["sessions"][0]["hosted"], true);

    // At its time it starts, and the entry moves on a day.
    wait_for(
        async || get(&d, "/api/schedule").await.json()["entries"][0]["status"] == "running",
        "the scheduled start",
    )
    .await;
    let entries = get(&d, "/api/schedule").await.json();
    let e = &entries["entries"][0];
    assert_eq!(e["start_at"].as_u64(), Some(now + 3 + 86_400));
    assert_eq!(e["enabled"], true);
    let list = get(&d, "/api/sessions").await.json();
    assert_ne!(list["sessions"][0]["state"], "lobby");

    let id = e["id"].as_str().unwrap();
    let del = http(
        d.http,
        "DELETE",
        &format!("/api/schedule/{id}"),
        &bearer(),
        None,
    )
    .await;
    assert_eq!(del.status, 200);
    assert!(get(&d, "/api/schedule").await.json()["entries"]
        .as_array()
        .unwrap()
        .is_empty());
}

#[tokio::test]
async fn cars_and_tracks_upload_into_custom_and_load_at_once() {
    let content = tempfile::tempdir().unwrap();
    let cars = content.path().join("cars");
    let tracks = content.path().join("tracks");
    std::fs::create_dir_all(cars.join("default")).unwrap();
    std::fs::create_dir_all(tracks.join("default")).unwrap();
    let shipped = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../content/cars/default/ashton-marvin-amr26/car.toml"),
    )
    .unwrap();
    std::fs::create_dir_all(cars.join("default/amr")).unwrap();
    std::fs::write(cars.join("default/amr/car.toml"), &shipped).unwrap();
    let (cars_dir, tracks_dir) = (
        cars.to_string_lossy().into_owned(),
        tracks.to_string_lossy().into_owned(),
    );
    let d = start_dashboard_with(
        move |c| {
            c.content.cars_dir = cars_dir;
            c.content.tracks_dir = tracks_dir;
        },
        None,
    )
    .await;
    let upload = |kind: &'static str, name: &'static str, body: String| {
        let d = &d;
        async move {
            http(
                d.http,
                "POST",
                &format!("/api/content/upload?kind={kind}&name={name}"),
                &bearer(),
                Some(&body),
            )
            .await
        }
    };

    // A car keeps its own id; a shipped car's id is refused.
    let refused = upload(
        "car",
        "car.toml",
        shipped.replace("Ashton Marvin AMR26", "Copy"),
    )
    .await;
    assert_eq!(refused.status, 422, "{}", refused.body);
    let own = shipped
        .replace(
            "cd10083c-7f03-454a-b7f9-427025fce254",
            "0f0f0f0f-1111-4222-8333-444444444444",
        )
        .replace("name = \"Ashton Marvin AMR26\"", "name = \"Club Special\"");
    let car = upload("car", "car.toml", own.clone()).await;
    assert_eq!(car.status, 200, "{}", car.body);
    assert_eq!(car.json()["name"], "Club Special");
    assert!(cars.join("custom/Club Special/car.toml").is_file());
    // The same car again replaces itself.
    assert_eq!(upload("car", "car.toml", own).await.status, 200);
    assert_eq!(
        upload("car", "car.toml", "not = [toml".into()).await.status,
        422
    );

    // A track: a sidecar, then its YAML, which puts it in the catalog.
    let yaml = "name: Square\ntrack_id: 6f1c2c4e-3b1f-4c8a-9d57-1a2b3c4d5e6f\nnodes:\n  \
                - { x: 0.0, y: 0.0 }\n  - { x: 300.0, y: 0.0 }\n  - { x: 300.0, y: 300.0 }\n  \
                - { x: 0.0, y: 300.0 }\ndefault_width: 12.0\nclosed_loop: true\n";
    assert_eq!(
        upload("track", "Square.notes.txt", "hello".into())
            .await
            .status,
        200
    );
    assert_eq!(
        upload("track", "..%2Fescape.yaml", yaml.into())
            .await
            .status,
        422
    );
    let track = upload("track", "Square.yaml", yaml.into()).await;
    assert_eq!(track.status, 200, "{}", track.body);
    assert_eq!(track.json()["track"]["name"], "Square");
    assert!(tracks.join("custom/Square/Square.notes.txt").is_file());
    let broken = upload("track", "Square.yaml", "nodes: [".into()).await;
    assert_eq!(broken.status, 422);
    assert_eq!(
        std::fs::read_to_string(tracks.join("custom/Square/Square.yaml")).unwrap(),
        yaml,
        "a file that does not load leaves the old one"
    );

    let listed = get(&d, "/api/content").await.json();
    assert!(listed["cars"]
        .as_array()
        .unwrap()
        .iter()
        .any(|c| c["name"] == "Club Special"));
    assert!(listed["tracks"]
        .as_array()
        .unwrap()
        .iter()
        .any(|t| t["name"] == "Square" && t["source"] == "CUSTOM"));
}

#[tokio::test]
async fn the_config_editor_validates_backs_up_and_reports_what_changed() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("server.toml");
    let mut base = ServerConfig::default();
    base.content.cars_dir = "../content/cars".into();
    base.content.tracks_dir = "../content/tracks".into();
    let original = toml::to_string_pretty(&base).unwrap();
    std::fs::write(&path, &original).unwrap();

    let d = start_dashboard_with(|_| {}, Some(path.clone())).await;
    let read = get(&d, "/api/config").await.json();
    assert_eq!(read["text"], original.as_str());
    assert_eq!(read["path"], "server.toml");
    assert_eq!(read["has_backup"], false);

    let put = |text: &str| {
        let body = serde_json::json!({ "text": text }).to_string();
        let d = &d;
        async move { http(d.http, "PUT", "/api/config", &bearer(), Some(&body)).await }
    };

    // Not TOML, not a config, and a config the server would refuse to start with.
    assert_eq!(put("this is = = not toml").await.status, 422);
    assert_eq!(put("[server]\ntick_rate_hz = \"fast\"").await.status, 422);
    let too_slow = original.replace("tick_rate_hz = 420", "tick_rate_hz = 5");
    let refused = put(&too_slow).await;
    assert_eq!(refused.status, 422);
    assert!(refused.json()["error"]
        .as_str()
        .unwrap()
        .contains("tick_rate_hz"));
    assert_eq!(
        std::fs::read_to_string(&path).unwrap(),
        original,
        "nothing was written"
    );

    let faster = original.replace("tick_rate_hz = 420", "tick_rate_hz = 240");
    let saved = put(&faster).await;
    assert_eq!(saved.status, 200, "{}", saved.body);
    let body = saved.json();
    assert_eq!(body["changed"], serde_json::json!(["server"]));
    assert_eq!(body["restart_required"], true);
    assert_eq!(std::fs::read_to_string(&path).unwrap(), faster);
    assert_eq!(
        std::fs::read_to_string(dir.path().join("server.toml.bak")).unwrap(),
        original,
        "the previous file is kept"
    );

    assert_eq!(
        body["restart_keys"],
        serde_json::json!(["server.tick_rate_hz"])
    );
    assert_eq!(body["applied"], serde_json::json!([]));

    // Saving the same text again changes nothing.
    let again = put(&faster).await.json();
    assert_eq!(again["restart_required"], false);

    // A live key goes into force as it is saved, with no restart.
    let fewer = faster.replace("max_sessions = 8", "max_sessions = 3");
    assert_ne!(fewer, faster);
    let live = put(&fewer).await.json();
    assert_eq!(live["applied"], serde_json::json!(["server.max_sessions"]));
    assert_eq!(live["restart_required"], false);
    assert_eq!(d.server.state.read().await.config.server.max_sessions, 3);
    // ... while what needs a restart has not moved.
    assert_eq!(d.server.state.read().await.config.server.tick_rate_hz, 420);
}

#[tokio::test]
async fn without_a_config_file_the_editor_says_so() {
    let d = start_dashboard().await;
    assert_eq!(get(&d, "/api/config").await.status, 404);
    assert_eq!(
        get(&d, "/api/overview").await.json()["config_editable"],
        false
    );
}

#[tokio::test]
async fn the_log_endpoint_filters_by_level_and_text() {
    use tracing_subscriber::layer::SubscriberExt;
    let d = start_dashboard().await;
    // The ring is process-wide; install the layer for this thread only.
    let subscriber =
        tracing_subscriber::registry().with(apexsim_server::admin::logbuf::LogBufferLayer);
    tracing::subscriber::with_default(subscriber, || {
        tracing::info!("dashboard-test info line");
        tracing::warn!("dashboard-test warn line");
        tracing::error!("dashboard-test error line");
    });

    let all = get(&d, "/api/logs?q=dashboard-test").await.json();
    assert_eq!(all["lines"].as_array().unwrap().len(), 3);
    let warn = get(&d, "/api/logs?q=dashboard-test&level=WARN")
        .await
        .json();
    let levels: Vec<&str> = warn["lines"]
        .as_array()
        .unwrap()
        .iter()
        .map(|l| l["level"].as_str().unwrap())
        .collect();
    assert_eq!(levels, vec!["WARN", "ERROR"]);

    // A poller that asks for what is after the last line it saw gets nothing.
    let seq = all["last_seq"].as_u64().unwrap();
    let none = get(&d, &format!("/api/logs?q=dashboard-test&after={seq}"))
        .await
        .json();
    assert!(none["lines"].as_array().unwrap().is_empty());

    let dl = get(&d, "/api/logs/download?q=dashboard-test").await;
    assert_eq!(dl.status, 200);
    assert!(dl.headers["content-disposition"].contains("attachment"));
    assert!(dl.body.contains("dashboard-test error line"));
}

/// Dev harness, not a test: serves the dashboard over a racing session so
/// the pages can be looked at in a browser.
///
/// `cargo test --test admin_test serve_demo_dashboard -- --ignored --nocapture`
/// then open http://127.0.0.1:19003 and sign in with the token `demo`.
/// `ADMIN_DEMO_SECONDS` (default 600) is how long it runs.
#[tokio::test]
#[ignore]
async fn serve_demo_dashboard() {
    let d = start_dashboard_with(
        |c| {
            c.admin.http_bind = "127.0.0.1:19003".into();
            c.admin.https_bind = "127.0.0.1:19004".into();
            c.admin.token = "demo".into();
            c.network.tcp_bind = "127.0.0.1:19000".into();
            c.network.udp_bind = "127.0.0.1:19001".into();
            c.network.health_bind = "127.0.0.1:19002".into();
        },
        None,
    )
    .await;
    let (mut host, _) = Racer::connect(d.server.tcp_addr, "R. Voss").await.unwrap();
    let lobby = host
        .until(|m| match m {
            ServerMessage::LobbyState(l) => Some((
                l.car_configs.first().map(|c| c.id),
                l.track_configs
                    .iter()
                    .find(|t| t.name.contains("Monza"))
                    .or(l.track_configs.first())
                    .map(|t| t.id),
            )),
            _ => None,
        })
        .await
        .expect("lobby state");
    host.send(&ClientMessage::SelectCar {
        car_config_id: lobby.0.unwrap(),
        livery: 0,
    })
    .await;
    host.send(&ClientMessage::CreateSession {
        track_config_id: lobby.1.unwrap(),
        max_players: 20,
        ai_count: 11,
        lap_limit: 5,
        session_kind: SessionKind::Multiplayer,
        allowed_assists: AllowedAssists::ALL,
        conditions: SessionConditions::default(),
        damage: Default::default(),
        ai_skill: None,
        race_seconds: None,
        grid_order: Vec::new(),
        qualifying_seconds: None,
        weekend: None,
        watch: None,
    })
    .await;
    host.until(|m| matches!(m, ServerMessage::SessionJoined(_)).then_some(()))
        .await
        .expect("session joined");
    host.send(&ClientMessage::StartCountdown {
        countdown_seconds: 3,
        next_mode: GameMode::Race,
    })
    .await;
    let (mut lurker, _) = Racer::connect(d.server.tcp_addr, "M. Sato").await.unwrap();
    let seconds: u64 = std::env::var("ADMIN_DEMO_SECONDS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(600);
    println!(
        "dashboard on http://127.0.0.1:19003 and https://127.0.0.1:19004 (token: demo) for {seconds} s"
    );
    // Heartbeats keep both clients connected; the host's telemetry is drained.
    let end = tokio::time::Instant::now() + Duration::from_secs(seconds);
    let mut tick = 0u32;
    while tokio::time::Instant::now() < end {
        tick += 1;
        host.send(&ClientMessage::Heartbeat {
            client_tick: tick,
            ping_ms: Some(24),
        })
        .await;
        lurker
            .send(&ClientMessage::Heartbeat {
                client_tick: tick,
                ping_ms: Some(61),
            })
            .await;
        let _ = timeout(Duration::from_millis(400), async {
            while host.recv().await.is_some() {}
        })
        .await;
    }
}

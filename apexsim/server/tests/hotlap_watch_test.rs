//! `SessionKind::HotlapWatch`: one AI car laps the track alone, on the line
//! and at the speed its car can manage, for the creator to watch (Main menu
//! > Garage > Tracks > Watch hotlap).
//!
//! The first test drives a `GameSession` directly (no network, no real-time
//! waits), like `fuel_test`; the second goes over the wire.

mod common;

use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::Path;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{timeout, Instant};
use uuid::Uuid;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::{
    GameSession, HOTLAP_WATCH_CLIMB_LAPS, HOTLAP_WATCH_HOLD_S, HOTLAP_WATCH_SKILL,
};
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage};
use apexsim_server::session_wire::SessionWire;
use apexsim_server::track_guide::wire_corners;
use apexsim_server::track_loader::TrackLoader;

const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

const MONZA: &str = "../../apexsim-content/tracks/default/Monza/Monza.yaml";

fn car(folder: &str) -> CarConfig {
    let path = format!("../../apexsim-content/cars/default/{folder}/car.toml");
    CarLoader::load_from_file(Path::new(&path)).expect("car loads")
}

/// A watched hotlap at Monza in `folder`'s car, as the server builds it.
fn watched_hotlap(folder: &str, conditions: SessionConditions) -> (GameSession, PlayerId) {
    let car = car(folder);
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let mut profile = AiDriverProfile::new("Hotlap", HOTLAP_WATCH_SKILL).with_car(car.id);
    profile.id = Uuid::from_u128(9100);
    profile.exact_line = true;
    let driver = profile.id;

    let mut track = TrackLoader::load_from_file(MONZA).expect("Monza loads");
    conditions.apply_to_track(&mut track);
    let corners = wire_corners(&track, Some(Path::new(MONZA)));

    let mut session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::HotlapWatch,
        1,
        1,
        3,
    );
    session.host_car_id = Some(car.id);
    session.conditions = conditions;
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.set_track_corners(corners);
    gs.start_hotlap_watch();
    (gs, driver)
}

/// Drive the watched car until it has closed `laps` laps (or `max_s` of
/// session time has gone); the lap times, s, and whether each stood.
fn drive_laps(
    gs: &mut GameSession,
    driver: PlayerId,
    laps: usize,
    max_s: usize,
) -> Vec<(f32, bool)> {
    let mut out = Vec::new();
    for _ in 0..(HZ * max_s) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        for event in gs.take_lap_events() {
            if let Some(ms) = event.event.lap_time_ms {
                out.push((ms as f32 / 1000.0, event.event.valid));
            }
        }
        if out.len() >= laps {
            break;
        }
    }
    out
}

#[test]
fn a_watched_hotlap_starts_on_the_run_up_and_holds_there() {
    let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);

    assert_eq!(gs.session.game_mode, GameMode::Hotlap);
    assert_eq!(gs.session.state, SessionState::Racing);
    assert!(gs.is_hotlap_watch());
    assert_eq!(gs.session.participants.len(), 1, "one car, the AI's");
    assert!(gs.track_corners_message(gs.session.id).is_some());

    // Out on the run-up, ahead of nothing, and not in a garage.
    let start = gs.session.participants[&driver].clone();
    assert!(!start.in_garage);
    assert!(
        start.track_progress > gs.track_length_m() * 0.5,
        "{} m of {}: it starts before the line",
        start.track_progress,
        gs.track_length_m()
    );

    // Held: a few seconds in, it has not moved.
    for _ in 0..(HZ * (HOTLAP_WATCH_HOLD_S as usize - 1)) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
    }
    let held = &gs.session.participants[&driver];
    assert!(held.speed_mps < 0.5, "held at {} m/s", held.speed_mps);
    assert!(
        (held.pos_x - start.pos_x).hypot(held.pos_y - start.pos_y) < 1.0,
        "held in place"
    );
}

#[test]
fn a_watched_hotlap_laps_the_track_alone_lap_after_lap() {
    let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    let laps = drive_laps(&mut gs, driver, 4, 600);
    assert_eq!(laps.len(), 4, "four laps closed: {laps:?}");
    // The exact line uses the kerbs. Without the curb sidecar (a checkout
    // that has not baked the tracks, as in CI) the road edge is the track
    // limit and a lap on that line is struck, which also steps the driver
    // down its ladder, so the laps are neither all legal nor the same lap:
    // both are only judged where the kerbs are there to count.
    let kerbs = gs.track_config.curbs.is_some();
    if !kerbs {
        eprintln!("Monza has no curbs sidecar: lap validity and drift not checked");
    }
    for (i, (time, valid)) in laps.iter().enumerate() {
        assert!(
            (60.0..200.0).contains(time),
            "lap {} takes {time:.1} s, not a Monza lap",
            i + 1
        );
        assert!(*valid || !kerbs, "lap {} was struck: {laps:?}", i + 1);
    }
    // From the second lap it is at the line at speed on fresh tyres, lap
    // after lap, so the laps are the same lap.
    let (second, third, fourth) = (laps[1].0, laps[2].0, laps[3].0);
    if kerbs {
        assert!(
            (second - third).abs() < 0.5 && (third - fourth).abs() < 0.5,
            "laps drift: {laps:?}"
        );
        // The first is from the run-up at speed, not a standing start: the
        // same lap to within a second or so, not the slowest by ten.
        assert!(
            (laps[0].0 - second).abs() < 2.0,
            "the lap from the run-up is not the same lap: {laps:?}"
        );
    }

    // Fresh tyres at the line: nothing wears a long watch out.
    let state = &gs.session.participants[&driver];
    let worn = state
        .tires
        .each()
        .iter()
        .map(|t| t.wear_percent)
        .fold(0.0, f32::max);
    assert!(worn < 3.0, "tyres worn {worn:.1}% at the start of a lap");
}

#[test]
fn a_different_car_is_a_different_lap() {
    let (mut gt3, gt3_driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    let (mut f1, f1_driver) = watched_hotlap("fugazzi-sf26", SessionConditions::DEFAULT);
    let gt3_laps = drive_laps(&mut gt3, gt3_driver, 2, 400);
    let f1_laps = drive_laps(&mut f1, f1_driver, 2, 400);
    assert_eq!((gt3_laps.len(), f1_laps.len()), (2, 2));
    assert!(
        f1_laps[1].0 < gt3_laps[1].0 - 5.0,
        "an F1 car {:.1} s, a GT3 {:.1} s",
        f1_laps[1].0,
        gt3_laps[1].0
    );
}

#[test]
fn rain_slows_the_watched_lap() {
    let wet = SessionConditions {
        weather: Weather::HeavyRain,
        ..SessionConditions::DEFAULT
    };
    let (mut dry, dry_driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    let (mut rain, rain_driver) = watched_hotlap("posh-gt3rs", wet);
    let dry_laps = drive_laps(&mut dry, dry_driver, 2, 400);
    let rain_laps = drive_laps(&mut rain, rain_driver, 2, 600);
    assert_eq!((dry_laps.len(), rain_laps.len()), (2, 2));
    assert!(
        rain_laps[1].0 > dry_laps[1].0 + 3.0,
        "dry {:.1} s, heavy rain {:.1} s",
        dry_laps[1].0,
        rain_laps[1].0
    );
}

#[test]
fn a_watched_hotlap_is_deterministic() {
    let run = || {
        let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
        drive_laps(&mut gs, driver, 2, 400)
    };
    assert_eq!(run(), run());
}

#[test]
fn the_corners_are_numbered_in_lap_order() {
    let track = TrackLoader::load_from_file(MONZA).expect("Monza loads");
    let corners = wire_corners(&track, Some(Path::new(MONZA)));
    assert!(corners.len() >= 5, "Monza has {} corners", corners.len());
    for (i, corner) in corners.iter().enumerate() {
        assert_eq!(corner.number as usize, i + 1);
    }
    assert!(
        corners.windows(2).all(|w| w[0].apex_m < w[1].apex_m),
        "in lap order: {:?}",
        corners.iter().map(|c| c.apex_m).collect::<Vec<_>>()
    );
    let lap = track.centerline.last().unwrap().distance_from_start_m;
    for corner in &corners {
        for station in [corner.entry_m, corner.apex_m, corner.exit_m] {
            assert!((0.0..lap).contains(&station), "{station} outside the lap");
        }
    }
}

// --- Over the wire ----------------------------------------------------------

struct Client {
    stream: TcpStream,
    last_heartbeat: Instant,
    heartbeat_tick: u32,
}

impl Client {
    async fn connect(name: &str, addr: SocketAddr) -> (Self, LobbyStateData) {
        let stream = TcpStream::connect(addr).await.expect("connect");
        let mut client = Self {
            stream,
            last_heartbeat: Instant::now(),
            heartbeat_tick: 0,
        };
        client
            .send(&ClientMessage::Authenticate {
                token: format!("test_token_{}", name),
                player_name: name.to_string(),
                protocol_version: apexsim_server::network::PROTOCOL_VERSION,
                resume_token: None,
            })
            .await;
        assert!(matches!(client.recv().await, ServerMessage::AuthSuccess(_)));
        let lobby = client
            .recv_until(|msg| match msg {
                ServerMessage::LobbyState(data) => Some(data),
                _ => None,
            })
            .await;
        (client, lobby)
    }

    async fn send(&mut self, msg: &ClientMessage) {
        let data = rmp_serde::to_vec_named(msg).expect("encode");
        self.stream
            .write_all(&(data.len() as u32).to_be_bytes())
            .await
            .expect("write length");
        self.stream.write_all(&data).await.expect("write body");
        self.stream.flush().await.expect("flush");
    }

    async fn recv(&mut self) -> ServerMessage {
        let mut len = [0u8; 4];
        timeout(Duration::from_secs(5), self.stream.read_exact(&mut len))
            .await
            .expect("timed out waiting for a message")
            .expect("read length");
        let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
        self.stream.read_exact(&mut buf).await.expect("read body");
        rmp_serde::from_slice(&buf).expect("decode")
    }

    async fn recv_until<T>(&mut self, mut pick: impl FnMut(ServerMessage) -> Option<T>) -> T {
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            assert!(Instant::now() < deadline, "expected message never arrived");
            if self.last_heartbeat.elapsed() > Duration::from_secs(1) {
                self.last_heartbeat = Instant::now();
                self.heartbeat_tick += 1;
                let client_tick = self.heartbeat_tick;
                self.send(&ClientMessage::Heartbeat {
                    client_tick,
                    ping_ms: None,
                })
                .await;
            }
            if let Some(found) = pick(self.recv().await) {
                return found;
            }
        }
    }
}

fn create_hotlap_watch(track: TrackConfigId) -> ClientMessage {
    ClientMessage::CreateSession {
        track_config_id: track,
        max_players: 8,
        ai_count: 5,
        lap_limit: 3,
        session_kind: SessionKind::HotlapWatch,
        allowed_assists: Default::default(),
        conditions: SessionConditions {
            weather: Weather::Overcast,
            time_of_day_minutes: 17 * 60,
            ..SessionConditions::DEFAULT
        },
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
    }
}

#[tokio::test]
async fn test_a_watched_hotlap_is_private_and_sent_to_its_creator() {
    let server = common::start_test_server().await;
    let (mut host, lobby) = Client::connect("Watcher", server.tcp_addr).await;
    let car = lobby.car_configs.first().expect("a car").id;
    let track = lobby
        .track_configs
        .iter()
        .find(|t| t.name.contains("Monza"))
        .or(lobby.track_configs.first())
        .expect("a track")
        .id;
    host.send(&ClientMessage::SelectCar {
        car_config_id: car,
        livery: 0,
    })
    .await;
    host.send(&create_hotlap_watch(track)).await;

    let (mut joined, mut sectors, mut corners) = (None, None, None);
    while joined.is_none() || sectors.is_none() || corners.is_none() {
        match host.recv_until(Some).await {
            ServerMessage::SessionJoined(data) => joined = Some(data),
            ServerMessage::TrackSectors(data) => sectors = Some(data),
            ServerMessage::TrackCorners(data) => corners = Some(data),
            ServerMessage::Error { message, .. } => panic!("creation failed: {message}"),
            _ => {}
        }
    }
    let (joined, sectors, corners) = (joined.unwrap(), sectors.unwrap(), corners.unwrap());
    assert_eq!(joined.session_kind, SessionKind::HotlapWatch);
    assert_eq!(joined.your_grid_position, 0, "the host watches");
    assert_eq!(joined.conditions.weather, Weather::Overcast);
    assert_eq!(joined.conditions.time_of_day_minutes, 17 * 60);
    assert!(sectors.track_length_m > 0.0);
    assert_eq!(corners.session_id, joined.session_id);
    assert!(!corners.corners.is_empty());

    {
        let state = server.state.read().await;
        let game = &state.sessions[&joined.session_id];
        assert_eq!(game.session.participants.len(), 1, "one AI car, no human");
        assert_eq!(game.session.game_mode, GameMode::Hotlap);
        assert_eq!(game.session.state, SessionState::Racing);
        let driver = game.session.participants.values().next().unwrap();
        assert_eq!(driver.car_config_id, car, "the host's own car");
    }

    // Unlisted, and nobody else can take a seat.
    let (mut other, _) = Client::connect("Other", server.tcp_addr).await;
    let listed = other
        .recv_until(|msg| match msg {
            ServerMessage::LobbyState(data) => Some(data.available_sessions),
            _ => None,
        })
        .await;
    assert!(listed.is_empty(), "a watched hotlap is not listed");
    other
        .send(&ClientMessage::JoinSession {
            session_id: joined.session_id,
        })
        .await;
    other
        .recv_until(|msg| match msg {
            ServerMessage::Error { .. } => Some(()),
            ServerMessage::SessionJoined(_) => panic!("joined a watched hotlap"),
            _ => None,
        })
        .await;

    // Asking for another one (another car, another sky) replaces it, with
    // nothing said to the client but the new join.
    host.send(&create_hotlap_watch(track)).await;
    let second = host
        .recv_until(|msg| match msg {
            ServerMessage::SessionJoined(data) => Some(data),
            ServerMessage::SessionLeft => panic!("the replacement was announced as a leave"),
            _ => None,
        })
        .await;
    assert_ne!(second.session_id, joined.session_id);
    let state = server.state.read().await;
    assert!(!state.sessions.contains_key(&joined.session_id));
    assert!(state.sessions.contains_key(&second.session_id));
    drop(state);

    // Leaving takes it away.
    host.send(&ClientMessage::LeaveSession).await;
    host.recv_until(|msg| matches!(msg, ServerMessage::SessionLeft).then_some(()))
        .await;
    assert!(server.state.read().await.sessions.is_empty());
}

/// The probe: `WATCH_TRACKS=Monza,Spa WATCH_CARS=posh-gt3rs,fugazzi-sf26 WATCH_LAPS=5`
/// prints each watched lap, and how far the driver was eased off the exact
/// line by the time it settled.
/// `cargo test --release --test hotlap_watch_test watch_probe -- --ignored --nocapture`
#[test]
#[ignore]
fn watch_probe() {
    let list = |name: &str, default: &str| -> Vec<String> {
        std::env::var(name)
            .unwrap_or_else(|_| default.to_string())
            .split(',')
            .map(str::to_string)
            .collect()
    };
    let laps: usize = std::env::var("WATCH_LAPS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(5);
    for stem in list("WATCH_TRACKS", "Monza") {
        for folder in list("WATCH_CARS", "posh-gt3rs") {
            let car = car(&folder);
            let mut car_configs = HashMap::new();
            car_configs.insert(car.id, car.clone());
            let mut profile = AiDriverProfile::new("Hotlap", HOTLAP_WATCH_SKILL).with_car(car.id);
            profile.id = Uuid::from_u128(9100);
            profile.exact_line = true;
            let driver = profile.id;
            let path = format!("../../apexsim-content/tracks/default/{stem}/{stem}.yaml");
            let track = TrackLoader::load_from_file(&path).expect("track loads");
            let mut session = RaceSession::new(
                Uuid::from_u128(1),
                track.id,
                SessionKind::HotlapWatch,
                1,
                1,
                3,
            );
            session.host_car_id = Some(car.id);
            let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
            gs.spawn_ai_drivers();
            gs.start_hotlap_watch();
            let mut line = Vec::new();
            for _ in 0..(HZ * 60 * 12) {
                let inputs: HashMap<PlayerId, PlayerInputData> =
                    [(driver, gs.generate_ai_input(&driver))].into();
                gs.tick(&inputs);
                for event in gs.take_lap_events() {
                    if let Some(ms) = event.event.lap_time_ms {
                        line.push(format!(
                            "{:.2}{}",
                            ms as f32 / 1000.0,
                            if event.event.valid { "" } else { "!" }
                        ));
                    }
                }
                if line.len() >= laps {
                    break;
                }
            }
            println!(
                "{stem:12} {folder:22} level {}  {}",
                gs.hotlap_watch_level(),
                line.join("  ")
            );
        }
    }
}

#[test]
fn a_watch_can_start_down_the_ladder_where_an_earlier_one_left_off() {
    let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    assert_eq!(gs.hotlap_watch_level(), 0);
    let key = gs.hotlap_watch_key().expect("a key for the cache");
    assert_eq!(key.1, gs.session.participants[&driver].car_config_id);

    gs.set_hotlap_watch_level(1);
    assert_eq!(gs.hotlap_watch_level(), 1);
    let profile = &gs.ai_profiles[&driver];
    assert!(!profile.exact_line, "off the exact line");
    assert_eq!(
        profile.skill_level, HOTLAP_WATCH_SKILL,
        "level 1 is still the top skill"
    );

    // Setting it again starts from the top, not on top of itself.
    gs.set_hotlap_watch_level(3);
    assert_eq!(gs.hotlap_watch_level(), 3);
    assert!(gs.ai_profiles[&driver].skill_level < HOTLAP_WATCH_SKILL);
}

#[test]
fn the_fastest_watched_lap_is_the_ghost_and_the_stream_carries_it() {
    use apexsim_server::session_stream::SessionStream;
    use apexsim_server::spectator::{Record, RECORD_GHOST};

    let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    let kerbs = gs.track_config.curbs.is_some();
    let names = HashMap::new();
    let mut stream = SessionStream::new(&gs, 30, &names);
    let mut ghosts = Vec::new();
    let mut laps = Vec::new();
    for _ in 0..(HZ * 400) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let events = gs.take_lap_events();
        for event in &events {
            if let Some(ms) = event.event.lap_time_ms {
                laps.push((ms, event.event.valid));
            }
        }
        let out = stream.tick(&gs, &events, &[], &names, false);
        ghosts.extend(
            out.records
                .into_iter()
                .filter(|r| r.kind == RECORD_GHOST)
                .map(|r| r.body),
        );
        if laps.len() >= 3 {
            break;
        }
    }
    let best_legal = laps
        .iter()
        .filter(|(_, valid)| *valid)
        .map(|(ms, _)| *ms)
        .min();
    let Some(best) = best_legal else {
        assert!(!kerbs, "no legal lap in {laps:?}");
        eprintln!("Monza has no curbs sidecar: every lap struck, no ghost to check");
        return;
    };
    let (_, kept) = gs.hotlap_watch_ghost().expect("a ghost after a legal lap");
    assert_eq!(kept.lap_time_ms, best, "the fastest legal lap: {laps:?}");
    // One record each time a faster lap replaced the ghost, the last the best.
    assert!(!ghosts.is_empty());
    let Record::Ghost(last) = Record::decode(ghosts.last().unwrap()).unwrap() else {
        panic!("not a ghost record");
    };
    assert_eq!(last.lap_time_ms, best);
    assert_eq!(last.track_id, gs.session.track_config_id.to_string());
    let expected = (best as f32 / 1000.0 * 20.0) as usize;
    assert!(
        last.samples.len().abs_diff(expected) <= 2,
        "{} samples for a {best} ms lap",
        last.samples.len()
    );
    // A viewer joining now is sent it with the catch-up.
    assert!(stream
        .catch_up(&gs)
        .iter()
        .any(|body| apexsim_server::spectator::record_type(body) == Some(RECORD_GHOST)));
}

/// A driver eased down by an earlier watch climbs back a step after
/// `HOTLAP_WATCH_CLIMB_LAPS` legal laps in a row.
#[test]
fn clean_laps_climb_the_ladder_back() {
    let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    if gs.track_config.curbs.is_none() {
        eprintln!("Monza has no curbs sidecar: the laps are struck, nothing to climb on");
        return;
    }
    gs.set_hotlap_watch_level(2);
    let laps = drive_laps(&mut gs, driver, HOTLAP_WATCH_CLIMB_LAPS as usize, 600);
    assert!(laps.iter().all(|(_, valid)| *valid), "{laps:?}");
    assert_eq!(gs.hotlap_watch_level(), 1, "a step back up: {laps:?}");
}

/// The ladder is kept per sky: another hour, air or wind is another key.
#[test]
fn another_sky_is_another_ladder() {
    let (noon, _) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    let dusk = SessionConditions {
        time_of_day_minutes: 19 * 60,
        ..SessionConditions::DEFAULT
    };
    let (evening, _) = watched_hotlap("posh-gt3rs", dusk);
    let windy = SessionConditions {
        wind_kph: Some(40),
        ..SessionConditions::DEFAULT
    };
    let (gusty, _) = watched_hotlap("posh-gt3rs", windy);
    let (a, b, c) = (
        noon.hotlap_watch_key().unwrap(),
        evening.hotlap_watch_key().unwrap(),
        gusty.hotlap_watch_key().unwrap(),
    );
    assert_eq!((a.0, a.1, a.2, a.3), (b.0, b.1, b.2, b.3));
    assert!(a.4 != b.4 && a.4 != c.4 && b.4 != c.4);
}

#[test]
fn a_setup_is_a_ladder_of_its_own() {
    use apexsim_server::car_setup::CarSetup;
    let (mut gs, driver) = watched_hotlap("posh-gt3rs", SessionConditions::DEFAULT);
    let stock = gs.hotlap_watch_key().unwrap();
    assert_eq!(stock.3, 0, "stock is key 0");
    let soft = CarSetup {
        rear_wing: 2,
        ..CarSetup::default()
    };
    gs.set_hotlap_watch_setup(soft);
    let tuned = gs.hotlap_watch_key().unwrap();
    assert_ne!(tuned.3, 0);
    assert_eq!((tuned.0, tuned.1, tuned.2), (stock.0, stock.1, stock.2));
    assert_eq!(gs.car_setup(&driver), soft.clamp(), "the car runs it");
}

fn create_streamed_hotlap_watch(track: TrackConfigId) -> ClientMessage {
    let ClientMessage::CreateSession {
        track_config_id,
        max_players,
        ai_count,
        lap_limit,
        session_kind,
        allowed_assists,
        conditions,
        damage,
        ai_skill,
        race_seconds,
        grid_order,
        qualifying_seconds,
        weekend,
        ..
    } = create_hotlap_watch(track)
    else {
        unreachable!()
    };
    ClientMessage::CreateSession {
        track_config_id,
        max_players,
        ai_count,
        lap_limit,
        session_kind,
        allowed_assists,
        conditions,
        damage,
        ai_skill,
        race_seconds,
        grid_order,
        qualifying_seconds,
        weekend,
        watch: Some(apexsim_server::network::HotlapWatchOptions {
            stream: true,
            setup: Some(apexsim_server::car_setup::CarSetup {
                rear_wing: 1,
                ..Default::default()
            }),
        }),
        qualifying_cutoff_pct: None,
        mandatory_stops: 0,
        compound_change: false,
        qualifying_format: QualifyingFormat::Open,
        qualifying_lap_allowance: 0,
        race_rules: Default::default(),
    }
}

#[tokio::test]
async fn test_a_hotlap_watched_as_a_stream_goes_with_its_viewer() {
    let server = common::start_test_server().await;
    let (mut host, lobby) = Client::connect("StreamWatcher", server.tcp_addr).await;
    let car = lobby.car_configs.first().expect("a car").id;
    let track = lobby
        .track_configs
        .iter()
        .find(|t| t.name.contains("Monza"))
        .or(lobby.track_configs.first())
        .expect("a track")
        .id;
    host.send(&ClientMessage::SelectCar {
        car_config_id: car,
        livery: 0,
    })
    .await;
    host.send(&create_streamed_hotlap_watch(track)).await;

    let (mut joined, mut corners, mut header) = (None, None, false);
    while joined.is_none() || corners.is_none() || !header {
        match host.recv_until(Some).await {
            ServerMessage::SpectatorJoined(data) => joined = Some(data),
            ServerMessage::TrackCorners(data) => corners = Some(data),
            ServerMessage::SpectatorRecord(bytes) => {
                header |= bytes.0.windows(2).any(|w| w[1] == 1 && w[0] & 0xF0 == 0x90);
            }
            ServerMessage::SessionJoined(_) => panic!("a streamed watch is not joined"),
            ServerMessage::Error { message, .. } => panic!("creation failed: {message}"),
            _ => {}
        }
    }
    let joined = joined.unwrap();
    assert_eq!(corners.unwrap().session_id, joined.stream_id);
    let session_id = joined.stream_id;
    {
        let state = server.state.read().await;
        let game = &state.sessions[&session_id];
        assert_eq!(game.session.session_kind, SessionKind::HotlapWatch);
        let driver = *game.session.participants.keys().next().unwrap();
        assert_eq!(
            game.car_setup(&driver).rear_wing,
            1,
            "the car runs the setup"
        );
        let host_id = game.session.host_player_id;
        assert_eq!(state.live.watching(&host_id), Some(session_id));
    }

    // Another watch replaces it.
    host.send(&create_streamed_hotlap_watch(track)).await;
    let second = host
        .recv_until(|msg| match msg {
            ServerMessage::SpectatorJoined(data) => Some(data),
            _ => None,
        })
        .await;
    assert_ne!(second.stream_id, session_id);
    {
        let state = server.state.read().await;
        assert!(!state.sessions.contains_key(&session_id));
        assert!(state.sessions.contains_key(&second.stream_id));
    }

    // Stopping watching ends it.
    host.send(&ClientMessage::LeaveSpectate).await;
    let deadline = Instant::now() + Duration::from_secs(5);
    while !server.state.read().await.sessions.is_empty() {
        assert!(Instant::now() < deadline, "the watch outlived its viewer");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

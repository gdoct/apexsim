//! Hotlap mode end to end: every driver starts in the garage, frozen and
//! out of the collision passes; going out puts the car on the run-up before
//! the line, where the first lap starts timed as it crosses; two drivers
//! going out together are queued, not stacked; the trip back keeps the
//! driver's bests. Then over the wire: a relocate outside a hotlap is
//! refused, and a driver with no record gets an empty ghost.

mod common;

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::Duration;

use apexsim_server::data::*;
use apexsim_server::game_session::{GameSession, HOTLAP_RUNUP_M, HOTLAP_SPACING_M};
use apexsim_server::laps;
use apexsim_server::network::{ClientMessage, LobbyStateData, ServerMessage, LAP_FLAG_IN_GARAGE};
use apexsim_server::track_loader::TrackLoader;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};
use uuid::Uuid;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/real/Monza.yaml").expect("failed to load Monza")
}

/// A session with `humans` human drivers seated, switched straight to Hotlap.
fn hotlap_session(humans: usize) -> (GameSession, Vec<PlayerId>) {
    let track = monza();
    let car = CarConfig::default();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let ids: Vec<PlayerId> = (0..humans)
        .map(|i| Uuid::from_u128(100 + i as u128))
        .collect();
    for id in &ids {
        gs.add_player(*id, car_id).expect("seat");
    }
    gs.set_game_mode(GameMode::Hotlap);
    (gs, ids)
}

fn full_throttle() -> PlayerInputData {
    PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    }
}

#[test]
fn drivers_start_in_the_garage_frozen_and_flagged() {
    let (mut gs, ids) = hotlap_session(1);
    let driver = ids[0];
    let state = &gs.session.participants[&driver];
    assert!(state.in_garage, "hotlap starts in the garage");
    assert_eq!(state.gear, 0, "parked in neutral");
    let parked = (state.pos_x, state.pos_y, state.pos_z);

    // Full throttle for two seconds does nothing to a car in the garage.
    let inputs: HashMap<PlayerId, PlayerInputData> = [(driver, full_throttle())].into();
    for _ in 0..480 {
        gs.tick(&inputs);
    }
    let state = &gs.session.participants[&driver];
    assert_eq!((state.pos_x, state.pos_y, state.pos_z), parked);
    assert_eq!(state.speed_mps, 0.0);
    assert_eq!(state.current_lap, 0);
    assert!(!state.is_colliding);

    // The telemetry says so, in the bit an older client ignores.
    let telemetry = gs.get_compact_telemetry();
    assert_eq!(telemetry.game_mode, GameMode::Hotlap);
    assert_eq!(telemetry.session_state, SessionState::Racing);
    assert_eq!(
        telemetry.car_states[0].lap_flags & LAP_FLAG_IN_GARAGE,
        LAP_FLAG_IN_GARAGE
    );
}

#[test]
fn going_out_puts_the_car_on_the_run_up_and_the_lap_starts_at_the_line() {
    let (mut gs, ids) = hotlap_session(1);
    let driver = ids[0];
    let length = laps::track_length_m(&gs.track_config);

    gs.hotlap_relocate(&driver, HotlapDestination::Track)
        .expect("out onto the track");
    let state = &gs.session.participants[&driver];
    assert!(!state.in_garage);
    assert_eq!(state.gear, 1);
    let station = state.track_progress;
    let expected = length - HOTLAP_RUNUP_M;
    assert!(
        (station - expected).abs() < 10.0,
        "the run-up is {HOTLAP_RUNUP_M} m before the line: station {station:.0} of {length:.0}"
    );
    assert_eq!(state.current_lap, 0, "the lap has not started yet");
    assert_eq!(
        gs.get_compact_telemetry().car_states[0].lap_flags & LAP_FLAG_IN_GARAGE,
        0
    );

    // The server's own gentle driver takes it up the straight and over the
    // line (the straight is not straight enough for hands-off).
    let mut crossed_at = None;
    for tick in 0..(240 * 30) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.cooldown_input(&driver))].into();
        gs.tick(&inputs);
        let state = &gs.session.participants[&driver];
        if state.current_lap == 1 {
            crossed_at = Some((tick, state.track_progress, state.speed_mps));
            break;
        }
    }
    let (tick, station, speed) = crossed_at.expect("the car should reach the line");
    assert!(tick > 240, "300 m takes more than a second from rest");
    assert!(
        station < 50.0,
        "lap 1 starts at the line, not part way round: station {station:.0}"
    );
    assert!(
        speed > 30.0,
        "the first lap is a flying one: {speed:.0} m/s"
    );
    let state = &gs.session.participants[&driver];
    assert!(
        state.current_lap_time_ms < 200,
        "the stopwatch starts at the line"
    );
    assert!(
        !state.laps.invalid,
        "a car put on the run-up has not cut the course"
    );
}

#[test]
fn two_drivers_going_out_are_queued_not_stacked() {
    let (mut gs, ids) = hotlap_session(2);
    gs.hotlap_relocate(&ids[0], HotlapDestination::Track)
        .unwrap();
    gs.hotlap_relocate(&ids[1], HotlapDestination::Track)
        .unwrap();
    let a = &gs.session.participants[&ids[0]];
    let b = &gs.session.participants[&ids[1]];
    let gap = (a.pos_x - b.pos_x).hypot(a.pos_y - b.pos_y);
    assert!(
        (gap - HOTLAP_SPACING_M).abs() < 5.0,
        "the second car waits a slot behind: {gap:.1} m apart"
    );
    assert!(b.track_progress < a.track_progress, "behind, not ahead");

    // Neither touches the other, and the one still in the garage is not hit
    // by a car driving through its grid slot.
    let inputs: HashMap<PlayerId, PlayerInputData> =
        ids.iter().map(|id| (*id, full_throttle())).collect();
    for _ in 0..240 {
        gs.tick(&inputs);
    }
    assert!(gs.session.participants.values().all(|s| !s.is_colliding));

    // Back into the garage: the car is where it was seated, and its bests
    // came with it.
    let state = gs.session.participants.get_mut(&ids[0]).unwrap();
    state.best_lap_time_ms = Some(101_000);
    state.laps.best_lap_ms = Some(101_000);
    state.damage.front_damage_percent = 3.0;
    gs.hotlap_relocate(&ids[0], HotlapDestination::Garage)
        .unwrap();
    let state = &gs.session.participants[&ids[0]];
    assert!(state.in_garage);
    assert_eq!(state.best_lap_time_ms, Some(101_000));
    assert_eq!(state.laps.best_lap_ms, Some(101_000));
    assert_eq!(
        state.damage.front_damage_percent, 0.0,
        "the car comes back repaired"
    );
    assert_eq!(state.speed_mps, 0.0);
    let slot = &gs.track_config.start_positions[(state.grid_position - 1) as usize];
    assert_eq!((state.pos_x, state.pos_y), (slot.x, slot.y));
}

#[test]
fn relocating_is_refused_outside_a_hotlap() {
    let (mut gs, ids) = hotlap_session(1);
    gs.set_game_mode(GameMode::FreePractice);
    assert!(gs
        .hotlap_relocate(&ids[0], HotlapDestination::Track)
        .is_err());
    assert!(gs
        .hotlap_relocate(&Uuid::from_u128(999), HotlapDestination::Garage)
        .is_err());
}

// --- Over the wire ---

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

    /// The next message `pick` accepts, skipping whatever else arrives.
    async fn wait_for<T>(
        &mut self,
        pick: impl Fn(ServerMessage) -> Option<T>,
    ) -> Result<T, Box<dyn std::error::Error>> {
        for _ in 0..100 {
            if let Some(found) = pick(timeout(Duration::from_secs(5), self.recv()).await??) {
                return Ok(found);
            }
        }
        Err("message never arrived".into())
    }
}

#[tokio::test]
async fn relocate_is_refused_in_practice_and_a_new_driver_has_no_ghost() {
    let result = timeout(Duration::from_secs(30), async {
        let server = common::start_test_server().await;
        let (mut host, lobby) = Client::connect("Hotlapper", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;

        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;
        host.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 2,
            ai_count: 0,
            lap_limit: 0,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::default(),
        })
        .await?;
        host.wait_for(|m| match m {
            ServerMessage::SessionJoined(d) => Some(d),
            _ => None,
        })
        .await?;

        // Not a hotlap yet: the request is refused, not silently dropped.
        host.send(&ClientMessage::SetGameMode {
            mode: GameMode::FreePractice,
        })
        .await?;
        host.send(&ClientMessage::HotlapRelocate {
            destination: HotlapDestination::Track,
        })
        .await?;
        let (code, message) = host
            .wait_for(|m| match m {
                ServerMessage::Error { code, message } => Some((code, message)),
                _ => None,
            })
            .await?;
        assert_eq!(code, 400);
        assert!(message.contains("hotlap"), "{message}");

        // In a hotlap it goes through: the next frame shows the car out.
        host.send(&ClientMessage::SetGameMode {
            mode: GameMode::Hotlap,
        })
        .await?;
        host.wait_for(|m| match m {
            ServerMessage::GameModeChanged {
                mode: GameMode::Hotlap,
            } => Some(()),
            _ => None,
        })
        .await?;
        host.send(&ClientMessage::HotlapRelocate {
            destination: HotlapDestination::Track,
        })
        .await?;
        // Telemetry is on UDP after a handshake; over TCP a client that has
        // not handshaken gets the fallback frames. Either way the flag shows.
        let flags = host
            .wait_for(|m| match m {
                ServerMessage::TelemetryCompact(t) => t
                    .car_states
                    .first()
                    .filter(|c| c.lap_flags & LAP_FLAG_IN_GARAGE == 0)
                    .map(|c| c.lap_flags),
                _ => None,
            })
            .await?;
        assert_eq!(flags & LAP_FLAG_IN_GARAGE, 0);

        // No record on this track in this car yet: an empty ghost, at once.
        host.send(&ClientMessage::RequestGhost).await?;
        let ghost = host
            .wait_for(|m| match m {
                ServerMessage::GhostLap(g) => Some(g),
                _ => None,
            })
            .await?;
        assert_eq!(ghost.track_id, track_id);
        assert_eq!(ghost.car_config_id, car_id);
        assert_eq!(ghost.lap_time_ms, 0);
        assert_eq!(ghost.sample_count(), 0);
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;
    result.expect("timed out").expect("test failed");
}

// --- Fixture --------------------------------------------------------------------

/// Writes a record lap with a ghost trace into a record store, so a client
/// run against a fresh server has a ghost to drive: the AI takes the LMP2
/// round Monza once, sampled at the ghost rate. Ignored: it writes files.
///
/// `APEXSIM_GHOST_DIR` is the store (the server's `[records] dir`),
/// `APEXSIM_GHOST_PLAYER` the driver's name (default `Player`).
#[test]
#[ignore]
fn generate_ghost_fixture() {
    use apexsim_server::ai_driver::AiDriverProfile;
    use apexsim_server::car_loader::CarLoader;
    use apexsim_server::records::{GhostLap, GhostSample, RecordStore, GHOST_SAMPLE_HZ};

    let dir = std::env::var("APEXSIM_GHOST_DIR").expect("APEXSIM_GHOST_DIR");
    let player = std::env::var("APEXSIM_GHOST_PLAYER").unwrap_or_else(|_| "Player".to_string());
    let track = monza();
    let car = CarLoader::load_from_file(std::path::Path::new(
        "../content/cars/fugazzi-lmp2/car.toml",
    ))
    .expect("LMP2");
    let (track_id, car_id) = (track.id, car.id);
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let mut profile = AiDriverProfile::new("Fixture", 95);
    profile.id = Uuid::from_u128(7000);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track_id,
        SessionKind::Multiplayer,
        8,
        1,
        10,
    );
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);

    let mut samples: Vec<GhostSample> = Vec::new();
    let mut lap_time_ms = None;
    let mut sampling = false;
    for tick in 0..(240 * 60 * 6) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let state = &gs.session.participants[&driver];
        if state.current_lap == 2 && !sampling {
            // Lap 1 was from the grid; lap 2 is flying from the line.
            sampling = true;
            samples.clear();
        }
        if state.current_lap == 3 {
            lap_time_ms = state.last_lap_time_ms;
            break;
        }
        if sampling && tick % 12 == 0 {
            samples.push(GhostSample {
                t_ms: state.current_lap_time_ms,
                x: state.pos_x,
                y: state.pos_y,
                z: state.pos_z,
                yaw_rad: state.yaw_rad,
                pitch_rad: state.pitch_rad,
                roll_rad: state.roll_rad,
                speed_mps: state.speed_mps,
                steering: state.steering_input,
                throttle: state.throttle_input,
                brake: state.brake_input,
                gear: state.gear,
                engine_rpm: state.engine_rpm,
            });
        }
    }
    let lap_time_ms = lap_time_ms.expect("the AI should complete lap 2");
    let store = RecordStore::open(&dir);
    let record = store
        .submit(
            &player,
            track_id,
            car_id,
            lap_time_ms,
            [lap_time_ms / 3; 3],
            Some(GhostLap {
                lap_time_ms,
                sample_hz: GHOST_SAMPLE_HZ,
                samples,
            }),
        )
        .expect("a fresh store takes the lap");
    println!(
        "ghost fixture: {} ms, {} samples, {:?} in {}",
        lap_time_ms,
        store.ghost(&record).map(|g| g.samples.len()).unwrap_or(0),
        record.ghost_file,
        dir
    );
}

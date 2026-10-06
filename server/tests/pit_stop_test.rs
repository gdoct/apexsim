//! Pit stops end to end at Monza: the limiter holds a car to the lane's
//! limit, a car stopped at its box is serviced (the compound its driver
//! chose, fresh), and an AI on worn tyres plans a stop, drives the lane to
//! its box and back out, and races on.
//!
//! Driven through `GameSession` directly, like `fuel_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::CarSetup;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza.yaml").expect("Monza")
}

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads")
}

/// One human on Monza in practice; skipped without the pit sidecar.
fn practice() -> Option<(GameSession, PlayerId)> {
    let track = monza();
    track.pit_lane.as_ref()?;
    let car = gt3();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let player = Uuid::from_u128(100);
    gs.set_game_mode(GameMode::FreePractice);
    gs.add_player(player, car.id).expect("seat");
    Some((gs, player))
}

/// Put `player` on the lane at `station` along it, heading down it.
fn on_the_lane(gs: &mut GameSession, player: &PlayerId, station: f32, speed: f32) {
    let lane = gs.track_config.pit_lane.clone().expect("lane");
    let (x, y) = lane.point_at(station);
    let (x2, y2) = lane.point_at(station + 2.0);
    let yaw = (y2 - y).atan2(x2 - x);
    let state = gs.session.participants.get_mut(player).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = if speed > 20.0 { 3 } else { 1 };
    state.auto_gearbox = true;
    state.nearest_centerline_idx = None;
    physics::seed_track_progress(state, &gs.track_config);
}

#[test]
fn the_limiter_holds_a_car_to_the_lane_limit() {
    let Some((mut gs, player)) = practice() else {
        eprintln!("no pit sidecar for Monza: run ats-export");
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    // Below the limit, flat out: it climbs to the limit and holds it. (It
    // used to start 8 m/s over and pass because the lane drove as grass,
    // whose drag had it down to the limit inside three seconds; on asphalt
    // a car over the limit coasts down, the throttle shut.)
    on_the_lane(
        &mut gs,
        &player,
        lane.limit_start_m + 5.0,
        lane.speed_limit_mps - 6.0,
    );
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, flat_out)].into();
    let mut top = 0.0f32;
    for k in 0..(240 * 8) {
        gs.tick(&inputs);
        let s = &gs.session.participants[&player];
        if k > 240 * 3 && s.pit.limiter {
            top = top.max(s.speed_mps);
        }
    }
    println!(
        "flat out between the lines: {:.1} km/h against {:.0}",
        top * 3.6,
        lane.speed_limit_mps * 3.6
    );
    assert!(top > lane.speed_limit_mps - 2.0, "it drives at the limit");
    assert!(top < lane.speed_limit_mps + 0.5, "and no faster");
}

#[test]
fn a_car_stopped_at_its_box_gets_the_set_its_driver_chose() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    gs.set_car_setup(
        &player,
        CarSetup {
            tyre_compound: 1,
            ..Default::default()
        },
    );
    // The only car in the session: box 0.
    let spot = *lane.box_at(0);
    {
        let state = gs.session.participants.get_mut(&player).unwrap();
        state.pos_x = spot.x;
        state.pos_y = spot.y;
        state.yaw_rad = spot.yaw_rad;
        state.tires.front_left.wear_percent = 60.0;
        state.damage.front_damage_percent = 30.0;
    }
    let held = PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, held)].into();
    gs.tick(&inputs);
    let s = &gs.session.participants[&player];
    assert!(s.pit.servicing, "stopped at its box, the crew start work");
    let seconds = s.pit.service_left_s;
    assert!(
        seconds > apexsim_server::pit::TYRE_CHANGE_S,
        "the repairs take time too: {seconds}"
    );
    for _ in 0..((seconds + 0.5) * 240.0) as usize {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&player];
    assert!(!s.pit.servicing && s.pit.serviced && s.pit.stops == 1);
    assert_eq!(s.tyre_compound, 0, "the softs the setup asked for");
    assert_eq!(s.tires.front_left.wear_percent, 0.0);
    assert_eq!(s.damage.front_damage_percent, 0.0);
}

#[test]
fn an_ai_on_worn_tyres_pits_and_races_on() {
    let track = monza();
    let Some(lane) = track.pit_lane.clone() else {
        return;
    };
    let car = gt3();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let mut profile = AiDriverProfile::new("Pit AI", 95);
    profile.id = Uuid::from_u128(7300);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        4,
    );
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    {
        let s = gs.session.participants.get_mut(&driver).unwrap();
        for t in s.tires.each_mut() {
            t.wear_percent = 72.0;
        }
    }

    let (mut laps, mut off_ticks, mut in_box_ticks) = (Vec::new(), 0u32, 0u32);
    let mut stopped_at = None;
    for _ in 0..(240 * 60 * 10) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&driver];
        off_ticks += (!s.is_on_track && !s.pit.in_lane) as u32;
        if s.pit.servicing {
            in_box_ticks += 1;
            stopped_at.get_or_insert((s.pos_x, s.pos_y));
        }
        for out in gs.take_lap_events() {
            if let Some(ms) = out.event.lap_time_ms {
                laps.push(ms as f32 / 1000.0);
            }
        }
        if laps.len() >= 3 {
            break;
        }
    }
    let s = &gs.session.participants[&driver];
    println!(
        "laps {laps:?}; stops {}, {:.1} s in the box on {} ; {:.1} s off the road",
        s.pit.stops,
        in_box_ticks as f32 / 240.0,
        apexsim_server::tyre_thermal::COMPOUNDS[s.tyre_compound as usize].name,
        off_ticks as f32 / 240.0
    );
    assert_eq!(laps.len(), 3, "it races on");
    assert_eq!(s.pit.stops, 1, "one stop");
    let (x, y) = stopped_at.expect("it stopped");
    let spot = lane.box_at(s.pit.box_index.expect("dealt a box"));
    assert!(
        ((x - spot.x).powi(2) + (y - spot.y).powi(2)).sqrt() <= apexsim_server::pit::BOX_RADIUS_M
    );
    assert!(
        s.tires.each().iter().all(|t| t.wear_percent < 20.0),
        "fresh tyres"
    );
    assert!(
        off_ticks < 240 * 3,
        "{:.1} s off the road",
        off_ticks as f32 / 240.0
    );
    // The stop lap costs the lane and the service, not a disaster.
    let clean = laps[2];
    let stop_lap = laps[0].max(laps[1]);
    assert!(
        stop_lap > clean + 10.0 && stop_lap < clean + 60.0,
        "{laps:?}"
    );
}

/// What became of one human's trip down a pit lane under the autopilot.
#[derive(Debug, Default)]
struct Trip {
    took_over: bool,
    /// Seconds from the takeover to the box, in the box, and from the box
    /// to the lane's end (handed back).
    to_box_s: Option<f32>,
    service_s: f32,
    handed_back_s: Option<f32>,
    /// Furthest the car strayed from the lane's middle outside the box swing, m.
    worst_off_m: f32,
    /// Seconds the car crawled (< 1 m/s) away from its box and the light.
    stuck_s: f32,
    /// Times the autopilot had to put the car back on its route.
    recoveries: u32,
    /// Where the worst deviation was: lane station, and whether on the
    /// way out.
    worst_at: (f32, bool),
}

/// Seat one human on `track` in practice, 80 m short of the pit lane on
/// the track, drive into the lane's mouth, then hold full throttle and
/// left lock (which the autopilot must ignore) until the lane hands the car
/// back or `limit_s` passes.
fn drive_a_trip(track: TrackConfig, car: CarConfig, limit_s: f32) -> Option<Trip> {
    let lane = track.pit_lane.clone()?;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let player = Uuid::from_u128(100);
    gs.set_game_mode(GameMode::FreePractice);
    gs.add_player(player, car.id)?;
    // On the track, 80 m short of where the lane leaves it, at 17 m/s.
    let total = gs.track_length_m();
    let start = (lane.entry_station_m - 80.0).rem_euclid(total);
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, start);
    {
        let s = gs.session.participants.get_mut(&player)?;
        s.pos_x = x;
        s.pos_y = y;
        s.pos_z = z;
        s.yaw_rad = yaw;
        s.vel_x = 17.0 * yaw.cos();
        s.vel_y = 17.0 * yaw.sin();
        s.speed_mps = 17.0;
        s.gear = 3;
        s.auto_gearbox = false;
        s.steering_assist = true;
        s.nearest_centerline_idx = None;
        s.tires.front_left.wear_percent = 40.0;
        s.damage.left_damage_percent = 10.0;
        physics::seed_track_progress(s, &gs.track_config);
    }
    let mut trip = Trip::default();
    let dt = 1.0 / 240.0;
    let mut t = 0.0f32;
    let mut served = false;
    let trace = std::env::var("PIT_TRACE").is_ok();
    let mut last = (x, y);
    // Off the lane's middle counts once the car has first come onto it.
    let mut settled = false;
    while t < limit_s {
        // The driver: until the autopilot has the car, follows the track
        // at 17 m/s and turns into the lane's mouth;
        // after that, full throttle and left lock, which must be ignored.
        let input = {
            let s = &gs.session.participants[&player];
            if s.pit.driving || trip.took_over {
                PlayerInputData {
                    throttle: 1.0,
                    steering: 1.0,
                    ..Default::default()
                }
            } else {
                // Along the track until the lane's mouth is close, then
                // into it.
                let at = lane.locate(s.pos_x, s.pos_y, None);
                let (mx, my) = lane.point_at(0.0);
                let (tx, ty) = if (s.pos_x - mx).hypot(s.pos_y - my) > 25.0 && at.station_m < 1.0 {
                    let ahead = (s.track_progress + 12.0).rem_euclid(total);
                    let (x, y, _, _) = physics::pose_at_station(&gs.track_config.centerline, ahead);
                    (x, y)
                } else {
                    lane.point_at(at.station_m + 15.0)
                };
                let (c, sn) = (s.yaw_rad.cos(), s.yaw_rad.sin());
                let (dx, dy) = (tx - s.pos_x, ty - s.pos_y);
                let alpha = (-dx * sn + dy * c).atan2(dx * c + dy * sn);
                let error = 17.0 - s.speed_mps;
                PlayerInputData {
                    throttle: (error * 0.2).clamp(0.0, 1.0),
                    brake: (-error * 0.2).clamp(0.0, 1.0),
                    steering: (alpha * 2.0).clamp(-1.0, 1.0),
                    gear: Some(3),
                    ..Default::default()
                }
            }
        };
        let inputs: HashMap<PlayerId, PlayerInputData> = [(player, input)].into();
        gs.tick(&inputs);
        t += dt;
        let s = &gs.session.participants[&player];
        if (s.pos_x - last.0).hypot(s.pos_y - last.1) > 1.5 {
            trip.recoveries += 1;
        }
        last = (s.pos_x, s.pos_y);
        if s.pit.driving {
            trip.took_over = true;
        }
        if !trip.took_over && t > 20.0 {
            break;
        }
        if s.pit.servicing {
            served = true;
            trip.to_box_s.get_or_insert(t);
            trip.service_s += dt;
        }
        let pit_box = lane.box_at(s.pit.box_index.unwrap_or(0));
        let (_, lateral) = lane.lateral_of(s.pos_x, s.pos_y, s.pit.lane_node);
        let near_box = (s.pit.lane_station_m - pit_box.lane_station_m).abs() < 25.0;
        if s.pit.driving && lateral.abs() < 1.5 {
            settled = true;
        }
        if settled && s.pit.driving && !near_box && lateral.abs() > trip.worst_off_m {
            trip.worst_off_m = lateral.abs();
            trip.worst_at = (s.pit.lane_station_m, s.pit.serviced);
        }
        if trip.took_over
            && s.pit.driving
            && !s.pit.servicing
            && !s.pit.held
            && s.speed_mps < 1.0
            && !near_box
        {
            trip.stuck_s += dt;
        }
        if trace && (t * 4.0).fract() < 4.0 * dt {
            println!(
                "  t {t:6.2} s {:7.1} lat {lateral:6.2} v {:5.1} yaw {:6.2} box {:5.1} drive {} svc {} done {} held {} pos ({:.1},{:.1}) prog {:.1} lane_h {:.2} r {:.2} steer_in {:.2}",
                s.pit.lane_station_m, s.speed_mps, s.yaw_rad, pit_box.lane_station_m,
                s.pit.driving, s.pit.servicing, s.pit.serviced, s.pit.held, s.pos_x, s.pos_y, s.track_progress,
                lane.frame_at(s.pit.lane_station_m).2, s.angular_vel_yaw, s.steering_input
            );
        }
        if served && trip.took_over && !s.pit.driving {
            trip.handed_back_s = Some(t);
            break;
        }
    }
    Some(trip)
}

#[test]
fn a_human_driving_into_the_pit_lane_is_driven_to_their_box_and_out() {
    let track = monza();
    if track.pit_lane.is_none() {
        return;
    }
    let trip = drive_a_trip(track, gt3(), 120.0).expect("a lane");
    println!("{trip:?}");
    assert!(trip.took_over, "the autopilot takes the car");
    let to_box = trip.to_box_s.expect("it reaches its box");
    assert!(to_box < 40.0, "{to_box:.1} s to the box");
    assert!(
        trip.service_s > apexsim_server::pit::TYRE_CHANGE_S,
        "tyres and repairs: {:.1} s",
        trip.service_s
    );
    assert!(
        trip.handed_back_s.is_some(),
        "handed back at the lane's end"
    );
    assert!(
        trip.worst_off_m < 2.5,
        "{:.1} m off the lane's middle",
        trip.worst_off_m
    );
    assert!(trip.stuck_s < 1.0, "{:.1} s stuck", trip.stuck_s);
    assert_eq!(trip.recoveries, 0, "never put back on its route");
}

#[test]
fn the_autopilot_puts_the_drivers_aids_back() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    on_the_lane(&mut gs, &player, lane.limit_start_m - 12.0, 12.0);
    {
        let s = gs.session.participants.get_mut(&player).unwrap();
        s.auto_gearbox = false;
        s.steering_assist = true;
    }
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, PlayerInputData::default())].into();
    // Placed against the road by its first physics step, taken the next.
    for _ in 0..3 {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&player];
    assert!(
        s.pit.driving && s.auto_gearbox && !s.steering_assist,
        "the autopilot's aids"
    );
    for _ in 0..(240 * 120) {
        gs.tick(&inputs);
        if !gs.session.participants[&player].pit.driving {
            break;
        }
    }
    let s = &gs.session.participants[&player];
    assert!(
        !s.pit.driving && s.pit.stops == 1,
        "out of the lane, one stop"
    );
    assert!(
        !s.auto_gearbox && s.steering_assist,
        "the driver's own aids again"
    );
}

/// Every circuit's lane under the autopilot: a GT3 driven in, to its box,
/// serviced and out. Prints a line per circuit; fails on any that does not
/// make the whole trip.
///
/// `cargo test --release --test pit_stop_test pit_lane_survey -- --ignored --nocapture`
/// (`PIT_TRACKS=Monza,Spa` for a few).
#[test]
#[ignore]
fn pit_lane_survey() {
    let only: Option<Vec<String>> = std::env::var("PIT_TRACKS")
        .ok()
        .map(|v| v.split(',').map(|s| s.trim().to_string()).collect());
    let mut failed = Vec::new();
    let mut stems: Vec<String> = std::fs::read_dir("../content/tracks/default")
        .expect("tracks")
        .filter_map(|e| {
            let name = e.ok()?.file_name().to_string_lossy().into_owned();
            name.strip_suffix(".pit.msgpack").map(str::to_string)
        })
        .collect();
    stems.sort();
    for stem in stems {
        if only.as_ref().is_some_and(|o| !o.contains(&stem)) {
            continue;
        }
        let Ok(track) =
            TrackLoader::load_from_file(format!("../content/tracks/default/{stem}.yaml"))
        else {
            continue;
        };
        let boxes = track.pit_lane.as_ref().map_or(0, |l| l.boxes.len());
        // The car stays on the lane: its middle no closer than 1.5 m to
        // either edge (a car is about 2 m wide).
        let within = track
            .pit_lane
            .as_ref()
            .map_or(0.0, |l| l.width_m / 2.0 - 1.5);
        let Some(trip) = drive_a_trip(track, gt3(), 150.0) else {
            continue;
        };
        let ok = trip.took_over
            && trip.to_box_s.is_some()
            && trip.handed_back_s.is_some()
            && trip.worst_off_m < within
            && trip.stuck_s < 2.0
            && trip.recoveries == 0;
        println!(
            "{stem:14} {} boxes {boxes:2}  took over {}  box {:>6}  service {:4.1} s  out {:>6}  off {:4.1} m at {:5.0} {}  stuck {:4.1} s  recovered {}",
            if ok { "ok  " } else { "FAIL" },
            trip.took_over,
            trip.to_box_s.map_or("-".into(), |t| format!("{t:.1}")),
            trip.service_s,
            trip.handed_back_s.map_or("-".into(), |t| format!("{t:.1}")),
            trip.worst_off_m,
            trip.worst_at.0,
            if trip.worst_at.1 { "out" } else { "in " },
            trip.stuck_s,
            trip.recoveries,
        );
        if !ok {
            failed.push(stem);
        }
    }
    assert!(
        failed.is_empty(),
        "lanes the autopilot cannot drive: {failed:?}"
    );
}

/// One AI in a race on `track` with its tyres 72% worn: the laps it ran,
/// the stops it made and the seconds it spent off the road outside the
/// lane, over at most `laps` laps or ten minutes.
fn ai_stop_on(track: TrackConfig, laps: usize) -> (Vec<f32>, u16, f32) {
    let car = gt3();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let mut profile = AiDriverProfile::new("Pit AI", 95);
    profile.id = Uuid::from_u128(7300);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        4,
    );
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    {
        let s = gs.session.participants.get_mut(&driver).unwrap();
        for t in s.tires.each_mut() {
            t.wear_percent = 72.0;
        }
    }
    let (mut times, mut off_ticks) = (Vec::new(), 0u32);
    let trace = std::env::var("PIT_TRACE").is_ok();
    for _ in 0..(240 * 60 * 20) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&driver];
        off_ticks += (!s.is_on_track && !s.pit.in_lane) as u32;
        if trace && gs.session.current_tick.is_multiple_of(120) && (s.pit.driving || !s.is_on_track)
        {
            println!(
                "  t {:6.1} prog {:7.1} lane {:6.1} in_lane {} driving {} wants {} served {} v {:5.1} on_track {} pos ({:.1},{:.1})",
                gs.session.current_tick as f32 / 240.0, s.track_progress, s.pit.lane_station_m,
                s.pit.in_lane, s.pit.driving, s.pit.wants_stop, s.pit.serviced, s.speed_mps,
                s.is_on_track, s.pos_x, s.pos_y
            );
        }
        for out in gs.take_lap_events() {
            if let Some(ms) = out.event.lap_time_ms {
                times.push(ms as f32 / 1000.0);
            }
        }
        if times.len() >= laps {
            break;
        }
    }
    let stops = gs.session.participants[&driver].pit.stops;
    (times, stops, off_ticks as f32 / 240.0)
}

/// Every circuit's lane in a race: an AI on worn tyres plans a stop,
/// drives in, is serviced at its box and races on.
///
/// `cargo test --release --test pit_stop_test ai_pit_survey -- --ignored --nocapture`
/// (`PIT_TRACKS=Monza,Spa` for a few).
#[test]
#[ignore]
fn ai_pit_survey() {
    let only: Option<Vec<String>> = std::env::var("PIT_TRACKS")
        .ok()
        .map(|v| v.split(',').map(|s| s.trim().to_string()).collect());
    let mut stems: Vec<String> = std::fs::read_dir("../content/tracks/default")
        .expect("tracks")
        .filter_map(|e| {
            let name = e.ok()?.file_name().to_string_lossy().into_owned();
            name.strip_suffix(".pit.msgpack").map(str::to_string)
        })
        .collect();
    stems.sort();
    let mut failed = Vec::new();
    for stem in stems {
        if only.as_ref().is_some_and(|o| !o.contains(&stem)) {
            continue;
        }
        let Ok(track) =
            TrackLoader::load_from_file(format!("../content/tracks/default/{stem}.yaml"))
        else {
            continue;
        };
        let (laps, stops, off_s) = ai_stop_on(track, 3);
        let ok = laps.len() == 3 && stops == 1;
        println!(
            "{stem:14} {} stops {stops}  laps {laps:?}  off the road {off_s:.1} s",
            if ok { "ok  " } else { "FAIL" }
        );
        if !ok {
            failed.push(stem);
        }
    }
    assert!(failed.is_empty(), "no stop: {failed:?}");
}

#[test]
fn every_car_in_a_race_has_its_own_box() {
    let track = monza();
    let Some(lane) = track.pit_lane.clone() else {
        return;
    };
    let car = gt3();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let profiles: Vec<AiDriverProfile> = (0..12)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("AI {i}"), 90);
            p.id = Uuid::from_u128(8000 + i);
            p.preferred_car_id = Some(car_id);
            p
        })
        .collect();
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        16,
        12,
        3,
    );
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, profiles);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    for _ in 0..(240 * 8) {
        let inputs: HashMap<PlayerId, PlayerInputData> = gs
            .session
            .ai_player_ids
            .clone()
            .into_iter()
            .map(|id| (id, gs.generate_ai_input(&id)))
            .collect();
        gs.tick(&inputs);
    }
    let mut boxes: Vec<u8> = gs
        .session
        .participants
        .values()
        .map(|s| s.pit.box_index.expect("dealt a box"))
        .collect();
    let n = boxes.len();
    boxes.sort_unstable();
    boxes.dedup();
    assert_eq!(boxes.len(), n, "no two cars share a box");
    assert!(lane.boxes.len() >= n, "the lane has a box for each");
}

#[test]
fn a_car_leaving_its_box_waits_at_a_red_exit_light() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    // A second car, on the track, about to pass where the lane rejoins it.
    let other = Uuid::from_u128(101);
    let car_id = gs.session.participants[&player].car_config_id;
    gs.add_player(other, car_id).expect("seat");
    on_the_lane(&mut gs, &player, lane.limit_end_m - 40.0, 8.0);
    {
        let s = gs.session.participants.get_mut(&player).unwrap();
        s.pit.driving = true;
        s.pit.serviced = true;
        s.auto_gearbox = true;
    }
    let total = gs.track_length_m();
    let hold_other = |gs: &mut GameSession| {
        let at = (lane.exit_station_m - 30.0).rem_euclid(total);
        let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, at);
        let s = gs.session.participants.get_mut(&other).unwrap();
        s.pos_x = x;
        s.pos_y = y;
        s.pos_z = z;
        s.yaw_rad = yaw;
        s.vel_x = 30.0 * yaw.cos();
        s.vel_y = 30.0 * yaw.sin();
        s.speed_mps = 30.0;
        // In a gear that turns 30 m/s, or the engine cooks.
        s.gear = 4;
        s.nearest_centerline_idx = None;
        physics::seed_track_progress(s, &gs.track_config);
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();
    let mut held = false;
    for tick in 0..(240 * 6) {
        hold_other(&mut gs);
        gs.tick(&inputs);
        let s = &gs.session.participants[&player];
        let o = &gs.session.participants[&other];
        assert!(
            s.pit.exit_closed,
            "the light is red while the car comes: tick {tick} other at {} v {} (garage {}, driving {}, drivable {}), exit {} of {total}",
            o.track_progress, o.speed_mps, o.in_garage, o.pit.driving, o.damage.is_drivable, lane.exit_station_m
        );
        held |= s.pit.held;
        assert!(
            s.pit.lane_station_m < lane.limit_end_m,
            "it waits short of the light"
        );
    }
    assert!(held, "held at the light");
    // The traffic passes: green, and it goes.
    for _ in 0..(240 * 20) {
        gs.tick(&inputs);
        if !gs.session.participants[&player].pit.driving {
            break;
        }
    }
    let s = &gs.session.participants[&player];
    assert!(!s.pit.driving, "out of the lane once the light is green");
}

/// The lane drives as the lane on the centerline backend too: the offline
/// tools (the guide, showcase renders, the AI survey, the surveys below)
/// load tracks without the road mesh, which is the only thing that drew
/// the lane as a surface, and every stop they drove was on the grass
/// (grass grip and drag: a GT3 left its box at Le Mans at 1.6 m/s² and
/// cooked its engine doing it). Every box and every 10 m between the limit
/// lines is pit lane on every circuit with a sidecar.
#[test]
fn the_pit_lane_is_a_pit_lane_without_the_road_mesh() {
    let mut stems: Vec<String> = std::fs::read_dir("../content/tracks/default")
        .expect("tracks")
        .filter_map(|e| {
            let name = e.ok()?.file_name().to_string_lossy().into_owned();
            name.strip_suffix(".pit.msgpack").map(str::to_string)
        })
        .collect();
    stems.sort();
    let mut bad = Vec::new();
    for stem in &stems {
        let Ok(track) =
            TrackLoader::load_from_file(format!("../content/tracks/default/{stem}.yaml"))
        else {
            continue;
        };
        let Some(lane) = track.pit_lane.as_ref() else {
            continue;
        };
        let mut points: Vec<(f32, f32)> = lane.boxes.iter().map(|b| (b.x, b.y)).collect();
        let mut station = lane.limit_start_m;
        while station <= lane.limit_end_m {
            points.push(lane.point_at(station));
            station += 10.0;
        }
        let off = points
            .iter()
            .filter(|(x, y)| {
                let contact = physics::probe_surface(&track, *x, *y, 0.0, None).map(|p| p.contact);
                !matches!(
                    contact,
                    Some(physics::RoadContact::PitLane | physics::RoadContact::Road)
                )
            })
            .count();
        if off > 0 {
            bad.push(format!("{stem}: {off} of {} points", points.len()));
        }
    }
    assert!(bad.is_empty(), "pit lane read as something else: {bad:?}");
}

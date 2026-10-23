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

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file(
        "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    )
    .expect("Monzarella")
}

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new(
        "../../../apexsim-content/cars/default/posh-gt3rs/car.toml",
    ))
    .expect("car loads")
}

/// One human on Monza in practice; `None` (the test skipped, and said so)
/// without the pit sidecar.
fn practice() -> Option<(GameSession, PlayerId)> {
    let track = monza();
    if !track.has_sidecars(&[Sidecar::Pit], "pit_stop_test") {
        return None;
    }
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
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    // Below the limit, flat out: it climbs to the limit and holds it. (It
    // used to start 8 m/s over and pass because the lane drove as grass,
    // whose drag had it down to the limit inside three seconds; on asphalt
    // a car over the limit coasts down, the throttle shut.)
    // Past the last box, so no box and no autopilot stands in its way:
    // the entry line is a short lead before the first box, and a car let go
    // just past it is taken to its box.
    let past_the_boxes = lane
        .boxes
        .last()
        .map_or(lane.limit_start_m, |b| b.lane_station_m)
        + 10.0;
    assert!(
        lane.limit_end_m - past_the_boxes > 150.0,
        "room to the exit line"
    );
    on_the_lane(&mut gs, &player, past_the_boxes, lane.speed_limit_mps - 6.0);
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, flat_out)].into();
    let mut top = 0.0f32;
    for k in 0..(HZ * 8) {
        gs.tick(&inputs);
        let s = &gs.session.participants[&player];
        if k > HZ * 3 && s.pit.limiter {
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
    for _ in 0..((seconds + 0.5) * HZ as f32) as usize {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&player];
    assert!(!s.pit.servicing && s.pit.serviced && s.pit.stops == 1);
    assert_eq!(s.tyre_compound, 0, "the softs the setup asked for");
    assert_eq!(s.tires.front_left.wear_percent, 0.0);
    assert_eq!(s.damage.front_damage_percent, 0.0);
}

/// Stop `player` at box 0 with worn tyres, 30% damage, `brake_wear` on the
/// pads and 20 L in the tank, and hold them there until the crew are done.
/// Returns the service's seconds and the PitService the crew announced.
fn serviced_at_box(
    gs: &mut GameSession,
    player: &PlayerId,
    brake_wear: f32,
) -> (f32, apexsim_server::game_session::PitService) {
    let lane = gs.track_config.pit_lane.clone().unwrap();
    let spot = *lane.box_at(0);
    {
        let state = gs.session.participants.get_mut(player).unwrap();
        state.pos_x = spot.x;
        state.pos_y = spot.y;
        state.yaw_rad = spot.yaw_rad;
        state.tires.front_left.wear_percent = 60.0;
        state.damage.front_damage_percent = 30.0;
        state.brake_wear_pct = [brake_wear; 4];
        state.fuel_liters = 20.0;
    }
    let held = PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(*player, held)].into();
    gs.tick(&inputs);
    let seconds = gs.session.participants[player].pit.service_left_s;
    assert!(gs.session.participants[player].pit.servicing);
    let (_, service) = gs
        .take_pit_events()
        .into_iter()
        .find(|(id, _)| id == player)
        .expect("the crew announce the stop");
    for _ in 0..((seconds + 0.5) * HZ as f32) as usize {
        gs.tick(&inputs);
    }
    (seconds, service)
}

#[test]
fn a_drivers_pit_plan_is_what_the_crew_do() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    // No tyres, 5 L, no repairs, the pads left on; and a request to box.
    let plan = apexsim_server::pit::PitPlan {
        keep_tyres: true,
        fuel_l: Some(5.0),
        no_repair: true,
        pads: Some(false),
        box_this_lap: true,
        ..Default::default()
    };
    gs.set_pit_plan(&player, plan).expect("seated");
    let flags = apexsim_server::network::pit_flags_of(&gs.session.participants[&player]);
    assert_ne!(
        flags & apexsim_server::network::PIT_FLAG_REQUESTED,
        0,
        "the request comes back in telemetry"
    );
    let compound_before = gs.session.participants[&player].tyre_compound;
    let (seconds, service) = serviced_at_box(&mut gs, &player, 70.0);
    println!("plan stop: {seconds:.2} s, {service:?}");
    assert!(
        (seconds - 5.0 / apexsim_server::pit::REFUEL_LPS).abs() < 0.05,
        "the fuel alone takes time: {seconds}"
    );
    assert_eq!((service.tyres_s, service.repair_s), (0.0, 0.0));
    assert!((service.fuel_l - 5.0).abs() < 1e-3);
    let s = &gs.session.participants[&player];
    assert!(s.pit.serviced && s.pit.stops == 1);
    assert!(
        (s.fuel_liters - 25.0).abs() < 0.05,
        "5 L in: {}",
        s.fuel_liters
    );
    assert_eq!(s.tyre_compound, compound_before);
    assert!(
        s.tires.front_left.wear_percent >= 60.0,
        "the old set stays on"
    );
    assert!(s.damage.front_damage_percent >= 30.0, "nothing repaired");
    assert!(s.brake_wear_pct[0] >= 70.0, "the old pads stay on");
    assert!(!s.pit.requested, "the request is answered by the stop");
    assert_eq!(
        apexsim_server::network::pit_flags_of(s) & apexsim_server::network::PIT_FLAG_REQUESTED,
        0
    );
}

#[test]
fn a_pit_plan_can_fit_new_pads_of_another_set() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let plan = apexsim_server::pit::PitPlan {
        keep_tyres: true,
        fuel_l: Some(0.0),
        no_repair: true,
        pads: Some(true),
        pad_set: Some(1),
        ..Default::default()
    };
    gs.set_pit_plan(&player, plan).expect("seated");
    // Barely worn pads: the crew would leave them, the driver asks for sprint pads.
    let (seconds, _) = serviced_at_box(&mut gs, &player, 10.0);
    assert!(
        (seconds - apexsim_server::brakes::PAD_CHANGE_S).abs() < 0.05,
        "the pads alone: {seconds}"
    );
    let s = &gs.session.participants[&player];
    assert_eq!(s.brake_wear_pct, [0.0; 4], "new pads");
    assert_eq!(
        gs.car_setup(&player).brake_pads,
        1,
        "the setup's pads follow"
    );
    assert_eq!(
        gs.simulated_config_for(&player).unwrap().brake_pads,
        apexsim_server::brakes::BrakePads::Sprint
    );
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
    for _ in 0..(HZ * 60 * 10) {
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
        in_box_ticks as f32 / HZ as f32,
        apexsim_server::tyre_thermal::compound(s.tyre_compound).name,
        off_ticks as f32 / HZ as f32
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
        off_ticks < HZ as u32 * 3,
        "{:.1} s off the road",
        off_ticks as f32 / HZ as f32
    );
    // The stop lap costs the lane and the service, not a disaster.
    let clean = laps[2];
    let stop_lap = laps[0].max(laps[1]);
    assert!(
        stop_lap > clean + 10.0 && stop_lap < clean + 60.0,
        "{laps:?}"
    );
}

/// Late in a race, short of fuel on tyres that reach the flag: a splash of
/// fuel and back out on the same set, quicker than a full stop.
#[test]
fn an_ai_short_of_fuel_late_on_splashes_and_keeps_its_tyres() {
    let track = monza();
    if !track.has_sidecars(
        &[Sidecar::Pit],
        "an_ai_short_of_fuel_late_on_splashes_and_keeps_its_tyres",
    ) {
        return;
    }
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
    let lap = gs.lap_fuel_liters(car_id).expect("a lap estimate");
    {
        let s = gs.session.participants.get_mut(&driver).unwrap();
        // Ten laps on this set, 20% worn: they last. Fuel for 1.4 laps.
        s.pit.stint_start_laps = -10.0;
        for t in s.tires.each_mut() {
            t.wear_percent = 20.0;
        }
        s.fuel_liters = 1.4 * lap;
    }
    let (mut laps, mut in_box_ticks) = (Vec::new(), 0u32);
    let mut fuel_only = false;
    for _ in 0..(HZ * 60 * 10) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&driver];
        fuel_only |= s.pit.wants_stop && s.pit.fuel_only;
        in_box_ticks += s.pit.servicing as u32;
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
    let in_box = in_box_ticks as f32 / HZ as f32;
    println!(
        "laps {laps:?}; stops {}, {in_box:.1} s in the box, fuel {:.1} L",
        s.pit.stops, s.fuel_liters
    );
    assert!(fuel_only, "planned as a splash");
    assert_eq!(s.pit.stops, 1, "one stop");
    assert!(
        s.tires.each().iter().all(|t| t.wear_percent >= 20.0),
        "the same tyres"
    );
    assert!(
        in_box < apexsim_server::pit::TYRE_CHANGE_S,
        "fuel alone: {in_box:.1} s"
    );
    assert!(s.fuel_liters > 0.0, "it reaches the flag");
}

/// A driver who lost their connection mid-race: the server drives their car
/// (`away_input`) and plans its stops as an AI's, so worn tyres take it to
/// its box rather than round and round until they or the tank give out.
#[test]
fn an_away_drivers_car_is_brought_in_for_its_stop() {
    let track = monza();
    if !track.has_sidecars(
        &[Sidecar::Pit],
        "an_away_drivers_car_is_brought_in_for_its_stop",
    ) {
        return;
    }
    let car = gt3();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        0,
        4,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let player = Uuid::from_u128(100);
    gs.add_player(player, car_id).expect("seat");
    gs.start_countdown_mode(1, GameMode::Race);
    for t in gs
        .session
        .participants
        .get_mut(&player)
        .unwrap()
        .tires
        .each_mut()
    {
        t.wear_percent = 72.0;
    }
    assert!(gs.hold_seat(&player), "the seat is held");

    for _ in 0..(HZ * 60 * 8) {
        let inputs: HashMap<PlayerId, PlayerInputData> = [(player, gs.away_input(&player))].into();
        gs.tick(&inputs);
        if gs.session.participants[&player].pit.stops > 0 {
            break;
        }
    }
    let s = &gs.session.participants[&player];
    assert_eq!(s.pit.stops, 1, "the server pitted the away car");
    assert!(
        s.tires.each().iter().all(|t| t.wear_percent < 20.0),
        "fresh tyres"
    );
}

/// A car the server drives for its driver (away, or a debug stand-in) on a
/// setup that asks for the wet, in the dry: the planner brings it in for
/// slicks, and the crew fit the slicks it was brought in for, not the
/// setup's wet, so it does not come back in every lap to change off them.
#[test]
fn a_server_planned_stop_fits_the_compound_it_was_planned_for() {
    let track = monza();
    if !track.has_sidecars(
        &[Sidecar::Pit],
        "a_server_planned_stop_fits_the_compound_it_was_planned_for",
    ) {
        return;
    }
    let car = CarLoader::load_from_file(Path::new(
        "../../../apexsim-content/cars/default/limbotiti-caravan-gt3/car.toml",
    ))
    .expect("car loads");
    let car_id = car.id;
    let wet = car
        .tire_config
        .compounds()
        .iter()
        .position(|c| c.name == "wet")
        .expect("a wet") as u8;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        0,
        6,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let player = Uuid::from_u128(100);
    gs.add_player(player, car_id).expect("seat");
    // One click from the reference slick: the wet.
    gs.set_car_setup(
        &player,
        CarSetup {
            tyre_compound: -1,
            ..Default::default()
        },
    );
    gs.start_countdown_mode(1, GameMode::Race);
    assert_eq!(
        gs.session.participants[&player].tyre_compound, wet,
        "starts on the setup's wet"
    );
    assert!(gs.hold_seat(&player), "the seat is held");

    let mut first_stop_at = None;
    for tick in 0..(HZ * 60 * 6) {
        let inputs: HashMap<PlayerId, PlayerInputData> = [(player, gs.away_input(&player))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&player];
        if first_stop_at.is_none() && s.pit.stops == 1 {
            first_stop_at = Some(tick);
        }
        if first_stop_at.is_some_and(|t| tick > t + HZ * 150) {
            break;
        }
    }
    let s = &gs.session.participants[&player];
    assert!(first_stop_at.is_some(), "brought in for slicks");
    assert_ne!(s.tyre_compound, wet, "on the slicks it was brought in for");
    assert_eq!(s.pit.stops, 1, "and not brought in again");
}

/// A human's car is handed back no slower than this under what the lane
/// allows there (its limit, or its bend), m/s: the autopilot does not slow
/// for the exit taper past the line.
const HANDBACK_SLACK_MPS: f32 = 1.5;

/// What became of one human's trip down a pit lane under the autopilot.
#[derive(Debug, Default)]
struct Trip {
    took_over: bool,
    /// Seconds from the takeover to the box, in the box, and from the box
    /// to the lane's end (handed back).
    to_box_s: Option<f32>,
    service_s: f32,
    handed_back_s: Option<f32>,
    /// Speed at the hand-back, and the slowest the car went over the 40 m
    /// of lane before it (past the box), m/s.
    handback_mps: f32,
    exit_min_mps: f32,
    /// What the lane lets the car do at the hand-back: its limit, or less
    /// where it bends there, m/s.
    handback_cap_mps: f32,
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
    let dt = 1.0 / HZ as f32;
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
                // A driver slows for the lane's bends ahead of the car (an
                // imported lane can peel off through a chicane: Spa's bus
                // stop), braking at about 4 m/s².
                let pace = (0..=25)
                    .map(|ahead| {
                        let ahead = 2.0 * ahead as f32;
                        // (Its bend is read clear of the lane's ends.)
                        let bend = apexsim_server::pit::lane_bend_mps(
                            &lane,
                            (at.station_m + ahead).max(4.0),
                        );
                        (bend * bend + 2.0 * 4.0 * ahead).sqrt()
                    })
                    .fold(17.0f32, f32::min);
                let error = pace - s.speed_mps;
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
        // Time to drive up to the entry line from the lane's mouth (an
        // imported lane's mouth is a long way before it).
        if !trip.took_over && t > 20.0 + lane.limit_start_m / 10.0 {
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
        if s.pit.driving
            && s.pit.serviced
            && !s.pit.servicing
            && s.pit.lane_station_m > lane.limit_end_m - 40.0
        {
            trip.exit_min_mps = if trip.exit_min_mps > 0.0 {
                trip.exit_min_mps.min(s.speed_mps)
            } else {
                s.speed_mps
            };
        }
        if served && trip.took_over && !s.pit.driving {
            trip.handed_back_s = Some(t);
            trip.handback_mps = s.speed_mps;
            // A bend behind the line, run out of at a GT3's pace on the
            // lane (about 4 m/s²).
            trip.handback_cap_mps = (0..=20)
                .map(|back| {
                    let back = 2.0 * back as f32;
                    let bend =
                        apexsim_server::pit::lane_bend_mps(&lane, s.pit.lane_station_m - back);
                    (bend * bend + 2.0 * 4.0 * back).sqrt()
                })
                .fold(lane.speed_limit_mps, f32::min);
            break;
        }
    }
    Some(trip)
}

#[test]
fn a_human_driving_into_the_pit_lane_is_driven_to_their_box_and_out() {
    let track = monza();
    if !track.has_sidecars(
        &[Sidecar::Pit],
        "a_human_driving_into_the_pit_lane_is_driven_to_their_box_and_out",
    ) {
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
    assert!(trip.handed_back_s.is_some(), "handed back at the exit line");
    assert!(
        trip.handback_mps > trip.handback_cap_mps - HANDBACK_SLACK_MPS,
        "handed back at the limit: {:.1} m/s against {:.1}",
        trip.handback_mps,
        trip.handback_cap_mps
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
    on_the_lane(&mut gs, &player, lane.limit_start_m - 3.0, 12.0);
    {
        let s = gs.session.participants.get_mut(&player).unwrap();
        s.auto_gearbox = false;
        s.steering_assist = true;
    }
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, PlayerInputData::default())].into();
    // The driver's own up to the entry line, taken over it.
    gs.tick(&inputs);
    assert!(
        !gs.session.participants[&player].pit.driving,
        "short of the entry line"
    );
    for _ in 0..HZ {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&player];
    assert!(
        s.pit.driving && s.auto_gearbox && !s.steering_assist,
        "the autopilot's aids"
    );
    for _ in 0..(HZ * 120) {
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
    let past = s.pit.lane_station_m - lane.limit_end_m;
    assert!(
        (0.0..5.0).contains(&past),
        "handed back at the exit line, not the lane's end: {past:.1} m past it"
    );
    assert!(
        !s.auto_gearbox && s.steering_assist,
        "the driver's own aids again"
    );
}

#[test]
fn a_driver_is_told_their_box_once() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    assert!(
        gs.take_pit_box_news().is_empty(),
        "no box dealt before a tick"
    );
    gs.tick(&HashMap::new());
    let news = gs.take_pit_box_news();
    assert_eq!(news.len(), 1);
    let (who, pit_box) = &news[0];
    assert_eq!(*who, player);
    let spot = lane.box_at(pit_box.pit_box);
    assert_eq!(
        (pit_box.x, pit_box.y, pit_box.yaw_rad),
        (spot.x, spot.y, spot.yaw_rad)
    );
    assert!(
        pit_box.lateral_m.abs() > 0.5 && pit_box.lateral_m.abs() < pit_box.lane_width_m / 2.0,
        "the spot is off the lane's middle, inside it: {}",
        pit_box.lateral_m
    );
    gs.tick(&HashMap::new());
    assert!(gs.take_pit_box_news().is_empty(), "told once");
    assert!(!pit_box.manual, "on the autopilot");

    // Switched to the manual pit lane: told again, so the client marks the box.
    gs.set_driver_aids(
        &player,
        DriverAids {
            pit_autopilot: Some(false),
            ..Default::default()
        },
    );
    let news = gs.take_pit_box_news();
    assert_eq!(news.len(), 1);
    assert!(news[0].1.manual);
    assert!(gs.take_pit_box_news().is_empty());
}

#[test]
fn a_driver_without_the_pit_autopilot_drives_the_lane_held_to_the_limit() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    gs.set_driver_aids(
        &player,
        DriverAids {
            auto_gearbox: true,
            pit_autopilot: Some(false),
            ..Default::default()
        },
    );
    // Over the entry line 10 m/s too fast, the throttle held open.
    on_the_lane(
        &mut gs,
        &player,
        lane.limit_start_m - 3.0,
        lane.speed_limit_mps + 10.0,
    );
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, flat_out)].into();
    for _ in 0..(HZ * 3) {
        gs.tick(&inputs);
        assert!(
            !gs.session.participants[&player].pit.driving,
            "never the autopilot's"
        );
    }
    let s = &gs.session.participants[&player];
    assert!(s.pit.limiter, "between the lines");
    assert!(
        s.speed_mps < lane.speed_limit_mps + 1.5,
        "braked down to the limit: {:.1} m/s against {:.1}",
        s.speed_mps,
        lane.speed_limit_mps
    );

    // Stopped at its own box, the crew work; the car is the driver's after.
    let spot = *lane.box_at(gs.session.participants[&player].pit.box_index.unwrap());
    {
        let state = gs.session.participants.get_mut(&player).unwrap();
        state.pos_x = spot.x;
        state.pos_y = spot.y;
        state.yaw_rad = spot.yaw_rad;
        state.vel_x = 0.0;
        state.vel_y = 0.0;
        state.speed_mps = 0.0;
        state.tires.front_left.wear_percent = 60.0;
    }
    let held = PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, held)].into();
    gs.tick(&inputs);
    let seconds = gs.session.participants[&player].pit.service_left_s;
    assert!(gs.session.participants[&player].pit.servicing, "serviced");
    for _ in 0..((seconds + 0.5) * HZ as f32) as usize {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&player];
    assert!(s.pit.serviced && !s.pit.servicing && !s.pit.driving);
    assert!(
        s.tires.front_left.wear_percent < 0.01,
        "fresh tyres: {}",
        s.tires.front_left.wear_percent
    );
}

/// The folder the lane surveys walk: the shipped circuits, or with
/// `PIT_CUSTOM=1` the player's imports (`content/tracks/custom`).
fn survey_dir() -> String {
    if std::env::var("PIT_CUSTOM").is_ok() {
        "../../../apexsim-content/tracks/custom".into()
    } else {
        "../../../apexsim-content/tracks/default".into()
    }
}

/// Every circuit's lane under the autopilot: a GT3 driven in, to its box,
/// serviced and out. Prints a line per circuit; fails on any that does not
/// make the whole trip.
///
/// `cargo test --release --test all pit_stop_test::pit_lane_survey -- --ignored --nocapture`
/// (`PIT_TRACKS=Monzarella,SpaFrankenchamps` for a few).
#[test]
#[ignore]
fn pit_lane_survey() {
    let only: Option<Vec<String>> = std::env::var("PIT_TRACKS")
        .ok()
        .map(|v| v.split(',').map(|s| s.trim().to_string()).collect());
    let mut failed = Vec::new();
    let mut stems: Vec<String> = std::fs::read_dir(survey_dir())
        .expect("tracks")
        .filter_map(|e| {
            let stem = e.ok()?.file_name().to_string_lossy().into_owned();
            std::path::Path::new(&survey_dir())
                .join(&stem)
                .join(format!("{stem}.pit.msgpack"))
                .is_file()
                .then_some(stem)
        })
        .collect();
    stems.sort();
    for stem in stems {
        if only.as_ref().is_some_and(|o| !o.contains(&stem)) {
            continue;
        }
        let Ok(track) = TrackLoader::load_from_file(format!("{}/{stem}/{stem}.yaml", survey_dir()))
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
            && trip.handback_mps > trip.handback_cap_mps - HANDBACK_SLACK_MPS
            && trip.worst_off_m < within
            && trip.stuck_s < 2.0
            && trip.recoveries == 0;
        println!(
            "{stem:14} {} boxes {boxes:2}  took over {}  box {:>6}  service {:4.1} s  out {:>6} at {:4.1}/{:4.1} m/s (lane {:4.1})  off {:4.1} m at {:5.0} {}  stuck {:4.1} s  recovered {}",
            if ok { "ok  " } else { "FAIL" },
            trip.took_over,
            trip.to_box_s.map_or("-".into(), |t| format!("{t:.1}")),
            trip.service_s,
            trip.handed_back_s.map_or("-".into(), |t| format!("{t:.1}")),
            trip.handback_mps,
            trip.exit_min_mps,
            trip.handback_cap_mps,
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
/// lane, over at most `laps` laps or twenty minutes a 7 km of lap.
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
    // Twenty minutes for a GP lap's length, longer for a longer lap (the
    // Nurburgring's 24h layout runs 14 minutes a lap).
    let lap_m: f32 = track
        .centerline
        .windows(2)
        .map(|w| (w[1].x - w[0].x).hypot(w[1].y - w[0].y))
        .sum();
    let minutes = (20.0 * (lap_m / 7000.0).max(1.0)).ceil() as usize;
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
    for _ in 0..(HZ * 60 * minutes) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&driver];
        off_ticks += (!s.is_on_track && !s.pit.in_lane) as u32;
        if trace && gs.session.current_tick.is_multiple_of(120) && (s.pit.driving || !s.is_on_track)
        {
            println!(
                "  t {:6.1} prog {:7.1} lane {:6.1} in_lane {} driving {} wants {} served {} v {:5.1} on_track {} pos ({:.1},{:.1})",
                gs.session.current_tick as f32 / HZ as f32, s.track_progress, s.pit.lane_station_m,
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
    (times, stops, off_ticks as f32 / HZ as f32)
}

/// Every circuit's lane in a race: an AI on worn tyres plans a stop,
/// drives in, is serviced at its box and races on.
///
/// `cargo test --release --test all pit_stop_test::ai_pit_survey -- --ignored --nocapture`
/// (`PIT_TRACKS=Monzarella,SpaFrankenchamps` for a few).
#[test]
#[ignore]
fn ai_pit_survey() {
    let only: Option<Vec<String>> = std::env::var("PIT_TRACKS")
        .ok()
        .map(|v| v.split(',').map(|s| s.trim().to_string()).collect());
    let mut stems: Vec<String> = std::fs::read_dir(survey_dir())
        .expect("tracks")
        .filter_map(|e| {
            let stem = e.ok()?.file_name().to_string_lossy().into_owned();
            std::path::Path::new(&survey_dir())
                .join(&stem)
                .join(format!("{stem}.pit.msgpack"))
                .is_file()
                .then_some(stem)
        })
        .collect();
    stems.sort();
    let mut failed = Vec::new();
    for stem in stems {
        if only.as_ref().is_some_and(|o| !o.contains(&stem)) {
            continue;
        }
        let Ok(track) = TrackLoader::load_from_file(format!("{}/{stem}/{stem}.yaml", survey_dir()))
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
    for _ in 0..(HZ * 8) {
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
    for tick in 0..(HZ * 6) {
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
    for _ in 0..(HZ * 20) {
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
    let mut stems: Vec<String> = std::fs::read_dir("../../../apexsim-content/tracks/default")
        .expect("tracks")
        .filter_map(|e| {
            let stem = e.ok()?.file_name().to_string_lossy().into_owned();
            std::path::Path::new("../../../apexsim-content/tracks/default")
                .join(&stem)
                .join(format!("{stem}.pit.msgpack"))
                .is_file()
                .then_some(stem)
        })
        .collect();
    stems.sort();
    let mut bad = Vec::new();
    for stem in &stems {
        let Ok(track) = TrackLoader::load_from_file(format!(
            "../../../apexsim-content/tracks/default/{stem}/{stem}.yaml"
        )) else {
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

/// The boxes are dealt by team (`pit::deal_team_boxes`): the cars of one
/// model in one livery stand side by side down the lane, and a driver who
/// joins later is put in the free box nearest their team-mates.
#[test]
fn team_mates_are_given_boxes_side_by_side() {
    let track = monza();
    if !track.has_sidecars(&[Sidecar::Pit], "pit_stop_test") {
        return;
    }
    let box_count = track.pit_lane.as_ref().unwrap().boxes.len();
    assert!(box_count >= 10, "Monza has room for the field");
    let mut car = gt3();
    // Two liveries: the AI take them in turn, so teams of three.
    car.livery_names = vec!["Second".to_string()];
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let profiles: Vec<AiDriverProfile> = (0..6)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("Team AI {}", i + 1), 95);
            p.id = Uuid::from_u128(5000 + i as u128);
            p
        })
        .collect();
    let host = Uuid::from_u128(1);
    let session = RaceSession::new(host, track.id, SessionKind::Multiplayer, 10, 6, 3);
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, profiles);
    gs.spawn_ai_drivers();
    gs.open_grid(10);
    gs.tick(&HashMap::new());

    let teams = gs.teams();
    let mut by_team: HashMap<u32, Vec<u8>> = HashMap::new();
    for (id, state) in &gs.session.participants {
        by_team
            .entry(teams[id])
            .or_default()
            .push(state.pit.box_index.expect("a box dealt"));
    }
    println!("boxes by team: {by_team:?}");
    assert_eq!(by_team.len(), 2, "two teams");
    for list in by_team.values_mut() {
        list.sort();
        assert_eq!(list.len(), 3);
        assert!(
            list.windows(2).all(|w| w[1] == w[0] + 1),
            "a team's boxes side by side: {list:?}"
        );
    }

    // A driver joining in a team's livery goes in the free box nearest it.
    let late = Uuid::from_u128(77);
    gs.set_livery(late, 1);
    gs.add_player(late, car.id).expect("seat");
    gs.tick(&HashMap::new());
    let teams = gs.teams();
    let mates: Vec<u8> = gs
        .session
        .participants
        .iter()
        .filter(|(id, _)| **id != late && teams[*id] == teams[&late])
        .map(|(_, s)| s.pit.box_index.unwrap())
        .collect();
    assert_eq!(mates.len(), 3, "the late driver is in a team of three");
    let mine = gs.session.participants[&late].pit.box_index.unwrap();
    println!("late driver's box {mine}, team-mates {mates:?}");
    let held: Vec<u8> = gs
        .session
        .participants
        .iter()
        .filter(|(id, _)| **id != late)
        .map(|(_, s)| s.pit.box_index.unwrap())
        .collect();
    let distance = |b: u8| mates.iter().map(|m| m.abs_diff(b)).min().unwrap();
    let best = (0..box_count as u8)
        .filter(|b| !held.contains(b))
        .map(distance)
        .min()
        .unwrap();
    assert_eq!(distance(mine), best, "the free box nearest the team");
}

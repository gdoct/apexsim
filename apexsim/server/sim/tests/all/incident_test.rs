//! The race's incident list for stewards (`incidents`): a contact forced
//! between two cars, a car put off the road, a penalty given and a steward
//! taking it back, each listed with its cars, tick and lap.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_sim::ai_driver::generate_ai_profiles;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::data::*;
use apexsim_sim::game_session::GameSession;
use apexsim_sim::incidents::IncidentKind;
use apexsim_sim::race_control::Offence;
use apexsim_sim::track_loader::TrackLoader;

const TICK_RATE: u16 = apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(path)
}

/// Three AI cars racing at the Monzarella, which has run-off beside its
/// straights, under every penalty rule.
fn race() -> GameSession {
    let track = TrackLoader::load_from_file(repo(
        "../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    ))
    .expect("track loads");
    let car =
        CarLoader::load_from_file(&repo("../apexsim-content/cars/default/posh-gt3rs/car.toml"))
            .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);
    let mut profiles = generate_ai_profiles(3, Some(95));
    for (i, profile) in profiles.iter_mut().enumerate() {
        profile.preferred_car_id = Some(car_id);
        profile.id = uuid::Uuid::from_u128(0x1000 + i as u128);
    }
    let session = RaceSession::new(
        uuid::Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        3,
        3,
        5,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    race.set_race_rules(RaceRules {
        jump_start: true,
        yellow_penalty: true,
        pit_exit_penalty: true,
        ..Default::default()
    });
    race.set_game_mode(GameMode::Race);
    race
}

fn tick(race: &mut GameSession) {
    let inputs: HashMap<PlayerId, PlayerInputData> = race
        .session
        .participants
        .keys()
        .map(|id| (*id, race.generate_ai_input(id)))
        .collect();
    race.tick(&inputs);
}

fn run(race: &mut GameSession, seconds: f32) {
    for _ in 0..(TICK_RATE as f32 * seconds) as u32 {
        tick(race);
    }
}

#[test]
fn a_forced_contact_a_trip_off_the_road_and_a_penalty_are_listed() {
    let mut race = race();
    run(&mut race, 6.0);
    assert_eq!(race.session.state, SessionState::Racing);
    assert!(
        race.incidents.incidents.is_empty(),
        "a clean start lists nothing: {:?}",
        race.incidents.incidents
    );
    let ids: Vec<PlayerId> = race.session.participants.keys().copied().collect();
    let (a, b, c) = (ids[0], ids[1], ids[2]);

    // B put nose to tail behind A (half a metre into it), closing at 6 m/s.
    let front = race.session.participants[&a].clone();
    {
        let back = race.session.participants.get_mut(&b).unwrap();
        let (dx, dy) = (front.yaw_rad.cos(), front.yaw_rad.sin());
        back.pos_x = front.pos_x - 4.2 * dx;
        back.pos_y = front.pos_y - 4.2 * dy;
        back.pos_z = front.pos_z;
        back.yaw_rad = front.yaw_rad;
        back.vel_x = front.vel_x + 6.0 * dx;
        back.vel_y = front.vel_y + 6.0 * dy;
        back.vel_z = front.vel_z;
        back.speed_mps = (back.vel_x.powi(2) + back.vel_y.powi(2)).sqrt();
        back.track_progress = front.track_progress;
        back.nearest_centerline_idx = front.nearest_centerline_idx;
    }
    let contact_tick = race.session.current_tick + 1;
    tick(&mut race);
    let contacts: Vec<_> = race
        .incidents
        .incidents
        .iter()
        .filter(|i| matches!(i.kind, IncidentKind::Contact { .. }))
        .collect();
    assert_eq!(contacts.len(), 1, "{:?}", race.incidents.incidents);
    let contact = contacts[0];
    let (lo, hi) = if a < b { (a, b) } else { (b, a) };
    assert_eq!(contact.car, lo);
    assert_eq!(contact.tick, contact_tick);
    assert_eq!(contact.lap, race.session.participants[&lo].current_lap);
    let IncidentKind::Contact {
        other, closing_mps, ..
    } = contact.kind
    else {
        unreachable!()
    };
    assert_eq!(other, hi);
    assert!(closing_mps > 3.0, "closing at {closing_mps}");
    // Rubbing on is the same contact.
    run(&mut race, 0.5);
    let count = |race: &GameSession, f: fn(&IncidentKind) -> bool| {
        race.incidents
            .incidents
            .iter()
            .filter(|i| f(&i.kind))
            .count()
    };
    assert_eq!(
        count(&race, |k| matches!(k, IncidentKind::Contact { .. })),
        1
    );

    // C put well off the road beside it, at rest.
    {
        let track = &race.track_config;
        let state = &race.session.participants[&c];
        let idx = state.nearest_centerline_idx.unwrap_or(0) as usize;
        let point = track.centerline[idx].clone();
        let side = point.width_right_m.max(point.width_left_m) + 12.0;
        let (nx, ny) = (-point.heading_rad.sin(), point.heading_rad.cos());
        let state = race.session.participants.get_mut(&c).unwrap();
        state.pos_x = point.x + nx * side;
        state.pos_y = point.y + ny * side;
        state.vel_x = 0.0;
        state.vel_y = 0.0;
        state.vel_z = 0.0;
        state.speed_mps = 0.0;
    }
    // It settles onto the grass, then is off for the limit's 0.2 s.
    run(&mut race, 1.0);
    assert!(race.session.participants[&c].laps.invalid);
    let limits: Vec<_> = race
        .incidents
        .incidents
        .iter()
        .filter(|i| matches!(i.kind, IncidentKind::TrackLimits { .. }))
        .collect();
    assert_eq!(limits.len(), 1, "{:?}", race.incidents.incidents);
    assert_eq!(limits[0].car, c);
    assert_eq!(limits[0].kind, IncidentKind::TrackLimits { cut: false });

    // A penalty given, then taken back by a steward.
    race.penalise(a, Offence::JumpStart);
    let given = race.incidents.incidents.last().unwrap().clone();
    assert_eq!(given.car, a);
    // Without the baked pit sidecar (CI) there is no lane to serve it in.
    let lane = race.track_config.pit_lane.is_some();
    let kind = if lane { "drive_through" } else { "time" };
    assert!(
        matches!(&given.kind, IncidentKind::Penalty { offence, penalty, .. }
            if offence == "jump_start" && penalty == kind),
        "{given:?}"
    );
    tick(&mut race);
    assert_eq!(race.session.participants[&a].pit.wants_stop, lane);
    race.clear_penalty(&a, 0).expect("cleared");
    assert!(!race.penalties.contains_key(&a));
    assert!(
        !race.session.participants[&a].pit.wants_stop,
        "coming in only to serve it, it stays out"
    );
    assert!(race.clear_penalty(&a, 0).is_err());
    assert!(matches!(
        race.incidents.incidents.last().unwrap().kind,
        IncidentKind::PenaltyCleared { .. }
    ));

    // The list is the race's: a new start begins it again.
    race.line_up_on_grid();
    assert!(race.incidents.incidents.is_empty());
}

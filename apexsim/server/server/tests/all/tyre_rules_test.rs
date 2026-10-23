//! The rules that count a car's tyre sets (`RaceRules`,
//! `apexsim_server::tyre_sets`), driven through `GameSession` at the
//! Norisring: the qualifying tyre allowance sends a car out on a used set
//! once its new ones are spent, and the front of the qualifying
//! classification starts the race on the set that placed it.

use std::collections::HashMap;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn norisring() -> TrackConfig {
    TrackLoader::load_from_file(
        "../../../apexsim-content/tracks/default/Snorisring/Snorisring.yaml",
    )
    .expect("failed to load the Norisring")
}

/// A session at the Norisring with `ai` AI cars and the host seated.
fn session(ai: usize, rules: RaceRules) -> (GameSession, PlayerId, Vec<PlayerId>) {
    let car = CarConfig::default();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let profiles: Vec<AiDriverProfile> = (0..ai)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("Tyre AI {}", i + 1), 95);
            p.id = Uuid::from_u128(4000 + i as u128);
            p
        })
        .collect();
    let ai_ids = profiles.iter().map(|p| p.id).collect();
    let host = Uuid::from_u128(1);
    let session = RaceSession::new(
        host,
        norisring().id,
        SessionKind::Multiplayer,
        8,
        ai as u8,
        3,
    );
    let mut gs = GameSession::with_ai_profiles(session, norisring(), car_configs, profiles);
    gs.spawn_ai_drivers();
    gs.add_player(host, car_id).expect("seat");
    gs.set_driver_name(host, "Host");
    gs.set_race_rules(rules);
    (gs, host, ai_ids)
}

fn wear(gs: &GameSession, id: &PlayerId) -> [f32; 4] {
    gs.session.participants[id]
        .tires
        .each()
        .map(|t| t.wear_percent)
}

/// Wear the set on `id`'s car to `percent` (as laps on it would).
fn wear_to(gs: &mut GameSession, id: &PlayerId, percent: f32) {
    for tyre in gs
        .session
        .participants
        .get_mut(id)
        .unwrap()
        .tires
        .each_mut()
    {
        tyre.wear_percent = percent;
    }
}

#[test]
fn past_the_allowance_a_car_goes_out_on_its_least_worn_used_set() {
    let rules = RaceRules {
        qualifying_tyre_sets: 2,
        ..Default::default()
    };
    let (mut gs, host, _) = session(0, rules);
    gs.set_game_mode(GameMode::Qualification);
    assert!(gs.session.participants[&host].in_garage);

    // Two new sets: each run out of the garage starts on one.
    for worn in [30.0, 12.0] {
        gs.hotlap_relocate(&host, HotlapDestination::Track, false)
            .expect("out");
        assert_eq!(wear(&gs, &host), [0.0; 4], "a new set");
        wear_to(&mut gs, &host, worn);
        gs.hotlap_relocate(&host, HotlapDestination::Garage, false)
            .expect("in");
    }
    // Spent: the least worn used set goes back on, then the other.
    gs.hotlap_relocate(&host, HotlapDestination::Track, false)
        .expect("out");
    assert_eq!(wear(&gs, &host), [12.0; 4], "the least worn used set");
    gs.hotlap_relocate(&host, HotlapDestination::Garage, false)
        .unwrap();
    gs.hotlap_relocate(&host, HotlapDestination::Track, false)
        .unwrap();
    assert_eq!(
        wear(&gs, &host),
        [12.0; 4],
        "the set just brought in is still the least worn"
    );
    // The car runs on that set: it ticks without trouble.
    for _ in 0..HZ {
        gs.tick(&HashMap::new());
    }

    // A fresh qualifying session counts afresh.
    gs.hotlap_relocate(&host, HotlapDestination::Garage, false)
        .unwrap();
    gs.set_game_mode(GameMode::Qualification);
    gs.hotlap_relocate(&host, HotlapDestination::Track, false)
        .unwrap();
    assert_eq!(wear(&gs, &host), [0.0; 4], "a new qualifying, a new set");
}

#[test]
fn without_an_allowance_every_run_is_on_a_new_set() {
    let (mut gs, host, _) = session(0, RaceRules::default());
    gs.set_game_mode(GameMode::Qualification);
    for _ in 0..3 {
        gs.hotlap_relocate(&host, HotlapDestination::Track, false)
            .unwrap();
        assert_eq!(wear(&gs, &host), [0.0; 4]);
        wear_to(&mut gs, &host, 40.0);
        gs.hotlap_relocate(&host, HotlapDestination::Garage, false)
            .unwrap();
    }
}

/// Run open-ended qualifying with the AI driving until each has ended
/// `laps` timed laps, classifying them as the game loop does.
fn qualify(gs: &mut GameSession, ai_ids: &[PlayerId], laps: usize) {
    let mut timed: HashMap<PlayerId, usize> = HashMap::new();
    let limit = HZ * 600;
    for tick in 0.. {
        assert!(tick < limit, "qualifying never ran its laps: {timed:?}");
        if ai_ids
            .iter()
            .all(|id| timed.get(id).copied().unwrap_or(0) >= laps)
        {
            break;
        }
        let inputs: HashMap<PlayerId, PlayerInputData> = ai_ids
            .iter()
            .map(|id| (*id, gs.generate_ai_input(id)))
            .collect();
        gs.tick(&inputs);
        for lap in gs.take_lap_events() {
            if let Some(ms) = lap.event.lap_time_ms {
                *timed.entry(lap.player_id).or_default() += 1;
                // Every lap classified, struck or not: the AI seldom keeps
                // a Norisring lap inside the track limits, and what is
                // checked here is the set, not the lap.
                gs.note_qualifying_lap(lap.player_id, ms);
            }
        }
    }
}

#[test]
fn the_front_of_qualifying_starts_the_race_on_the_set_that_placed_it() {
    let rules = RaceRules {
        qualifying_tyre_start: 1,
        ..Default::default()
    };
    let (mut gs, _host, ai_ids) = session(2, rules);
    gs.set_game_mode(GameMode::Qualification);
    qualify(&mut gs, &ai_ids, 2);
    let order = gs.qualifying_order();
    assert!(!order.is_empty(), "somebody set a time");
    let pole = order[0];
    let worn_at_lap: f32 = wear(&gs, &pole).iter().copied().fold(0.0, f32::max);
    assert!(worn_at_lap > 0.0, "qualifying laps wear the tyres");

    gs.line_up_on_grid();
    let pole_wear = wear(&gs, &pole);
    let pole_worst = pole_wear.iter().copied().fold(0.0, f32::max);
    println!("pole starts on wear {pole_wear:?} (at the end of qualifying {worn_at_lap})");
    assert!(pole_worst > 0.0, "pole starts on its qualifying set");
    assert!(
        pole_worst <= worn_at_lap + 1e-3,
        "the set as it was when the lap was set, no more worn than after"
    );
    assert!(
        gs.session.participants[&pole].pit.stint_start_laps < 0.0,
        "the set's laps count in its stint, for the AI's wear rate"
    );
    // Only the front: the rest start on new sets.
    for id in ai_ids.iter().filter(|id| **id != pole) {
        assert_eq!(wear(&gs, id), [0.0; 4], "behind the front, new tyres");
    }
}

#[test]
fn without_the_rule_the_race_starts_on_new_tyres() {
    let (mut gs, _host, ai_ids) = session(1, RaceRules::default());
    gs.set_game_mode(GameMode::Qualification);
    qualify(&mut gs, &ai_ids, 1);
    gs.line_up_on_grid();
    assert_eq!(wear(&gs, &ai_ids[0]), [0.0; 4]);
    assert_eq!(
        gs.session.participants[&ai_ids[0]].pit.stint_start_laps,
        0.0
    );
}

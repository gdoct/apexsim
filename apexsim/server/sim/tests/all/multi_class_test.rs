//! A multi-class field (`RaceSession::classes`): the AI dealt per class,
//! the classes ranked by their planned lap of the track, the grid by class
//! and each car classified in its class as well as overall.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_sim::ai_driver::generate_class_profiles;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::data::*;
use apexsim_sim::game_session::GameSession;
use apexsim_sim::track_loader::TrackLoader;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(path)
}

fn car(stem: &str) -> CarConfig {
    CarLoader::load_from_file(&repo(&format!(
        "../apexsim-content/cars/default/{stem}/car.toml"
    )))
    .expect("car loads")
}

/// A session at the Norisring with every shipped GT3 and LMP2 loaded, the
/// host in a GT3, and the classes given slowest first.
fn two_class_session() -> GameSession {
    let track = TrackLoader::load_from_file(repo(
        "../apexsim-content/tracks/default/Snorisring/Snorisring.yaml",
    ))
    .expect("track loads");
    let host = car("posh-gt3rs");
    let host_car = host.id;
    let cars: HashMap<CarConfigId, CarConfig> = [
        "posh-gt3rs",
        "murcetes-amd-gt3",
        "limbotiti-caravan-gt3",
        "yotota-lmp2",
        "posh-lmp2",
        "fugazzi-lmp2",
        "jeanetti-lmp2",
    ]
    .into_iter()
    .map(car)
    .map(|c| (c.id, c))
    .collect();
    let mut session = RaceSession::new(
        uuid::Uuid::from_u128(7),
        track.id,
        SessionKind::Multiplayer,
        12,
        0,
        3,
    );
    session.host_car_id = Some(host_car);
    let mut race = GameSession::new(session, track, cars);
    race.set_classes(vec![
        ClassEntry {
            class: "gt3".into(),
            ai_count: 3,
            ai_skill: None,
        },
        ClassEntry {
            class: "LMP2".into(),
            ai_count: 4,
            ai_skill: Some(100),
        },
        ClassEntry {
            class: "Zeppelin".into(),
            ai_count: 2,
            ai_skill: None,
        },
    ]);
    let spec: Vec<(String, u8, Option<u8>)> = race
        .session
        .classes
        .iter()
        .map(|c| (c.class.clone(), c.ai_count, c.ai_skill))
        .collect();
    let profiles = generate_class_profiles(&spec, None);
    race.session.ai_count = profiles.len() as u8;
    race.set_ai_profiles(profiles);
    race.spawn_ai_drivers();
    race
}

#[test]
fn the_classes_are_ranked_and_the_ai_dealt_per_class() {
    let race = two_class_session();
    let classes: Vec<&str> = race
        .session
        .classes
        .iter()
        .map(|c| c.class.as_str())
        .collect();
    assert_eq!(
        classes,
        vec!["LMP2", "gt3"],
        "the quicker class first, a class with no car dropped"
    );
    assert_eq!(race.session.ai_count, 7);
    assert_eq!(race.session.participants.len(), 7);

    let mut per_class: HashMap<String, Vec<u8>> = HashMap::new();
    for id in &race.session.ai_player_ids {
        let state = &race.session.participants[id];
        let class = race.car_configs[&state.car_config_id].class.clone();
        let profile = race.get_ai_profile(id).expect("an AI");
        assert!(
            profile.class.eq_ignore_ascii_case(&class),
            "{} seated in a {class}, its profile says {}",
            profile.name,
            profile.class
        );
        per_class
            .entry(class.to_ascii_uppercase())
            .or_default()
            .push(profile.skill_level);
    }
    assert_eq!(per_class["GT3"].len(), 3);
    assert_eq!(per_class["LMP2"].len(), 4);
    assert!(
        per_class["LMP2"].iter().all(|s| (98..=102).contains(s)),
        "the LMP2 field round its own level: {:?}",
        per_class["LMP2"]
    );
    assert!(
        per_class["GT3"].iter().any(|s| *s < 90),
        "the GT3 field is the mixed one: {:?}",
        per_class["GT3"]
    );
    let names: std::collections::BTreeSet<&str> =
        race.ai_profiles.values().map(|p| p.name.as_str()).collect();
    assert_eq!(names.len(), 7, "no two drivers share a name");

    // Every LMP2 ranks ahead of every GT3.
    for id in &race.session.ai_player_ids {
        let state = &race.session.participants[id];
        let class = &race.car_configs[&state.car_config_id].class;
        let rank = race.class_rank_of(state.car_config_id);
        assert_eq!(rank, if class == "LMP2" { 0 } else { 1 }, "{class}");
    }
}

#[test]
fn the_grid_and_the_classification_are_by_class() {
    let race = two_class_session();
    let slots = race.starting_slots();
    let rank_of = |id: &PlayerId| race.class_rank_of(race.session.participants[id].car_config_id);
    let mut by_slot: Vec<(u8, u8)> = slots.iter().map(|(id, s)| (*s, rank_of(id))).collect();
    by_slot.sort();
    assert_eq!(
        by_slot.iter().map(|(_, r)| *r).collect::<Vec<u8>>(),
        vec![0, 0, 0, 0, 1, 1, 1],
        "the LMP2s on the first four slots, the GT3s behind: {by_slot:?}"
    );

    let table = race.classification(&HashMap::new());
    let mut lmp2 = 0;
    let mut gt3 = 0;
    for car in &table {
        match car.class.as_str() {
            "LMP2" => {
                lmp2 += 1;
                assert_eq!(car.class_position, lmp2, "{car:?}");
            }
            "GT3" => {
                gt3 += 1;
                assert_eq!(car.class_position, gt3, "{car:?}");
            }
            other => panic!("a car of class {other:?}"),
        }
    }
    assert_eq!((lmp2, gt3), (4, 3));
}

//! The safety cars (`race_control::Course`): a car stranded on the road
//! neutralises the race. The virtual one holds every car to its pace and
//! ends once the road is clear; the one on the road (`crate::safety_car`)
//! comes out of the pit exit, the field queues behind it, lapped cars are
//! let by, the pit lane is closed then open, and the leader restarts the
//! race at the line.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_server::ai_driver::generate_ai_profiles;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::race_control::{Course, VSC_SPEED_MPS};
use apexsim_server::race_start::RacePhase;
use apexsim_server::safety_car::{ScStage, SC_GAP_M, SC_QUEUED_M};
use apexsim_server::track_loader::TrackLoader;

const TICK_RATE: u16 = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path)
}

fn race(rules: RaceRules) -> GameSession {
    race_of(rules, 4)
}

fn race_of(rules: RaceRules, count: u8) -> GameSession {
    let track =
        TrackLoader::load_from_file(repo("content/tracks/default/Norisring/Norisring.yaml"))
            .expect("track loads");
    let car = CarLoader::load_from_file(&repo("content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);
    let mut profiles = generate_ai_profiles(count, Some(95));
    for (i, profile) in profiles.iter_mut().enumerate() {
        profile.preferred_car_id = Some(car_id);
        profile.id = uuid::Uuid::from_u128(0x1000 + i as u128);
    }
    let session = RaceSession::new(
        uuid::Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        count,
        count,
        10,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    race.set_race_rules(rules);
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

/// Race 20 s, then one car breaks down where it is.
fn break_one_down(race: &mut GameSession) -> PlayerId {
    for _ in 0..(TICK_RATE as u32 * 20) {
        tick(race);
    }
    let id = *race.session.participants.keys().last().unwrap();
    race.session
        .participants
        .get_mut(&id)
        .unwrap()
        .damage
        .is_drivable = false;
    id
}

#[test]
fn a_stranded_car_brings_out_the_virtual_safety_car() {
    let mut race = race(RaceRules {
        safety_car: SafetyCar::Virtual,
        ..Default::default()
    });
    let broken = break_one_down(&mut race);
    let mut out_at = None;
    let mut ended_at = None;
    let mut fastest_held = 0.0f32;
    for t in 0..(TICK_RATE as u32 * 90) {
        tick(&mut race);
        match race.race_control.course {
            Course::Vsc => {
                out_at.get_or_insert(t);
                assert_eq!(race.race_phase(), Some(RacePhase::Vsc as u8));
                // Some seconds after it came out the field is at its pace.
                if t > out_at.unwrap() + TICK_RATE as u32 * 15 {
                    for s in race.session.participants.values() {
                        if s.player_id != broken && !s.pit.in_lane {
                            fastest_held = fastest_held.max(s.speed_mps);
                        }
                    }
                }
            }
            Course::VscEnding => {
                assert_eq!(race.race_phase(), Some(RacePhase::VscEnding as u8));
            }
            Course::Green => {
                if out_at.is_some() {
                    ended_at = Some(t);
                    break;
                }
            }
            Course::Sc | Course::ScInThisLap => panic!("no safety car on the road under Virtual"),
        }
    }
    assert!(out_at.is_some(), "the VSC came out");
    assert!(ended_at.is_some(), "and went in once the car was towed");
    assert!(
        fastest_held <= VSC_SPEED_MPS + 1.0,
        "held to the pace: {fastest_held:.1} m/s"
    );
    assert!(race
        .session
        .participants
        .values()
        .all(|s| s.speed_cap_mps == 0.0));
    assert_eq!(race.race_phase(), None);
    assert!(race.race_control.safety_car.is_none());
}

#[test]
fn without_the_rule_a_stranded_car_brings_out_yellows_only() {
    let mut race = race(RaceRules::default());
    break_one_down(&mut race);
    for _ in 0..(TICK_RATE as u32 * 30) {
        tick(&mut race);
        assert_eq!(race.race_control.course, Course::Green);
    }
}

/// Laps run by each car, the lap in progress as a share.
fn laps_of(race: &GameSession) -> Vec<(PlayerId, f32)> {
    let total = apexsim_server::laps::track_length_m(&race.track_config);
    race.session
        .participants
        .values()
        .map(|s| (s.player_id, s.current_lap as f32 + s.track_progress / total))
        .collect()
}

/// Whether a car is on the road and racing (as the safety car reads it).
fn running(s: &CarState) -> bool {
    !s.in_garage
        && !s.towed
        && !(s.pit.in_lane && (s.pit.limiter || s.pit.servicing))
        && s.finish_position.is_none()
        && s.damage.is_drivable
}

#[test]
fn a_stranded_car_brings_out_the_safety_car_on_the_road() {
    let mut race = race_of(
        RaceRules {
            safety_car: SafetyCar::Full,
            yellow_penalty: true,
            ..Default::default()
        },
        5,
    );
    let total = apexsim_server::laps::track_length_m(&race.track_config);
    let broken = break_one_down(&mut race);
    // Once the queue has formed, the second car in it is put a lap down:
    // a lapped car in the queue, to be let by.
    let mut lapped = broken;

    let lane_end = {
        let lane = race.track_config.pit_lane.as_ref().expect("a pit lane");
        *lane.nodes.last().unwrap()
    };
    let mut deployed = false;
    let mut closed_seen = false;
    let mut opened_seen = false;
    let mut formed_seen = false;
    let mut lapped_passed_sc = false;
    let mut in_this_lap_at = None;
    let mut green_at = None;
    let mut worst_queue_gap = 0.0f32;
    let mut last_behind: Option<f32> = None;
    for t in 0..(TICK_RATE as u32 * 900) {
        tick(&mut race);
        let course = race.race_control.course;
        if course == Course::Sc && !deployed {
            deployed = true;
            let sc = race.race_control.safety_car.as_ref().expect("out");
            assert_eq!(sc.stage, ScStage::Waiting);
            let d = ((sc.pose.x - lane_end[0]).powi(2) + (sc.pose.y - lane_end[1]).powi(2)).sqrt();
            assert!(d < 1.0, "waiting at the pit exit, {d:.1} m off it");
            assert!(race.race_control.pit_closed(), "the lane is closed");
            assert_eq!(race.race_phase(), Some(RacePhase::SafetyCar as u8));
            let frame = race.get_compact_telemetry();
            assert!(frame.safety_car.is_some(), "its pose on the wire");
        }
        if !deployed {
            assert_eq!(
                course,
                Course::Green,
                "not out before a car is stranded long"
            );
            continue;
        }
        closed_seen |= race.race_control.pit_closed();
        if lapped == broken
            && race
                .race_control
                .safety_car
                .as_ref()
                .is_some_and(|sc| sc.formed)
        {
            let mut order = laps_of(&race);
            order.retain(|(id, _)| running(&race.session.participants[id]));
            order.sort_by(|a, b| b.1.total_cmp(&a.1));
            lapped = order[1].0;
            last_behind = None;
            race.session
                .participants
                .get_mut(&lapped)
                .unwrap()
                .current_lap -= 1;
        }
        if let Some(sc) = race.race_control.safety_car.as_ref() {
            opened_seen |= sc.pit_open && closed_seen;
            formed_seen |= sc.formed;
            // The lapped car goes past the safety car.
            if sc.stage == ScStage::OnTrack {
                let s = &race.session.participants[&lapped];
                let behind = (sc.progress_m - s.track_progress).rem_euclid(total);
                if let Some(was) = last_behind {
                    if was < 60.0 && behind > total - 60.0 {
                        lapped_passed_sc = true;
                    }
                }
                last_behind = Some(behind);
            }
        }
        if course == Course::ScInThisLap && in_this_lap_at.is_none() {
            in_this_lap_at = Some(t);
            assert_eq!(race.race_phase(), Some(RacePhase::ScInThisLap as u8));
            // The queue: every car on the road single file behind the
            // safety car, in race order, closed up.
            let sc = race.race_control.safety_car.as_ref().unwrap();
            assert!(sc.formed && sc.pit_open);
            let mut queue: Vec<(f32, f32, PlayerId)> = race
                .session
                .participants
                .values()
                .filter(|s| running(s))
                .map(|s| {
                    (
                        (sc.progress_m - s.track_progress).rem_euclid(total),
                        s.current_lap as f32 + s.track_progress / total,
                        s.player_id,
                    )
                })
                .collect();
            queue.sort_by(|a, b| a.0.total_cmp(&b.0));
            // The lapped car let by catches the queue up on its own.
            let mut front = 0.0;
            for (behind, _, id) in &queue {
                if *id != lapped {
                    worst_queue_gap = worst_queue_gap.max(behind - front);
                    front = *behind;
                }
            }
            for pair in queue.windows(2) {
                assert!(pair[0].1 > pair[1].1, "in race order: {queue:?}");
            }
            // The lapped car, unless it stopped under it, is at the back.
            if queue.iter().any(|q| q.2 == lapped) {
                assert_eq!(
                    queue.last().map(|q| q.2),
                    Some(lapped),
                    "the lapped car at the back"
                );
            }
        }
        if in_this_lap_at.is_some() && course == Course::Green {
            green_at = Some(t);
            break;
        }
    }
    assert!(deployed, "the safety car came out");
    assert!(closed_seen && opened_seen, "the lane closed, then opened");
    assert!(formed_seen, "the queue formed");
    assert!(lapped_passed_sc, "the lapped car was let by");
    assert!(in_this_lap_at.is_some(), "in this lap");
    assert!(green_at.is_some(), "and green again");
    assert!(
        worst_queue_gap <= SC_QUEUED_M,
        "queued within {SC_QUEUED_M} m (held at {SC_GAP_M}): {worst_queue_gap:.1} m"
    );
    // Nobody passed under it: no penalty given.
    assert!(
        race.penalties.values().all(|list| list.is_empty()),
        "{:?}",
        race.penalties
    );
    // Green: no car held, the safety car gone, nothing on the wire.
    assert!(race.race_control.safety_car.is_none());
    assert!(race
        .session
        .participants
        .values()
        .all(|s| s.speed_cap_mps == 0.0));
    assert_eq!(race.race_phase(), None);
    assert!(race.get_compact_telemetry().safety_car.is_none());
    // Never a participant: the classification is the field's.
    let classified = race.classification(&HashMap::new());
    assert_eq!(classified.len(), race.session.participants.len());
    assert!(classified
        .iter()
        .all(|c| race.session.participants.contains_key(&c.player_id)));
}

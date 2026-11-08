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

/// Put `id` `side` metres off the road beside where it is (0: back on the
/// line), at rest. Set down on the surface, not dropped from above it: without
/// the baked ground (CI) a car's height is held past the road's margin, so one
/// let go above the ground there would hang with its wheels unloaded and never
/// count as off the track.
fn put(race: &mut GameSession, id: PlayerId, side: f32) {
    let track = &race.track_config;
    let state = &race.session.participants[&id];
    let idx = state.nearest_centerline_idx.unwrap_or(0) as usize;
    let point = track.centerline[idx].clone();
    let side = if side > 0.0 {
        point.width_right_m.max(point.width_left_m) + side
    } else {
        0.0
    };
    let (nx, ny) = (-point.heading_rad.sin(), point.heading_rad.cos());
    let (x, y) = (point.x + nx * side, point.y + ny * side);
    let ground = apexsim_sim::physics::probe_surface(track, x, y, point.z, Some(idx))
        .expect("the track is under the car")
        .context
        .elevation;
    let state = race.session.participants.get_mut(&id).unwrap();
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = ground;
    state.yaw_rad = point.heading_rad;
    state.vel_x = 0.0;
    state.vel_y = 0.0;
    state.vel_z = 0.0;
    state.speed_mps = 0.0;
}

/// Under the track limits rule a car's first three trips off the road are
/// warnings and the fourth costs it a time penalty; without the rule none
/// does.
#[test]
fn track_limits_cost_time_after_three_warnings() {
    use apexsim_sim::race_control::{TRACK_LIMITS_PENALTY_S, TRACK_LIMITS_WARNINGS};
    for rule in [true, false] {
        let mut race = race();
        race.set_race_rules(RaceRules {
            track_limits_penalty: rule,
            ..Default::default()
        });
        let ids: Vec<PlayerId> = race.session.participants.keys().copied().collect();
        let car = ids[2];
        // Into the race proper.
        while race.session.state != SessionState::Racing {
            tick(&mut race);
        }
        run(&mut race, 5.0);
        let penalties = |race: &GameSession| race.penalties.get(&car).map_or(0, |p| p.len());
        for trip in 1..=TRACK_LIMITS_WARNINGS + 1 {
            put(&mut race, car, 12.0);
            run(&mut race, 1.0);
            put(&mut race, car, 0.0);
            run(&mut race, 1.0);
            assert_eq!(
                race.track_limit_strikes.get(&car),
                Some(&trip),
                "trip {trip}"
            );
            // The driver is warned through the roster, under the rule only.
            let row = race
                .roster_rows(&HashMap::new())
                .into_iter()
                .find(|r| r.player_id == car)
                .unwrap();
            assert_eq!(row.track_limit_strikes, if rule { trip } else { 0 });
            if trip <= TRACK_LIMITS_WARNINGS || !rule {
                assert_eq!(penalties(&race), 0, "trip {trip} is a warning");
            }
        }
        if rule {
            let given = &race.penalties[&car];
            assert_eq!(given.len(), 1);
            assert_eq!(given[0].offence, Offence::TrackLimits);
            assert_eq!(given[0].seconds, TRACK_LIMITS_PENALTY_S);
            assert_eq!(race.penalty_seconds(&car), TRACK_LIMITS_PENALTY_S as u32);
        }
        // A new start counts again.
        race.line_up_on_grid();
        assert!(race.track_limit_strikes.is_empty());
    }
}

#[test]
fn track_limits_penalise_the_fourth_strike_and_every_fourth_after() {
    use apexsim_sim::race_control::track_limits_penalised;
    let penalised: Vec<u16> = (1..=13).filter(|&s| track_limits_penalised(s)).collect();
    assert_eq!(penalised, vec![4, 8, 12]);
}

/// A car back on the road from a trip off it, just ahead of a car arriving
/// fast, is listed as an unsafe rejoin naming that car, and under the
/// unsafe rejoin rule costs it a time penalty; rejoining with nobody coming
/// is neither.
#[test]
fn a_rejoin_in_front_of_a_car_arriving_fast_is_listed() {
    use apexsim_sim::race_control::UNSAFE_REJOIN_PENALTY_S;
    let unsafe_rejoins = |race: &GameSession| -> Vec<(PlayerId, PlayerId)> {
        race.incidents
            .incidents
            .iter()
            .filter_map(|i| match i.kind {
                IncidentKind::UnsafeRejoin { other, .. } => Some((i.car, other)),
                _ => None,
            })
            .collect()
    };
    for (arriving, rule) in [(true, true), (true, false), (false, true)] {
        let mut race = race();
        race.set_race_rules(RaceRules {
            unsafe_rejoin_penalty: rule,
            ..Default::default()
        });
        let ids: Vec<PlayerId> = race.session.participants.keys().copied().collect();
        let (b, c) = (ids[1], ids[2]);
        while race.session.state != SessionState::Racing {
            tick(&mut race);
        }
        run(&mut race, 5.0);
        // C off the road long enough to count.
        put(&mut race, c, 12.0);
        run(&mut race, 1.0);
        assert!(race.session.participants[&c].laps.off_track_ticks > 0);
        // Back on the line 40 m ahead of B, at rest; B at 60 m/s (or B
        // parked far behind, nobody coming).
        let track = race.track_config.clone();
        let from = race.session.participants[&b]
            .nearest_centerline_idx
            .unwrap_or(0) as usize;
        let n = track.centerline.len();
        let mut ahead = from;
        let mut covered = 0.0;
        while covered < 40.0 {
            let (p, q) = (&track.centerline[ahead], &track.centerline[(ahead + 1) % n]);
            covered += (q.x - p.x).hypot(q.y - p.y);
            ahead = (ahead + 1) % n;
        }
        let point = track.centerline[ahead].clone();
        {
            let state = race.session.participants.get_mut(&c).unwrap();
            state.pos_x = point.x;
            state.pos_y = point.y;
            state.pos_z = point.z + 0.5;
            state.yaw_rad = point.heading_rad;
            state.vel_x = 0.0;
            state.vel_y = 0.0;
            state.speed_mps = 0.0;
            state.nearest_centerline_idx = None;
            let behind = race.session.participants[&b].track_progress;
            let state = race.session.participants.get_mut(&c).unwrap();
            state.track_progress = behind + covered;
        }
        // B arriving at 60 m/s, or every other car stopped.
        for id in ids.iter().filter(|id| **id != c) {
            let state = race.session.participants.get_mut(id).unwrap();
            let speed = if arriving && *id == b {
                60.0
            } else if arriving {
                state.speed_mps
            } else {
                0.0
            };
            state.vel_x = speed * state.yaw_rad.cos();
            state.vel_y = speed * state.yaw_rad.sin();
            state.speed_mps = speed;
        }
        run(&mut race, 0.3);
        let listed = unsafe_rejoins(&race);
        if arriving {
            assert!(listed.contains(&(c, b)), "{:?}", race.incidents.incidents);
        } else {
            assert!(listed.iter().all(|(car, _)| *car != c), "{listed:?}");
        }
        let given: Vec<_> = race.penalties.get(&c).map_or(vec![], |p| {
            p.iter()
                .filter(|p| p.offence == Offence::UnsafeRejoin)
                .collect()
        });
        if arriving && rule {
            assert_eq!(given.len(), 1, "{:?}", race.penalties);
            assert_eq!(given[0].seconds, UNSAFE_REJOIN_PENALTY_S);
        } else {
            assert!(given.is_empty(), "{given:?}");
        }
    }
}

#[test]
fn an_unsafe_rejoin_is_the_car_that_would_reach_it_soonest() {
    use apexsim_sim::incidents::unsafe_rejoin;
    let (near, far, slow) = (
        uuid::Uuid::from_u128(1),
        uuid::Uuid::from_u128(2),
        uuid::Uuid::from_u128(3),
    );
    // Rejoining at 1000 m going 10 m/s.
    let behind = [(near, 980.0, 40.0), (far, 900.0, 80.0), (slow, 995.0, 15.0)];
    // near reaches it in 20/30 s, far in 100/70 s, slow closes at 5 m/s.
    assert_eq!(unsafe_rejoin(1000.0, 10.0, behind), Some((near, 30.0)));
    // Nobody within reach: far alone needs 100/70 s, under 2 s: listed.
    assert_eq!(
        unsafe_rejoin(1000.0, 10.0, [(far, 900.0, 80.0)]),
        Some((far, 70.0))
    );
    assert_eq!(unsafe_rejoin(1000.0, 10.0, [(far, 800.0, 80.0)]), None);
    // A car ahead of it is no threat.
    assert_eq!(unsafe_rejoin(1000.0, 10.0, [(near, 1010.0, 80.0)]), None);
}

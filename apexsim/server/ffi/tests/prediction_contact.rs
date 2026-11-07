//! A predicting client's car touched by another (follow-ups #42): the
//! server's two-car session, one car rammed from the side by the other, and
//! the rammed car replayed through the C API from the server's state a
//! round trip before the contact, as a client replays a snapshot. Alone
//! (the library before ABI 9) it misses the hit and lands far from the
//! server; with the other car handed over where the snapshot had it
//! (`apx_car_set_others`), carried on at its speed, it lands close.

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::path::PathBuf;

use apexsim_physics::*;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::car_step;
use apexsim_sim::data::*;
use apexsim_sim::game_session::{GameSession, DEFAULT_TICK_RATE_HZ};
use apexsim_sim::physics::ContactBox;
use apexsim_sim::track_loader::RoadContactMode;
use apexsim_sim::track_loader::TrackLoader;
use uuid::Uuid;

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Monzarella/Monzarella.yaml";
const CAR: &str = "../apexsim-content/cars/default/fugazzi-sf26/car.toml";

/// The round trip a replay reaches back: 80 ms at 420 Hz.
const LEAD_TICKS: u32 = 34;

fn ok(status: ApxStatus) {
    let error = unsafe { CStr::from_ptr(apx_last_error()) }.to_string_lossy();
    assert_eq!(status, APX_OK, "{error}");
}

fn to_c(input: &PlayerInputData) -> ApxInput {
    ApxInput {
        throttle: input.throttle,
        brake: input.brake,
        steering: input.steering,
        has_gear: 0,
        has_clutch: 0,
        headlights: -1,
        ers_mode: -1,
        ..ApxInput::default()
    }
}

fn input(throttle: f32, steering: f32) -> PlayerInputData {
    PlayerInputData {
        throttle,
        brake: 0.0,
        steering,
        gear: None,
        clutch: None,
        drs: false,
        headlights: None,
        flash: false,
        ers_mode: None,
        ers_boost: false,
        handbrake: false,
    }
}

/// One server tick of the recording: the rammed car's state after it, the
/// input it held, and the other car as a client would hand it over.
struct Tick {
    state: CarState,
    input: PlayerInputData,
    other: ApxOtherCar,
}

fn other_car(state: &CarState, config: &CarConfig, tick: u32) -> ApxOtherCar {
    let b = ContactBox::of(state, config);
    ApxOtherCar {
        tick,
        pos: [b.pos_x, b.pos_y, b.pos_z],
        yaw_rad: b.yaw_rad,
        vel: [b.vel_x, b.vel_y],
        yaw_rate_rps: state.angular_vel_yaw,
        length_m: b.length_m,
        width_m: b.width_m,
        height_m: b.height_m,
        mass_kg: b.mass_kg,
    }
}

#[test]
fn a_replay_meets_the_other_car_where_the_server_did() {
    let mut track = TrackLoader::load_from_file_with(repo(TRACK), RoadContactMode::Centerline)
        .expect("Monza loads");
    let conditions = SessionConditions::default()
        .at_site(&track)
        .resolve_at(1, track.climate_site());
    conditions.apply_to_track(&mut track);
    let car = CarLoader::load_from_file(&repo(CAR)).expect("the car loads");
    let (rammed, rammer) = (Uuid::from_u128(1), Uuid::from_u128(2));
    let mut race = RaceSession::new(rammed, track.id, SessionKind::Practice, 8, 0, 2);
    race.id = Uuid::from_u128(3);
    race.conditions = conditions;
    let mut cars = HashMap::new();
    cars.insert(car.id, car.clone());
    let mut session = GameSession::new(race, track, cars);
    session.set_tick_rate(DEFAULT_TICK_RATE_HZ);
    session.add_player(rammed, car.id).unwrap();
    session.add_player(rammer, car.id).unwrap();
    let aids = DriverAids {
        auto_gearbox: true,
        steering_assist: true,
        abs: Some(true),
        traction_control: Some(TractionControl::High),
        pit_autopilot: None,
    };
    session.set_driver_aids(&rammed, aids);
    session.set_driver_aids(&rammer, aids);
    session.set_game_mode(GameMode::FreePractice);

    // The rammed car drives up the straight; the other launches harder and
    // aims at it.
    let start = session.session.participants[&rammed].clone();
    let mut recording: Vec<Tick> = vec![Tick {
        state: start.clone(),
        input: input(0.0, 0.0),
        other: other_car(&session.session.participants[&rammer], &car, 0),
    }];
    let mut first_contact = None;
    for t in 1..=2400u32 {
        let (a, b) = (
            &session.session.participants[&rammed],
            &session.session.participants[&rammer],
        );
        let bearing = (a.pos_y - b.pos_y).atan2(a.pos_x - b.pos_x);
        let off = (bearing - b.yaw_rad + std::f32::consts::PI)
            .rem_euclid(2.0 * std::f32::consts::PI)
            - std::f32::consts::PI;
        let held = input(0.55, 0.0);
        let mut inputs = HashMap::new();
        inputs.insert(rammed, held);
        inputs.insert(rammer, input(1.0, (off * 3.0).clamp(-0.6, 0.6)));
        session.tick(&inputs);
        let tick = session.session.current_tick;
        let a = &session.session.participants[&rammed];
        if a.is_colliding && first_contact.is_none() && t > 60 {
            first_contact = Some(t);
        }
        recording.push(Tick {
            state: a.clone(),
            input: held,
            other: other_car(&session.session.participants[&rammer], &car, tick),
        });
        if first_contact.is_some_and(|c| t > c + LEAD_TICKS + 10) {
            break;
        }
    }
    let contact = first_contact.expect("the rammer reaches the rammed car");

    let path = CString::new(repo(TRACK).to_str().unwrap()).unwrap();
    let cond = rmp_serde::to_vec_named(&conditions).unwrap();
    let toml = std::fs::read(repo(CAR)).unwrap();
    let (mut ffi_track, mut ffi_car) = (std::ptr::null_mut(), std::ptr::null_mut());
    unsafe {
        ok(apx_track_load(
            path.as_ptr(),
            0,
            cond.as_ptr(),
            cond.len(),
            &mut ffi_track,
        ));
        ok(apx_car_create(
            ffi_track,
            toml.as_ptr(),
            toml.len(),
            std::ptr::null(),
            0,
            1,
            &mut ffi_car,
        ));
    }
    // The shape a client reads off a car of the other's model.
    let mut shape = ApxCarShape::default();
    unsafe { ok(apx_car_shape(ffi_car, &mut shape)) };
    assert_eq!(shape.length_m, car.length_m);
    assert_eq!(shape.width_m, car.width_m);
    assert!(shape.mass_kg > car.mass_kg, "the fuel counts");

    // Replays from server states before the contact, each LEAD_TICKS on.
    let replay = |from: u32, others: bool| -> f32 {
        let blob = car_step::encode_state(&recording[from as usize].state);
        unsafe {
            ok(apx_car_state_load(ffi_car, blob.as_ptr(), blob.len()));
            let other = recording[from as usize].other;
            ok(apx_car_set_others(
                ffi_car,
                &other,
                if others { 1 } else { 0 },
            ));
        }
        for t in from + 1..=from + LEAD_TICKS {
            let c = to_c(&recording[t as usize].input);
            unsafe {
                ok(apx_car_step(
                    ffi_car,
                    ffi_track,
                    &c,
                    t,
                    DEFAULT_TICK_RATE_HZ,
                ))
            };
        }
        let mut pose = ApxCarPose::default();
        unsafe { ok(apx_car_pose(ffi_car, &mut pose)) };
        let server = &recording[(from + LEAD_TICKS) as usize].state;
        ((pose.pos_x - server.pos_x).powi(2) + (pose.pos_y - server.pos_y).powi(2)).sqrt()
    };
    let (mut alone_worst, mut among_worst) = (0.0f32, 0.0f32);
    for from in (contact.saturating_sub(LEAD_TICKS)..contact).step_by(1) {
        let (alone, among) = (replay(from, false), replay(from, true));
        eprintln!("from tick {from} (contact at {contact}): alone {alone:.3} m, with the other car {among:.3} m");
        alone_worst = alone_worst.max(alone);
        among_worst = among_worst.max(among);
    }
    unsafe {
        apx_car_free(ffi_car);
        apx_track_free(ffi_track);
    }
    eprintln!("worst: alone {alone_worst:.3} m, with the other car {among_worst:.3} m");
    assert!(
        alone_worst > 0.05,
        "the contact moves the car: {alone_worst} m"
    );
    assert!(
        among_worst < alone_worst * 0.35,
        "with the other car the replay lands near the server: {among_worst} m against {alone_worst} m alone"
    );
}

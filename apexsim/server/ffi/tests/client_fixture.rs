//! The recorded server sequence the client's prediction tests replay
//! (game-unreal/Source/ApexSim/Private/Tests/ApexPredictionFixture.h,
//! `ApexSim.Prediction.*`): a free-practice session at Monza, one car with
//! aids and a tuned setup, driven by 60 Hz numbered inputs as a client sends
//! them (each held for the ticks between two), with the car's full state
//! blob at the window's start and its `OwnCarSnapshot` every telemetry tick
//! after, each naming the input it had applied.
//!
//! The track is the YAML alone (copied to a scratch folder, no sidecars), on
//! the centerline, so a fresh clone and CI record and replay the same.
//! Regenerate after a physics change or an edit to Monzarella.yaml or the car:
//!
//! ```text
//! cd server/ffi
//! cargo test --test client_fixture -- --ignored --nocapture
//! ```

use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::PathBuf;

use apexsim_physics::*;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::car_setup::{CarSetup, KNOB_COUNT};
use apexsim_sim::car_step;
use apexsim_sim::content_crc::content_crc;
use apexsim_sim::data::*;
use apexsim_sim::game_session::{GameSession, DEFAULT_TICK_RATE_HZ};
use apexsim_sim::own_car::OwnCarSnapshot;
use apexsim_sim::track_loader::RoadContactMode;
use apexsim_sim::track_loader::TrackLoader;
use uuid::Uuid;

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

/// Relative to the content root, as a session's `TrackFile` names it.
const TRACK: &str = "tracks/default/Monzarella/Monzarella.yaml";
const CAR: &str = "fugazzi-sf26";
const OUT: &str = "game-unreal/Source/ApexSim/Private/Tests/ApexPredictionFixture.h";

/// Ticks an input is held: 60 Hz of inputs at 420 Hz of ticks.
const TICKS_PER_INPUT: u32 = 7;
/// The window recorded: two seconds once the car is at speed.
const WINDOW_START: u32 = 1260;
const WINDOW_TICKS: u32 = 840;

fn setup_clicks() -> [i8; KNOB_COUNT] {
    let mut c = [0i8; KNOB_COUNT];
    c[0] = 2;
    c[1] = -1;
    c[4] = 1;
    c[7] = -2;
    c[15] = -2;
    c[16] = 1;
    c[21] = -1;
    c
}

/// Input `seq` (from 1): a launch, then weaving with the throttle breathing
/// and a lift and brake late in the window, as a driver's hands would be.
fn input_for(seq: u32) -> PlayerInputData {
    let t = ((seq - 1) * TICKS_PER_INPUT) as f32;
    let braking = (1820..1960).contains(&((seq - 1) * TICKS_PER_INPUT));
    PlayerInputData {
        throttle: if braking {
            0.0
        } else {
            (t / 200.0).min(1.0) * (0.8 + 0.2 * (t * 0.013).sin())
        },
        brake: if braking { 0.8 } else { 0.0 },
        steering: 0.07 * (t * 0.005).sin() + 0.03 * (t * 0.029).sin(),
        gear: None,
        clutch: None,
        drs: false,
        headlights: None,
        flash: false,
        ers_mode: None,
        ers_boost: (1400..1600).contains(&((seq - 1) * TICKS_PER_INPUT)),
        handbrake: false,
    }
}

struct Writer(Vec<u8>);

impl Writer {
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn i8(&mut self, v: i8) {
        self.0.push(v as u8);
    }
    fn u16(&mut self, v: u16) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn u32(&mut self, v: u32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn f32(&mut self, v: f32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn f32s(&mut self, v: &[f32]) {
        for &x in v {
            self.f32(x);
        }
    }
    fn bytes(&mut self, v: &[u8]) {
        self.u32(v.len() as u32);
        self.0.extend_from_slice(v);
    }
    fn input(&mut self, i: &ApxInput) {
        self.f32s(&[i.throttle, i.brake, i.steering, i.clutch]);
        for b in [
            i.gear as u8,
            i.has_gear,
            i.has_clutch,
            i.drs,
            i.headlights as u8,
            i.flash,
            i.ers_mode as u8,
            i.ers_boost,
            i.handbrake,
        ] {
            self.u8(b);
        }
    }
    fn snapshot(&mut self, s: &ApxCarSnapshot) {
        self.f32s(&s.pos);
        self.f32s(&s.attitude);
        self.f32s(&s.vel);
        self.f32(s.speed_mps);
        self.f32s(&s.angular_vel);
        self.f32s(&s.wheel_angular_vel);
        self.f32s(&s.wheel_load_n);
        self.f32s(&s.suspension_travel_m);
        self.f32s(&s.suspension_velocity_mps);
        self.f32s(&s.g_forces);
        self.f32(s.vertical_accel_mps2);
        self.f32s(&s.ground_slope);
        self.f32s(&s.porpoise);
        self.f32s(&s.controls);
        self.f32(s.engine_rpm);
        self.f32(s.turbo_spool);
        self.f32(s.track_progress);
        self.f32(s.lateral_offset_m);
        self.u32(s.centerline_idx);
        self.u16(s.auto_shift_hold_ticks);
        self.u16(s.auto_reverse_ticks);
        self.u16(s.current_lap);
        self.i8(s.gear);
        self.u8(s.flags);
    }
}

fn to_c(input: &PlayerInputData) -> ApxInput {
    ApxInput {
        throttle: input.throttle,
        brake: input.brake,
        steering: input.steering,
        clutch: input.clutch.unwrap_or(0.0),
        gear: input.gear.unwrap_or(0),
        has_gear: input.gear.is_some() as u8,
        has_clutch: input.clutch.is_some() as u8,
        drs: input.drs as u8,
        headlights: input.headlights.map_or(-1, |on| on as i8),
        flash: input.flash as u8,
        ers_mode: input.ers_mode.map_or(-1, |m| m as i8),
        ers_boost: input.ers_boost as u8,
        handbrake: input.handbrake as u8,
    }
}

#[test]
#[ignore = "writes the client's prediction fixture; run after a physics or Monza/car change"]
fn print_client_prediction_fixture() {
    // The YAML alone, as CI and a fresh clone have it.
    let scratch = std::env::temp_dir().join(format!("apexsim-fixture-{}", std::process::id()));
    std::fs::create_dir_all(&scratch).unwrap();
    let yaml_bytes = std::fs::read(repo("../apexsim-content").join(TRACK)).unwrap();
    let yaml = scratch.join("Monzarella.yaml");
    std::fs::write(&yaml, &yaml_bytes).unwrap();
    let toml_path = repo("../apexsim-content/cars/default")
        .join(CAR)
        .join("car.toml");
    let toml = std::fs::read(&toml_path).unwrap();

    let mut track = TrackLoader::load_from_file_with(&yaml, RoadContactMode::Centerline).unwrap();
    let asked = SessionConditions {
        wind_kph: Some(20),
        wind_from_deg: Some(200),
        ..SessionConditions::default()
    };
    let conditions = asked
        .at_site(&track)
        .resolve_at(asked.sky_seed_or(3), track.climate_site());
    conditions.apply_to_track(&mut track);
    let car = CarLoader::load_from_file(&toml_path).unwrap();
    let human = Uuid::from_u128(0xC0FFEE);
    let mut race = RaceSession::new(human, track.id, SessionKind::Practice, 8, 0, 2);
    race.id = Uuid::from_u128(2);
    race.conditions = conditions;
    let mut cars = HashMap::new();
    cars.insert(car.id, car.clone());
    let mut session = GameSession::new(race, track, cars);
    session.set_tick_rate(DEFAULT_TICK_RATE_HZ);
    let grid = session.add_player(human, car.id).unwrap();
    session.set_car_setup(&human, CarSetup::from_clicks(setup_clicks()));
    session.set_driver_aids(
        &human,
        DriverAids {
            auto_gearbox: true,
            steering_assist: true,
            abs: Some(true),
            traction_control: Some(TractionControl::High),
            pit_autopilot: None,
        },
    );
    session.set_game_mode(GameMode::FreePractice);

    let mut w = Writer(Vec::new());
    w.0.extend_from_slice(b"APXF");
    w.u32(2);
    w.u16(DEFAULT_TICK_RATE_HZ);
    w.u8(0); // the centerline
    w.u8(grid);
    w.u32(content_crc(&yaml_bytes));
    w.u32(content_crc(&toml));
    w.bytes(&rmp_serde::to_vec_named(&conditions).unwrap());
    w.bytes(&setup_clicks().map(|c| c as u8));
    w.bytes(CAR.as_bytes());
    w.bytes(TRACK.as_bytes());

    let mut inputs = Vec::new();
    let mut snapshots = Vec::new();
    let mut feedback = Vec::new();
    let mut blob = None;
    let (mut top_speed, mut first_tick) = (0.0f32, HashMap::new());
    for t in 1..=WINDOW_START + WINDOW_TICKS {
        let seq = 1 + (t - 1) / TICKS_PER_INPUT;
        let input = input_for(seq);
        first_tick.entry(seq).or_insert(t);
        let mut held = HashMap::new();
        held.insert(human, input);
        session.tick(&held);
        let tick = session.session.current_tick;
        assert_eq!(tick, t, "the session counts the ticks it steps");
        if tick.is_multiple_of(TICKS_PER_INPUT) {
            // The DriverFeedback of this telemetry tick, drained as the game
            // loop drains it.
            let message = session
                .session
                .participants
                .get_mut(&human)
                .unwrap()
                .feedback
                .take(tick);
            if tick > WINDOW_START {
                feedback.push((tick, message));
            }
        }
        let state = &session.session.participants[&human];
        top_speed = top_speed.max(state.speed_mps);
        if tick == WINDOW_START {
            blob = Some((tick, seq, car_step::encode_state(state)));
        }
        if tick > WINDOW_START && tick.is_multiple_of(TICKS_PER_INPUT) {
            snapshots.push((
                tick,
                seq,
                ApxCarSnapshot::from(&OwnCarSnapshot::of(state, tick, Some(seq))),
            ));
        }
    }
    let (blob_tick, blob_seq, blob) = blob.unwrap();
    w.u32(blob_tick);
    w.u32(blob_seq);
    w.bytes(&blob);
    // Every input the window applies, with the tick it first drove.
    let first_seq = blob_seq;
    let last_seq = 1 + (WINDOW_START + WINDOW_TICKS - 1) / TICKS_PER_INPUT;
    w.u32(last_seq - first_seq + 1);
    for seq in first_seq..=last_seq {
        w.u32(seq);
        w.u32(first_tick[&seq]);
        inputs.push(seq);
        w.input(&to_c(&input_for(seq)));
    }
    w.u32(snapshots.len() as u32);
    for (tick, seq, s) in &snapshots {
        w.u32(*tick);
        w.u32(*seq);
        w.snapshot(s);
    }
    // Version 2: the DriverFeedback of every telemetry tick in the window,
    // as the server sends it (its own-car fields left out).
    w.u32(feedback.len() as u32);
    for (tick, f) in &feedback {
        w.u32(*tick);
        w.u32(f.steer_torque.len() as u32);
        w.f32s(&f.steer_torque);
        w.f32s(&f.slip_ratio);
        w.f32s(&f.slip_angle);
        w.f32s(&f.suspension_mps);
        w.f32s(&[
            f.impact_mps,
            f.steer_kick,
            f.steer_input,
            f.steer_stiffness,
            f.front_load,
        ]);
        for b in f.surface.iter().chain(&f.flat_spot) {
            w.u8(*b);
        }
        w.u8(f.abs_active as u8);
        w.u8(f.tc_active as u8);
    }
    assert!(top_speed > 25.0, "the car was at speed ({top_speed} m/s)");

    // How far the library, stepped from the blob with the same inputs on its
    // own copy of the track (the road as loaded, not the session's), lands
    // from the server: the client test's tolerance.
    let path = std::ffi::CString::new(yaml.to_str().unwrap()).unwrap();
    let cond = rmp_serde::to_vec_named(&conditions).unwrap();
    let clicks = setup_clicks();
    let mut worst = 0.0f32;
    unsafe {
        let mut ctrack = std::ptr::null_mut();
        assert_eq!(
            apx_track_load(path.as_ptr(), 0, cond.as_ptr(), cond.len(), &mut ctrack),
            APX_OK
        );
        let mut ccar = std::ptr::null_mut();
        assert_eq!(
            apx_car_create(
                ctrack,
                toml.as_ptr(),
                toml.len(),
                clicks.as_ptr(),
                clicks.len(),
                grid,
                &mut ccar
            ),
            APX_OK
        );
        assert_eq!(apx_car_state_load(ccar, blob.as_ptr(), blob.len()), APX_OK);
        let mut next = 0;
        for t in blob_tick + 1..=WINDOW_START + WINDOW_TICKS {
            let seq = 1 + (t - 1) / TICKS_PER_INPUT;
            let c = to_c(&input_for(seq));
            assert_eq!(
                apx_car_step(ccar, ctrack, &c, t, DEFAULT_TICK_RATE_HZ),
                APX_OK
            );
            if next < snapshots.len() && snapshots[next].0 == t {
                let mut pose = ApxCarPose::default();
                apx_car_pose(ccar, &mut pose);
                let s = &snapshots[next].2;
                let e = ((pose.pos_x - s.pos[0]).powi(2)
                    + (pose.pos_y - s.pos[1]).powi(2)
                    + (pose.pos_z - s.pos[2]).powi(2))
                .sqrt();
                worst = worst.max(e);
                next += 1;
            }
        }
        apx_car_free(ccar);
        apx_track_free(ctrack);
    }
    eprintln!(
        "{} inputs, {} snapshots, blob {} bytes at tick {blob_tick}, top speed {top_speed:.1} m/s; \
         the library from the blob ends {worst:.4} m from the server",
        inputs.len(),
        snapshots.len(),
        blob.len()
    );

    let mut text = String::new();
    writeln!(
        text,
        "#pragma once\n\n// Generated by server/ffi/tests/client_fixture.rs \
         (print_client_prediction_fixture): do not edit.\n\
         // Monza from the YAML alone, the centerline, {CAR}, {} inputs, {} snapshots, {} feedback messages.\n\
         // Regenerate: cd server/ffi && cargo test --test client_fixture -- --ignored --nocapture\n\n\
         #include \"CoreMinimal.h\"\n\nnamespace ApexPredictionFixture\n{{\n\tinline constexpr uint8 Bytes[] = {{",
        inputs.len(),
        snapshots.len(),
        feedback.len()
    )
    .unwrap();
    for chunk in w.0.chunks(24) {
        text.push_str("\t\t");
        for b in chunk {
            write!(text, "0x{b:02X},").unwrap();
        }
        text.push('\n');
    }
    text.push_str("\t};\n}\n");
    std::fs::write(repo(OUT), text.replace('\n', "\r\n")).unwrap();
    let _ = std::fs::remove_dir_all(&scratch);
    eprintln!("wrote {OUT} ({} bytes of fixture)", w.0.len());
}

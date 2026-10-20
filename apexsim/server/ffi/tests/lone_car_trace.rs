//! The C API steps a car exactly as the server's session does: a recorded
//! input trace driven through a `GameSession` (one car, free practice) and
//! through the extern "C" functions gives the same state every tick, bit
//! for bit; and a state saved, stepped on and loaded again replays the same.
//!
//! The track is Monza from the checkout: with its sidecars (walls, curbs,
//! road mesh, pit lane) where they have been baked, from the YAML alone in
//! CI, as the other server tests load it.

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::path::PathBuf;

use apexsim_physics::*;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::car_setup::{CarSetup, KNOB_COUNT};
use apexsim_sim::car_step;
use apexsim_sim::data::*;
use apexsim_sim::game_session::{GameSession, DEFAULT_TICK_RATE_HZ, PUNCTURE_HIT_PCT};
use apexsim_sim::own_car::OwnCarSnapshot;
use apexsim_sim::track_loader::RoadContactMode;
use apexsim_sim::track_loader::TrackLoader;
use uuid::Uuid;

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Monzarella/Monzarella.yaml";
/// A car with DRS, a hybrid and a tuned setup: the most the car model does.
const CAR: &str = "../apexsim-content/cars/default/fugazzi-sf26/car.toml";

/// A setup off stock on knobs the car model reads (pressures, final drive,
/// brake bias, springs, wings, ride height, camber, toe).
fn setup_clicks() -> [i8; KNOB_COUNT] {
    let mut c = [0i8; KNOB_COUNT];
    c[0] = 2;
    c[1] = -1;
    c[4] = 1;
    c[7] = -2;
    c[8] = 1;
    c[15] = -2;
    c[16] = 1;
    c[17] = 1;
    c[21] = -1;
    c[24] = 1;
    c
}

/// Weather with wind, so the tick's gusts reach the car.
fn conditions(track: &TrackConfig) -> SessionConditions {
    let asked = SessionConditions {
        wind_kph: Some(30),
        wind_from_deg: Some(120),
        ..SessionConditions::default()
    };
    asked
        .at_site(track)
        .resolve_at(asked.sky_seed_or(7), track.latitude_deg())
}

/// Ticks in the recorded trace: about ten seconds.
const TRACE_TICKS: u32 = 4400;

/// The recorded trace: a standing start up the straight, shifting up and
/// weaving, a hard stop with a flick of the handbrake, then a launch
/// turning hard left off the road, over the grass and into whatever stands
/// there (manual gears; the aids' run lets the box shift).
fn trace_input(tick: u32, manual: bool) -> PlayerInputData {
    let t = tick as f32;
    let braking = (2600..3000).contains(&tick);
    let off = tick >= 3000;
    let gear = manual.then_some(match tick {
        0..=499 => 1,
        500..=999 => 2,
        1000..=1599 => 3,
        1600..=2199 => 4,
        2200..=2599 => 5,
        2600..=2999 => 2,
        _ => 2,
    });
    PlayerInputData {
        throttle: if braking {
            0.0
        } else {
            (t / 200.0).min(1.0) * (0.85 + 0.15 * (t * 0.011).sin())
        },
        brake: if braking { 0.9 } else { 0.0 },
        steering: if off {
            0.7
        } else {
            0.06 * (t * 0.006).sin() + 0.02 * (t * 0.031).sin()
        },
        gear,
        clutch: None,
        drs: tick > 1200,
        headlights: None,
        flash: false,
        ers_mode: None,
        ers_boost: (1400..2000).contains(&tick),
        handbrake: (2900..2940).contains(&tick),
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

fn last_error() -> String {
    unsafe { CStr::from_ptr(apx_last_error()) }
        .to_string_lossy()
        .into_owned()
}

fn ok(status: ApxStatus) {
    assert_eq!(status, APX_OK, "{}", last_error());
}

/// The fields a session's passes around the car step own (the pit lane,
/// DRS, the wake, the road state's bookkeeping, the hit record debris
/// reads, the headlights): the client holds them at what the server last
/// said, so they are not compared. Nothing else is left out.
fn without_session_fields(mut s: CarState) -> CarState {
    let fresh = CarState::new(
        s.player_id,
        s.car_config_id,
        &GridSlot {
            position: 0,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        },
    );
    s.pit = fresh.pit;
    s.drs_allowed = false;
    s.drs_armed = 0;
    s.drs_last_progress = 0.0;
    s.wake = fresh.wake;
    s.racecraft = fresh.racecraft;
    s.road_cell = 0;
    s.road_work = Default::default();
    s.last_hit_pct = 0.0;
    s.last_hit_angle = 0.0;
    s.headlights = false;
    s.headlight_flash = false;
    // How long a car has been out of the race: the session counts it
    // toward its tow, the lone car step does not.
    s.retired_s = fresh.retired_s;
    s
}

/// Which top-level fields of two states differ, for the failure message.
fn differing_fields(a: &CarState, b: &CarState) -> Vec<String> {
    let a = serde_json::to_value(a).unwrap();
    let b = serde_json::to_value(b).unwrap();
    let (Some(a), Some(b)) = (a.as_object(), b.as_object()) else {
        return vec!["<not objects>".into()];
    };
    a.iter()
        .filter(|(k, v)| b.get(*k) != Some(v))
        .map(|(k, v)| format!("{k}: session {v} ffi {}", b[k]))
        .collect()
}

struct Ffi {
    track: *mut ApxTrack,
    car: *mut ApxCar,
}

impl Ffi {
    fn new(mode: u8, conditions: &SessionConditions, clicks: &[i8]) -> Self {
        let path = CString::new(repo(TRACK).to_str().unwrap()).unwrap();
        let cond = rmp_serde::to_vec_named(conditions).unwrap();
        let toml = std::fs::read(repo(CAR)).unwrap();
        let mut track = std::ptr::null_mut();
        let mut car = std::ptr::null_mut();
        unsafe {
            ok(apx_track_load(
                path.as_ptr(),
                mode,
                cond.as_ptr(),
                cond.len(),
                &mut track,
            ));
            ok(apx_car_create(
                track,
                toml.as_ptr(),
                toml.len(),
                clicks.as_ptr(),
                clicks.len(),
                1,
                &mut car,
            ));
        }
        Self { track, car }
    }

    fn save(&self) -> Vec<u8> {
        let mut size = 0usize;
        unsafe {
            assert_eq!(
                apx_car_state_save(self.car, std::ptr::null_mut(), 0, &mut size),
                APX_ERR_BUFFER
            );
            let mut buf = vec![0u8; size];
            ok(apx_car_state_save(
                self.car,
                buf.as_mut_ptr(),
                buf.len(),
                &mut size,
            ));
            assert_eq!(size, buf.len());
            buf
        }
    }

    fn load(&self, blob: &[u8]) {
        unsafe { ok(apx_car_state_load(self.car, blob.as_ptr(), blob.len())) }
    }

    fn step(&self, input: &PlayerInputData, tick: u32) {
        let c = to_c(input);
        unsafe {
            ok(apx_car_step(
                self.car,
                self.track,
                &c,
                tick,
                DEFAULT_TICK_RATE_HZ,
            ))
        }
    }

    fn pose(&self) -> ApxCarPose {
        let mut pose = ApxCarPose::default();
        unsafe { ok(apx_car_pose(self.car, &mut pose)) };
        pose
    }

    fn feedback(&self) -> ApxFeedback {
        let mut feel = ApxFeedback::default();
        unsafe { ok(apx_car_feedback_take(self.car, &mut feel)) };
        feel
    }
}

impl Drop for Ffi {
    fn drop(&mut self) {
        unsafe {
            apx_car_free(self.car);
            apx_track_free(self.track);
        }
    }
}

/// The server's way: one human in a free-practice session on a track the
/// server prepared (`server::create_session`'s bake), with the setup and
/// aids sent as their messages would.
fn session(
    mode: RoadContactMode,
    aids: Option<DriverAids>,
) -> (GameSession, PlayerId, SessionConditions) {
    let mut track = TrackLoader::load_from_file_with(repo(TRACK), mode).expect("Monza loads");
    let conditions = conditions(&track);
    conditions.apply_to_track(&mut track);
    let car = CarLoader::load_from_file(&repo(CAR)).expect("the car loads");
    let human = Uuid::from_u128(0xC0FFEE);
    let mut race = RaceSession::new(human, track.id, SessionKind::Practice, 8, 0, 2);
    race.id = Uuid::from_u128(2);
    race.conditions = conditions;
    let mut cars = HashMap::new();
    cars.insert(car.id, car.clone());
    let mut session = GameSession::new(race, track, cars);
    session.set_tick_rate(DEFAULT_TICK_RATE_HZ);
    session.add_player(human, car.id).expect("a grid slot");
    session.set_car_setup(&human, CarSetup::from_clicks(setup_clicks()));
    if let Some(aids) = aids {
        session.set_driver_aids(&human, aids);
    }
    session.set_game_mode(GameMode::FreePractice);
    (session, human, conditions)
}

fn run_trace(mode: RoadContactMode, aids: Option<DriverAids>, ticks: u32) {
    let (mut session, human, conditions) = session(mode, aids);
    let ffi_mode = match mode {
        RoadContactMode::Centerline => 0,
        RoadContactMode::Mesh => 1,
    };
    let ffi = Ffi::new(ffi_mode, &conditions, &setup_clicks());
    // The FFI car starts from the session's car, through the blob.
    ffi.load(&car_step::encode_state(
        &session.session.participants[&human],
    ));

    let manual = aids.is_none();
    let (mut top_speed, mut off_track, mut contacts, mut corrections) = (0.0f32, 0, 0, 0);
    let (mut kicks, mut surfaces) = (0u32, 0u32);
    for t in 0..ticks {
        let input = trace_input(t, manual);
        let mut inputs = HashMap::new();
        inputs.insert(human, input);
        // A hard hit last tick is the session's to act on this tick (debris
        // shed, a puncture rolled), as is a piece of debris picked up: race
        // events, which reach a predicting client as a correction.
        let hit_before = session.session.participants[&human].last_hit_pct;
        let driven = session.session.participants[&human].damage.is_drivable;
        let debris_before = session.debris.len();
        session.tick(&inputs);
        let race_event = hit_before >= PUNCTURE_HIT_PCT || session.debris.len() < debris_before;
        let tick = session.session.current_tick;
        // The road's rubber and marbles are the session's world, laid by
        // its passes before the physics: the FFI's track holds what the
        // session's held for this tick, as a client holds the server's.
        unsafe { (*ffi.track).track_mut() }.road_state = session.track_config.road_state.clone();
        ffi.step(&input, tick);

        // What the driver feels: the session's collector drained as the game
        // loop drains it, against the library's, every tick.
        let served_feel = session
            .session
            .participants
            .get_mut(&human)
            .unwrap()
            .feedback
            .take(tick);
        let felt = ffi.feedback();
        let car = &session.session.participants[&human];
        let served = without_session_fields(car.clone());
        let predicted = without_session_fields(car_step::decode_state(&ffi.save()).unwrap());
        if !race_event {
            assert_eq!(
                felt,
                ApxFeedback::from(&served_feel),
                "tick {tick}: the library's feedback left the session's"
            );
            // One sample a tick the car is driven; none once a wall has
            // left it undrivable (the trace runs off the road on purpose).
            assert_eq!(
                felt.steer_samples, driven as u32,
                "tick {tick}: one steering sample a driven tick"
            );
            kicks += (felt.steer_kick != 0.0) as u32;
            surfaces += felt.surface.iter().any(|&s| s != 0) as u32;
        }
        if car_step::encode_state(&served) != car_step::encode_state(&predicted) {
            assert!(
                race_event,
                "tick {tick}: the FFI's car left the session's: {:#?}",
                differing_fields(&served, &predicted)
            );
            ffi.load(&car_step::encode_state(car));
            corrections += 1;
        }
        let pose = ffi.pose();
        assert_eq!(pose.pos_x.to_bits(), car.pos_x.to_bits());
        assert_eq!(pose.yaw_rad.to_bits(), car.yaw_rad.to_bits());
        top_speed = top_speed.max(pose.speed_mps);
        off_track += (pose.flags & APX_POSE_ON_TRACK == 0) as u32;
        contacts += car.is_colliding as u32;
    }
    eprintln!(
        "{ticks} ticks: top speed {top_speed:.1} m/s, {off_track} ticks off the road, \
         {contacts} in contact, {corrections} race-event corrections; the feedback equal every \
         tick, {kicks} with a steering kick, {surfaces} off the road or on a kerb"
    );
    assert!(surfaces > 0, "the feedback saw the trace leave the road");
    assert!(
        top_speed > 40.0,
        "the trace drove the car (top speed {top_speed} m/s)"
    );
    assert!(off_track > 0, "the trace left the road");
}

#[test]
fn the_ffi_steps_a_car_as_the_session_does_on_the_centerline() {
    run_trace(RoadContactMode::Centerline, None, TRACE_TICKS);
}

#[test]
fn the_ffi_steps_a_car_as_the_session_does_on_the_road_mesh_with_aids() {
    // The mesh where Monza's road sidecar is baked; the centerline (and so
    // the same check with the aids) where it is not, as in CI.
    run_trace(
        RoadContactMode::Mesh,
        Some(DriverAids {
            auto_gearbox: true,
            steering_assist: true,
            abs: Some(true),
            traction_control: Some(TractionControl::High),
            pit_autopilot: None,
        }),
        TRACE_TICKS,
    );
}

/// What a rewind needs (docs/proposals/client-prediction.md, phase 2): a
/// car whose whole state is the server's of `STALE` ticks ago, with the
/// server's `OwnCarSnapshot` of now laid over it, steps on beside the
/// server within a millimetre over a ping's worth of ticks; pose and
/// velocities alone drift by centimetres. The slow state the snapshot
/// leaves out (tyres, fuel, hybrid, damage) comes in the blob.
#[test]
fn a_snapshot_over_a_stale_state_replays_with_the_server() {
    /// The blob comes twice a second: a second stale is the worst case
    /// with one lost.
    const STALE: usize = DEFAULT_TICK_RATE_HZ as usize;
    /// 100 ms at 420 Hz.
    const REPLAY: u32 = 42;
    let (mut session, human, conditions) = session(
        RoadContactMode::Mesh,
        Some(DriverAids {
            auto_gearbox: true,
            steering_assist: true,
            abs: Some(true),
            traction_control: Some(TractionControl::High),
            pit_autopilot: None,
        }),
    );
    let ffi = Ffi::new(1, &conditions, &setup_clicks());
    let toml = std::fs::read(repo(CAR)).unwrap();
    let new_car = |state: &CarState| -> *mut ApxCar {
        let mut car = std::ptr::null_mut();
        let clicks = setup_clicks();
        let blob = car_step::encode_state(state);
        unsafe {
            ok(apx_car_create(
                ffi.track,
                toml.as_ptr(),
                toml.len(),
                clicks.as_ptr(),
                clicks.len(),
                1,
                &mut car,
            ));
            ok(apx_car_state_load(car, blob.as_ptr(), blob.len()));
        }
        car
    };
    // The runtime fields a blob carries but the session's passes own are
    // the server's of now in every probe: only the car's own state is
    // stale.
    let stale_with_now = |stale: &CarState, now: &CarState| {
        let mut s = stale.clone();
        s.nearest_centerline_idx = now.nearest_centerline_idx;
        s.next_checkpoint = now.next_checkpoint;
        s.laps = now.laps.clone();
        s.wheels_off_track = now.wheels_off_track;
        s.drs_armed = now.drs_armed;
        s.drs_last_progress = now.drs_last_progress;
        s.road_work = now.road_work;
        s
    };

    let mut history: std::collections::VecDeque<CarState> = Default::default();
    // [exact, pose and velocities, snapshot], and when they end.
    let mut probes: Vec<*mut ApxCar> = Vec::new();
    let mut probe_end = 0;
    let mut worst = [0.0f32; 3];
    let mut rewinds = 0;
    for t in 0..TRACE_TICKS {
        let input = trace_input(t, false);
        let mut inputs = HashMap::new();
        inputs.insert(human, input);
        session.tick(&inputs);
        let tick = session.session.current_tick;
        unsafe { (*ffi.track).track_mut() }.road_state = session.track_config.road_state.clone();
        for &car in &probes {
            let c = to_c(&input);
            unsafe { ok(apx_car_step(car, ffi.track, &c, tick, DEFAULT_TICK_RATE_HZ)) };
        }
        let now = session.session.participants[&human].clone();
        if !probes.is_empty() && tick == probe_end {
            for (i, car) in probes.drain(..).enumerate() {
                let mut pose = ApxCarPose::default();
                unsafe { ok(apx_car_pose(car, &mut pose)) };
                let error = ((pose.pos_x - now.pos_x).powi(2)
                    + (pose.pos_y - now.pos_y).powi(2)
                    + (pose.pos_z - now.pos_z).powi(2))
                .sqrt();
                worst[i] = worst[i].max(error);
                unsafe { apx_car_free(car) };
            }
        }
        history.push_back(now.clone());
        if history.len() > STALE + 1 {
            history.pop_front();
        }
        if t >= 600 && t % 200 == 0 && t + REPLAY < TRACE_TICKS && probes.is_empty() {
            let stale = stale_with_now(&history[0], &now);
            let mut pose_only = stale.clone();
            pose_only.pos_x = now.pos_x;
            pose_only.pos_y = now.pos_y;
            pose_only.pos_z = now.pos_z;
            pose_only.yaw_rad = now.yaw_rad;
            pose_only.pitch_rad = now.pitch_rad;
            pose_only.roll_rad = now.roll_rad;
            pose_only.vel_x = now.vel_x;
            pose_only.vel_y = now.vel_y;
            pose_only.vel_z = now.vel_z;
            pose_only.speed_mps = now.speed_mps;
            pose_only.angular_vel_yaw = now.angular_vel_yaw;
            pose_only.angular_vel_pitch = now.angular_vel_pitch;
            pose_only.angular_vel_roll = now.angular_vel_roll;
            // The snapshot goes on through the C API, as a client lays it.
            let served = ApxCarSnapshot::from(&OwnCarSnapshot::of(&now, tick, None));
            let snapshot = new_car(&stale);
            let mut read = ApxCarSnapshot::default();
            unsafe {
                ok(apx_car_snapshot_apply(snapshot, &served));
                ok(apx_car_snapshot(snapshot, &mut read));
            }
            assert_eq!(read, served, "the snapshot reads back as laid");
            probes = vec![new_car(&now), new_car(&pose_only), snapshot];
            probe_end = tick + REPLAY;
            rewinds += 1;
        }
    }
    eprintln!(
        "{rewinds} rewinds over a {STALE}-tick stale state, {REPLAY} ticks replayed: \
         max error exact {:.4} m, pose and velocities {:.4} m, snapshot {:.4} m",
        worst[0], worst[1], worst[2]
    );
    assert!(rewinds >= 15);
    assert_eq!(worst[0], 0.0, "the full state replays exactly");
    assert!(worst[2] < 0.005, "the snapshot leaves {} m", worst[2]);
    assert!(
        worst[1] > 5.0 * worst[2],
        "pose and velocities alone ({} m) are not enough",
        worst[1]
    );
}

/// The road as the server sends it (`RoadState` rows) held on the
/// library's track: a client's copy of a rubbered, marbled road grips as
/// the server's within the bytes' resolution, where the road it loaded
/// does not.
#[test]
fn the_road_from_the_wire_grips_as_the_servers() {
    use apexsim_sim::road_state::ROW_BYTES;
    let track = TrackLoader::load_from_file(repo(TRACK)).unwrap();
    let conditions = conditions(&track);
    let ffi = Ffi::new(0, &conditions, &setup_clicks());
    let loaded = unsafe { (*ffi.track).track_mut() }
        .road_state
        .clone()
        .expect("the library lays the road");
    // The server's road after a race: rubbered and marbled all over, so
    // the rows carry what the loaded road does not.
    let mut served = loaded.clone();
    for (i, r) in served.rubber.iter_mut().enumerate() {
        *r = (i % 7) as f32 / 7.0;
    }
    for (i, m) in served.marbles.iter_mut().enumerate() {
        *m = (i % 5) as f32 / 10.0;
    }
    let n = served.cell_count();
    let mut rows = Vec::new();
    served.write_rows(0, n, &mut rows);

    // In two slices, the second running to the end of the lap.
    let half = n / 2;
    let none = std::ptr::null();
    unsafe {
        ok(apx_track_road_rows(
            ffi.track,
            0,
            rows.as_ptr(),
            half * ROW_BYTES,
            none,
            0,
            none,
            0,
        ));
        let rest = &rows[half * ROW_BYTES..];
        ok(apx_track_road_rows(
            ffi.track,
            half as u32,
            rest.as_ptr(),
            rest.len(),
            none,
            0,
            none,
            0,
        ));
    }
    let copy = unsafe { (*ffi.track).track_mut() }
        .road_state
        .clone()
        .unwrap();
    let (mut worst, mut loaded_worst) = (0.0f32, 0.0f32);
    for k in 0..200 {
        let station = k as f32 * served.lap_m / 200.0;
        for lateral in [-4.0, -1.0, 0.0, 1.5, 3.0] {
            let s = served.sample(station, lateral).grip;
            worst = worst.max((copy.sample(station, lateral).grip - s).abs());
            loaded_worst = loaded_worst.max((loaded.sample(station, lateral).grip - s).abs());
        }
    }
    eprintln!("grip off the server's: from the wire {worst:.5}, as loaded {loaded_worst:.5}");
    assert!(worst < 0.002, "{worst}");
    assert!(loaded_worst > 10.0 * worst);

    // Bytes that are not whole cells are refused and change nothing.
    unsafe {
        assert_eq!(
            apx_track_road_rows(ffi.track, 0, rows.as_ptr(), 5, none, 0, none, 0),
            APX_ERR_INVALID
        );
        assert!(!last_error().is_empty());
        assert!((*ffi.track).track_mut().road_state.as_ref() == Some(&copy));
    }
}

#[test]
fn a_saved_state_replays_bit_for_bit() {
    let track = TrackLoader::load_from_file(repo(TRACK)).unwrap();
    let ffi = Ffi::new(0, &conditions(&track), &setup_clicks());
    let rewind_at = 500;
    let replay = 400;
    for t in 0..rewind_at {
        ffi.step(&trace_input(t, true), t + 1);
    }
    let saved = ffi.save();
    // The blob round-trips to itself.
    ffi.load(&saved);
    assert_eq!(ffi.save(), saved);

    let mut first = Vec::new();
    for t in rewind_at..rewind_at + replay {
        ffi.step(&trace_input(t, true), t + 1);
        first.push(ffi.save());
    }
    ffi.load(&saved);
    for (i, t) in (rewind_at..rewind_at + replay).enumerate() {
        ffi.step(&trace_input(t, true), t + 1);
        assert!(
            ffi.save() == first[i],
            "replay left the first run at tick {}",
            t + 1
        );
    }
    assert!(ffi.pose().speed_mps > 10.0);
}

#[test]
fn failures_come_back_as_codes() {
    unsafe {
        let mut track = std::ptr::null_mut();
        let missing = CString::new("no/such/track.yaml").unwrap();
        assert_eq!(
            apx_track_load(missing.as_ptr(), 0, std::ptr::null(), 0, &mut track),
            APX_ERR_TRACK
        );
        assert!(track.is_null());
        assert!(!last_error().is_empty());
        assert_eq!(
            apx_track_load(std::ptr::null(), 0, std::ptr::null(), 0, &mut track),
            APX_ERR_NULL
        );

        let path = CString::new(repo(TRACK).to_str().unwrap()).unwrap();
        ok(apx_track_load(
            path.as_ptr(),
            0,
            std::ptr::null(),
            0,
            &mut track,
        ));
        let mut car = std::ptr::null_mut();
        let junk = b"this is not a car";
        assert_eq!(
            apx_car_create(
                track,
                junk.as_ptr(),
                junk.len(),
                std::ptr::null(),
                0,
                1,
                &mut car
            ),
            APX_ERR_CAR
        );
        let toml = std::fs::read(repo(CAR)).unwrap();
        ok(apx_car_create(
            track,
            toml.as_ptr(),
            toml.len(),
            std::ptr::null(),
            0,
            0,
            &mut car,
        ));

        let nan = ApxInput {
            throttle: f32::NAN,
            ..Default::default()
        };
        assert_eq!(
            apx_car_step(car, track, &nan, 1, DEFAULT_TICK_RATE_HZ),
            APX_ERR_INVALID
        );
        assert_eq!(apx_car_step(car, track, &nan, 1, 0), APX_ERR_INVALID);
        let mut snapshot = ApxCarSnapshot::default();
        ok(apx_car_snapshot(car, &mut snapshot));
        let before = snapshot;
        snapshot.vel[1] = f32::NAN;
        assert_eq!(apx_car_snapshot_apply(car, &snapshot), APX_ERR_INVALID);
        let mut after = ApxCarSnapshot::default();
        ok(apx_car_snapshot(car, &mut after));
        assert_eq!(after, before, "a refused snapshot leaves the car");
        let odd = [1u8; 7];
        assert_eq!(
            apx_track_road_rows(
                track,
                0,
                odd.as_ptr(),
                odd.len(),
                std::ptr::null(),
                0,
                std::ptr::null(),
                0
            ),
            APX_ERR_INVALID
        );
        let bad = b"APXC\xff\xffjunk";
        assert_eq!(
            apx_car_state_load(car, bad.as_ptr(), bad.len()),
            APX_ERR_STATE
        );
        assert_eq!(
            apx_car_state_load(car, junk.as_ptr(), junk.len()),
            APX_ERR_STATE
        );

        apx_car_free(car);
        apx_track_free(track);
        assert_eq!(apx_abi_version(), APX_ABI_VERSION);
    }
}

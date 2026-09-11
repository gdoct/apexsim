//! The racing line built from real content: a circuit's measured raceline and
//! a shipped car. Checks the braking zones land where a driver would brake.

use apexsim_server::car_loader::CarLoader;
use apexsim_server::racing_line::{self, LinePhase, RacingLineProfile};
use apexsim_server::track_loader::TrackLoader;
use std::path::Path;

/// (start station m, length m, entry km/h, exit km/h) of every braking zone,
/// with stations measured from the line's first point.
fn braking_zones(profile: &RacingLineProfile) -> Vec<(f32, f32, f32, f32)> {
    let n = profile.phases.len();
    let is_brake = |i: usize| profile.phases[i % n] == LinePhase::Brake;
    let mut zones = Vec::new();
    for i in 0..n {
        if is_brake(i) && !is_brake(i + n - 1) {
            let mut len = 0;
            while is_brake(i + len) && len < n {
                len += 1;
            }
            zones.push((
                i as f32 * profile.spacing_m,
                len as f32 * profile.spacing_m,
                profile.speed_mps[i] * 3.6,
                profile.speed_mps[(i + len - 1) % n] * 3.6,
            ));
        }
    }
    zones
}

fn profile_for(track: &str, car: &str) -> RacingLineProfile {
    let track = TrackLoader::load_from_file(format!("../content/tracks/real/{track}.yaml"))
        .expect("track loads");
    let car = CarLoader::load_from_file(Path::new(&format!("../content/cars/{car}/car.toml")))
        .expect("car loads");
    racing_line::build(&track, &car).expect("line builds")
}

fn report(name: &str, profile: &RacingLineProfile) {
    let fastest = profile.speed_mps.iter().copied().fold(0.0, f32::max) * 3.6;
    let slowest = profile.speed_mps.iter().copied().fold(f32::MAX, f32::min) * 3.6;
    println!(
        "{name}: {} points, {:.0}..{:.0} km/h",
        profile.points.len(),
        slowest,
        fastest
    );
    for (start, len, entry, exit) in braking_zones(profile) {
        println!("  brake at {start:6.0} m for {len:4.0} m: {entry:3.0} -> {exit:3.0} km/h");
    }
}

#[test]
fn monza_has_its_heavy_braking_zones() {
    let f1 = profile_for("Monza", "2021-f1-fugazzi-sf21");
    report("Monza / SF21", &f1);
    let zones = braking_zones(&f1);

    // Rettifilo, Roggia, both Lesmos, Ascari and Parabolica: six stops, and
    // no phantom zones on the straights.
    assert!(
        (5..=9).contains(&zones.len()),
        "expected Monza's six braking zones, got {}",
        zones.len()
    );
    // The first chicane is the hardest stop of the lap, from well over
    // 300 km/h down to first- or second-gear speed.
    let (_, _, entry, exit) = zones[0];
    assert!(entry > 290.0, "entry to the Rettifilo at {entry} km/h");
    assert!(exit < 130.0, "Rettifilo apex at {exit} km/h");

    // Every point has a phase and the lap is mostly flat out, as Monza is.
    let throttle = f1
        .phases
        .iter()
        .filter(|&&p| p == LinePhase::Throttle)
        .count();
    assert!(throttle as f32 / f1.phases.len() as f32 > 0.6);
}

#[test]
fn a_gt_car_brakes_earlier_than_an_f1_car() {
    let f1 = profile_for("Monza", "2021-f1-fugazzi-sf21");
    let gt = profile_for("Monza", "posh-911gt3");
    report("Monza / 911 GT3", &gt);

    let slowest = |p: &RacingLineProfile| p.speed_mps.iter().copied().fold(f32::MAX, f32::min);
    assert!(slowest(&f1) > slowest(&gt), "the F1 car corners faster");

    // Less grip and weaker brakes: even from a lower top speed the GT car
    // has to start braking for the first chicane well before the F1 car.
    let (f1_start, f1_len, ..) = braking_zones(&f1)[0];
    let (gt_start, gt_len, ..) = braking_zones(&gt)[0];
    assert!(
        gt_start + 40.0 < f1_start,
        "GT brakes at {gt_start} m, F1 at {f1_start} m"
    );
    assert!(gt_len > f1_len, "GT zone {gt_len} m vs F1 {f1_len} m");
}

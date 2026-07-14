//! Scratch diagnostic: watch AI race progress on Monza.
use std::collections::HashMap;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn fixed_uuid(n: u128) -> Uuid {
    Uuid::from_u128(n)
}

fn main() {
    let track = TrackLoader::load_from_file("../content/tracks/real/Monza.yaml").unwrap();
    println!(
        "track: centerline={} raceline={} raceline_dist={} checkpoints={:?} length={:.1}",
        track.centerline.len(),
        track.raceline.len(),
        track.raceline_distances.len(),
        track.checkpoints,
        track.centerline.last().unwrap().distance_from_start_m
    );

    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let skills = [105u8, 96, 88, 80];
    let ai_profiles: Vec<AiDriverProfile> = skills
        .iter()
        .enumerate()
        .map(|(i, &skill)| {
            let mut p = AiDriverProfile::new(format!("Race AI {}", i), skill);
            p.id = fixed_uuid(2000 + i as u128);
            p
        })
        .collect();
    let ai_ids: Vec<PlayerId> = ai_profiles.iter().map(|p| p.id).collect();

    let session = RaceSession::new(fixed_uuid(1), track.id, SessionKind::Multiplayer, 8, 4, 2);
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(3, GameMode::Race);

    let max_ticks = 240u32 * 900;
    for tick in 0..max_ticks {
        let mut inputs = HashMap::new();
        for ai_id in &ai_ids {
            inputs.insert(*ai_id, gs.generate_ai_input(ai_id));
        }
        gs.tick(&inputs);

        if tick % (240 * 30) == 0 {
            println!(
                "--- t={}s mode={:?} state={:?}",
                tick / 240,
                gs.session.game_mode,
                gs.session.state
            );
            for (i, ai_id) in ai_ids.iter().enumerate() {
                let s = &gs.session.participants[ai_id];
                println!(
                    "  ai{} skill={} pos=({:.0},{:.0}) v={:.1} lap={} cp={} prog={:.0} on_track={} drivable={} dmg_f={:.0} colliding={} fin={:?}",
                    i, skills[i], s.pos_x, s.pos_y, s.speed_mps, s.current_lap,
                    s.next_checkpoint, s.track_progress, s.is_on_track,
                    s.damage.is_drivable, s.damage.front_damage_percent, s.is_colliding, s.finish_position
                );
            }
        }
        if gs.session.state == SessionState::Finished {
            println!("FINISHED at t={}s", tick / 240);
            break;
        }
    }
    for (i, ai_id) in ai_ids.iter().enumerate() {
        let s = &gs.session.participants[ai_id];
        println!(
            "final ai{}: lap={} last={:?} best={:?} fin={:?}",
            i, s.current_lap, s.last_lap_time_ms, s.best_lap_time_ms, s.finish_position
        );
    }
}

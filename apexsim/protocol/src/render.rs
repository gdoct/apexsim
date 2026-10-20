//! The render of a seeded AI race into a spectator stream: what
//! `apexsim-replay render` writes as a showcase and the physics library
//! writes for the client (`apx_showcase_render`). The race is the
//! simulation's own (`seeded_race`), so the same options and seed give the
//! same bytes in either build.

use std::collections::HashMap;
use std::path::Path;

use crate::data::*;
use crate::seeded_race::{start_race, CarMap, FolderMap, SimulateOptions, StartedRace};
use crate::spectator::{RaceScore, ScoreKeeper, StreamContent, StreamRender};

/// Ticks between two recorded frames for a wanted frame rate: the nearest
/// whole number. Truncating gave 400 / 60 = 6 (66.7 fps) and the header
/// then claimed 60.
pub fn frame_spacing(tick_rate: u16, frame_hz: u16) -> u32 {
    let tick_rate = tick_rate.max(1);
    (tick_rate as f32 / frame_hz.clamp(1, tick_rate) as f32)
        .round()
        .max(1.0) as u32
}

/// The frame rate a spacing really gives, for a header.
pub fn frames_per_second(tick_rate: u16, every: u32) -> u16 {
    (tick_rate.max(1) as f32 / every.max(1) as f32).round() as u16
}

/// Which content a track's YAML is beside: the file stem the client's level
/// and catalog are named by.
pub fn track_stem(track_path: &Path) -> Option<String> {
    track_path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
}

/// A showcase race to render (`apexsim-replay render`).
#[derive(Debug, Clone)]
pub struct RenderOptions {
    /// The race itself. `record_hz` is the stream's frame rate, `laps` the
    /// laps from green, `countdown_seconds` how long the grid stands.
    pub race: SimulateOptions,
    /// Seconds kept after the winner takes the flag.
    pub tail_seconds: f32,
    /// Keep only this window of the race, in ticks.
    pub from_tick: Option<u32>,
    pub to_tick: Option<u32>,
}

impl Default for RenderOptions {
    fn default() -> Self {
        Self {
            race: SimulateOptions {
                ai_count: 20,
                laps: 2,
                countdown_seconds: 8,
                record_hz: 30,
                ..SimulateOptions::default()
            },
            tail_seconds: 10.0,
            from_tick: None,
            to_tick: None,
        }
    }
}

/// A rendered race and how it went.
#[derive(Debug, Clone)]
pub struct RenderedStream {
    pub content: StreamContent,
    pub score: RaceScore,
    pub seed: Option<u64>,
}

/// The first car of a class by folder name, for `render --class`.
pub fn car_of_class(cars: &CarMap, folders: &FolderMap, class: &str) -> Option<CarConfigId> {
    let mut matches: Vec<(&str, CarConfigId)> = cars
        .values()
        .filter(|car| car.class.trim().eq_ignore_ascii_case(class.trim()))
        .map(|car| (folders.get(&car.id).map_or("", |f| f.as_str()), car.id))
        .collect();
    matches.sort();
    matches.first().map(|(_, id)| *id)
}

/// Simulate an AI race and return it as a spectator stream: the grid and
/// its countdown, `laps` from green, and a tail after the winner's flag.
/// The same options and seed give the same bytes.
pub fn render_stream(opts: &RenderOptions) -> Result<RenderedStream, String> {
    let race_opts = &opts.race;
    let StartedRace {
        mut race,
        track_length_m,
        ..
    } = start_race(race_opts)?;
    let stem = track_stem(&race_opts.track_path).unwrap_or_default();

    let names = HashMap::new();
    let mut stream = crate::session_stream::SessionStream::new(&race, race_opts.record_hz, &names);
    let mut header = stream.header(&race, &stem);
    let roster = stream.roster().clone();
    let path = stream.path(&race);
    let sectors = stream.encoder.sectors(&race);

    let tick_rate = race_opts.tick_rate.max(1);
    let frame_dt = stream.record_every() as f32 / tick_rate as f32;
    let max_ticks = (race_opts.max_seconds.max(1.0) * tick_rate as f32) as u32
        + race_opts.countdown_seconds as u32 * tick_rate as u32;
    let tail_ticks = (opts.tail_seconds.max(0.0) * tick_rate as f32) as u32;

    let mut records: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut keeper = ScoreKeeper::default();
    let mut won_at: Option<u32> = None;
    loop {
        crate::seeded_race::tick_field(&mut race);
        let tick = race.session.current_tick;
        let laps = race.take_lap_events();
        let pit_starts = race.take_pit_events();
        let out = stream.tick(&race, &laps, &pit_starts, &names, false);
        if let Some(rows) = &out.rows {
            keeper.frame(
                rows,
                race.session.state == SessionState::Racing,
                frame_dt,
                track_length_m,
            );
        }
        for record in out.records {
            records.push((record.tick, record.body));
        }
        if won_at.is_none()
            && (race.session.state == SessionState::Finished
                || race
                    .session
                    .participants
                    .values()
                    .any(|c| c.finish_position == Some(1)))
        {
            won_at = Some(tick);
        }
        if won_at.is_some_and(|w| tick >= w + tail_ticks) || tick >= max_ticks {
            break;
        }
    }

    if opts.from_tick.is_some() || opts.to_tick.is_some() {
        let from = opts.from_tick.unwrap_or(0);
        let to = opts.to_tick.unwrap_or(u32::MAX);
        records.retain(|(tick, _)| *tick >= from && *tick <= to);
    }
    let (Some(first), Some(last)) = (records.first(), records.last()) else {
        return Err("the window holds no frames".to_string());
    };
    header.start_tick = first.0;
    header.end_tick = last.0;
    header.race_start_tick = race.session.race_start_tick;
    // What is being watched, not the countdown the stream opens in.
    header.game_mode = race.session.game_mode;
    header.render = StreamRender {
        seed: race_opts.seed,
        score: None,
    };
    Ok(RenderedStream {
        content: StreamContent {
            header,
            roster,
            path: Some(path),
            preamble: vec![sectors],
            records,
        },
        score: keeper.finish(),
        seed: race_opts.seed,
    })
}

/// Render `count` seeds from `opts.race.seed` (0 when unset) upward and
/// keep the race that scores best ([`RaceScore::score`]); its seed and
/// score go into the header, so the pick can be rendered again alone.
pub fn render_best(opts: &RenderOptions, count: u32) -> Result<RenderedStream, String> {
    let first = opts.race.seed.unwrap_or(0);
    let mut best: Option<RenderedStream> = None;
    for seed in first..first + count.max(1) as u64 {
        let mut one = opts.clone();
        one.race.seed = Some(seed);
        let rendered = render_stream(&one)?;
        eprintln!(
            "seed {seed}: score {:.1} ({} retired, {:.1} s contact, {:.1} s off the road, {:.1} s close)",
            rendered.score.score(),
            rendered.score.retirements,
            rendered.score.contact_s,
            rendered.score.off_road_s,
            rendered.score.close_s
        );
        if best
            .as_ref()
            .is_none_or(|b| rendered.score.score() > b.score.score())
        {
            best = Some(rendered);
        }
    }
    let mut best = best.expect("at least one seed is rendered");
    if count > 1 {
        best.content.header.render.score = Some(best.score.score());
    }
    Ok(best)
}

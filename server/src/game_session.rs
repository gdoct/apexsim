use crate::ai_driver::{AiDriverController, AiDriverProfile, TrafficCar};
use crate::car_setup::CarSetup;
use crate::data::*;
use crate::laps::{LapEvent, SECTOR_COUNT};
use crate::network::*;
use crate::physics;
use crate::racing_line::{self, RacingLineProfile};
use crate::records::{GhostLap, GhostSample, GHOST_SAMPLE_HZ};
use std::collections::HashMap;
use tracing::debug;

/// Default simulation tick rate; used when no explicit rate is configured
/// (tests, benches) so behavior matches the historical hardcoded 240Hz.
pub const DEFAULT_TICK_RATE_HZ: u16 = 240;

/// Once the winner has finished, the rest of the field has this many of the
/// winner's average laps to complete the distance...
pub const FINISH_GRACE_LAPS: f32 = 2.0;
/// ...but never less than this. Past it the race ends with whoever has not
/// finished unclassified, so a car stuck in a gravel trap (or a player who
/// walked away) cannot hold the session open forever.
pub const FINISH_GRACE_MIN_SECONDS: u32 = 60;
/// Skill of the server driver that takes a human's car round on the
/// cool-down lap after they finish: unhurried, off the racing pace.
const COOLDOWN_SKILL: u8 = 75;

/// How far before the line a hotlap car is put out, so the first flying lap
/// starts at speed. Shortened on a track too small for it.
pub const HOTLAP_RUNUP_M: f32 = 300.0;
/// Cars going out together are queued this far apart on the run-up.
pub const HOTLAP_SPACING_M: f32 = 30.0;
/// A run-up slot is taken while a car on the track is this close to it.
const HOTLAP_SLOT_CLEARANCE_M: f32 = 20.0;
/// How many slots back the queue reaches before cars double up.
const HOTLAP_SLOT_TRIES: usize = 16;

pub struct GameSession {
    pub session: RaceSession,
    pub track_config: TrackConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
    /// AI driver profiles indexed by their player ID
    pub ai_profiles: std::collections::BTreeMap<PlayerId, AiDriverProfile>,
    /// Speed profile along the line per car the AI drives, built when the AI
    /// is seated. Looked up by key only, never iterated.
    ai_speed_profiles: HashMap<CarConfigId, RacingLineProfile>,
    /// Simulation tick rate (Hz); the fixed timestep is `1 / tick_rate_hz`.
    tick_rate_hz: u16,
    /// Set when session membership changed since the last roster broadcast;
    /// starts true so the roster goes out once when the session first ticks.
    roster_dirty: bool,
    /// Tick at which the race ends whether or not every car has finished,
    /// set when the winner crosses the line.
    finish_deadline_tick: Option<u32>,
    /// A finished human's steering aid, held while the server drives their
    /// car on the cool-down lap (the AI steers the rack directly) and given
    /// back when the grid is lined up again. Looked up by key only.
    held_steering_assist: HashMap<PlayerId, bool>,
    /// The car a driver with a garage setup is simulated with: their
    /// `CarSetup` baked into a copy of the shared config, remade on every
    /// `SetCarSetup` and dropped when the setup is stock or the player
    /// leaves. Looked up by key only, never iterated.
    tuned_configs: HashMap<PlayerId, CarConfig>,
    /// What each driver asked for, clamped.
    car_setups: HashMap<PlayerId, CarSetup>,
    /// The livery each human driver picked (`SelectCar`); AI drivers are
    /// dealt one in `build_roster`. Looked up by key only.
    liveries: HashMap<PlayerId, u8>,
    /// Timing lines crossed since the game loop last drained them.
    lap_events: Vec<SessionLapEvent>,
    /// The lap each car is driving, sampled for a ghost. Human drivers only:
    /// a record is a driver's own, and the AI never sets one.
    lap_traces: HashMap<PlayerId, Vec<GhostSample>>,
    /// The fastest legal lap anyone has set in this session, and the fastest
    /// each sector has been driven — the purple times on a timing screen.
    session_best_lap_ms: Option<u32>,
    session_best_splits_ms: [Option<u32>; SECTOR_COUNT],
}

/// A timing line crossed by one car, with what it meant for the session.
#[derive(Debug, Clone)]
pub struct SessionLapEvent {
    pub player_id: PlayerId,
    pub event: LapEvent,
    /// The fastest anyone has gone in this session.
    pub session_best_lap: bool,
    pub session_best_sector: bool,
    /// The lap's trace, taken when a human driver set a personal best on a
    /// legal lap. The store decides whether it beats their record.
    pub ghost: Option<GhostLap>,
}

/// Longest ghost trace held for one car: five minutes at [`GHOST_SAMPLE_HZ`].
/// A car parked on the road must not grow a buffer without end.
const MAX_GHOST_SAMPLES: usize = (GHOST_SAMPLE_HZ as usize) * 300;

/// Take one car's timing-line crossing: update the session bests, take the
/// ghost trace when the lap earned one, and queue the event for the loop.
///
/// A free function, not a method: the tick loops hold `participants`
/// mutably, and this touches only the disjoint fields beside it.
#[allow(clippy::too_many_arguments)]
fn note_lap_event(
    out: &mut Vec<SessionLapEvent>,
    traces: &mut HashMap<PlayerId, Vec<GhostSample>>,
    best_lap_ms: &mut Option<u32>,
    best_splits_ms: &mut [Option<u32>; SECTOR_COUNT],
    player_id: PlayerId,
    is_ai: bool,
    event: LapEvent,
) {
    let sector = (event.sector as usize).min(SECTOR_COUNT - 1);
    let session_best_sector =
        event.valid && best_splits_ms[sector].is_none_or(|best| event.sector_time_ms < best);
    if session_best_sector {
        best_splits_ms[sector] = Some(event.sector_time_ms);
    }

    let mut session_best_lap = false;
    let mut ghost = None;
    if let Some(lap_time_ms) = event.lap_time_ms {
        session_best_lap = event.valid && best_lap_ms.is_none_or(|best| lap_time_ms < best);
        if session_best_lap {
            *best_lap_ms = Some(lap_time_ms);
        }
        // The lap is over either way: the trace of it goes with the event or
        // is thrown away, so the next lap records from empty.
        let samples = traces.remove(&player_id).unwrap_or_default();
        if !is_ai && event.valid && event.personal_best_lap && !samples.is_empty() {
            ghost = Some(GhostLap {
                lap_time_ms,
                sample_hz: GHOST_SAMPLE_HZ,
                samples,
            });
        }
    }

    out.push(SessionLapEvent {
        player_id,
        event,
        session_best_lap,
        session_best_sector,
        ghost,
    });
}

/// Append this tick's pose to a human driver's ghost trace, at
/// [`GHOST_SAMPLE_HZ`]. Called before the progress update, so the sample's
/// time is measured against the lap the car is still on.
fn sample_ghost_trace(
    traces: &mut HashMap<PlayerId, Vec<GhostSample>>,
    state: &CarState,
    is_ai: bool,
    current_tick: u32,
    tick_rate_hz: u16,
) {
    if is_ai || state.current_lap == 0 {
        return;
    }
    let divisor = ((tick_rate_hz as f32 / GHOST_SAMPLE_HZ).round() as u32).max(1);
    if !current_tick.is_multiple_of(divisor) {
        return;
    }
    let trace = traces.entry(state.player_id).or_default();
    if trace.len() >= MAX_GHOST_SAMPLES {
        return;
    }
    trace.push(GhostSample {
        t_ms: crate::laps::ticks_to_ms(
            current_tick.saturating_sub(state.lap_start_tick),
            tick_rate_hz,
        ),
        x: state.pos_x,
        y: state.pos_y,
        z: state.pos_z,
        yaw_rad: state.yaw_rad,
        pitch_rad: state.pitch_rad,
        roll_rad: state.roll_rad,
        speed_mps: state.speed_mps,
        steering: state.steering_input,
        throttle: state.throttle_input,
        brake: state.brake_input,
        gear: state.gear,
        engine_rpm: state.engine_rpm,
    });
}

/// The config a car is simulated with: the driver's tuned copy when they
/// have one, else the shared config for the car. A free function so the
/// tick loops can hold `participants` mutably alongside it.
fn simulated_config<'a>(
    car_configs: &'a HashMap<CarConfigId, CarConfig>,
    tuned_configs: &'a HashMap<PlayerId, CarConfig>,
    state: &CarState,
) -> Option<&'a CarConfig> {
    tuned_configs
        .get(&state.player_id)
        .or_else(|| car_configs.get(&state.car_config_id))
}

impl GameSession {
    pub fn new(
        session: RaceSession,
        track_config: TrackConfig,
        car_configs: HashMap<CarConfigId, CarConfig>,
    ) -> Self {
        Self {
            session,
            track_config,
            car_configs,
            ai_profiles: std::collections::BTreeMap::new(),
            tick_rate_hz: DEFAULT_TICK_RATE_HZ,
            roster_dirty: true,
            ai_speed_profiles: HashMap::new(),
            finish_deadline_tick: None,
            held_steering_assist: HashMap::new(),
            tuned_configs: HashMap::new(),
            liveries: HashMap::new(),
            car_setups: HashMap::new(),
            lap_events: Vec::new(),
            lap_traces: HashMap::new(),
            session_best_lap_ms: None,
            session_best_splits_ms: [None; SECTOR_COUNT],
        }
    }

    /// Create a new game session with AI driver profiles.
    ///
    /// # Arguments
    /// * `session` - The race session configuration
    /// * `track_config` - Track configuration
    /// * `car_configs` - Available car configurations
    /// * `ai_profiles` - AI driver profiles to use for this session
    pub fn with_ai_profiles(
        session: RaceSession,
        track_config: TrackConfig,
        car_configs: HashMap<CarConfigId, CarConfig>,
        ai_profiles: Vec<AiDriverProfile>,
    ) -> Self {
        let ai_profiles_map: std::collections::BTreeMap<PlayerId, AiDriverProfile> =
            ai_profiles.into_iter().map(|p| (p.id, p)).collect();

        Self {
            session,
            track_config,
            car_configs,
            ai_profiles: ai_profiles_map,
            tick_rate_hz: DEFAULT_TICK_RATE_HZ,
            roster_dirty: true,
            ai_speed_profiles: HashMap::new(),
            finish_deadline_tick: None,
            held_steering_assist: HashMap::new(),
            tuned_configs: HashMap::new(),
            liveries: HashMap::new(),
            car_setups: HashMap::new(),
            lap_events: Vec::new(),
            lap_traces: HashMap::new(),
            session_best_lap_ms: None,
            session_best_splits_ms: [None; SECTOR_COUNT],
        }
    }

    /// Set the simulation tick rate (Hz). Called with the configured server
    /// tick rate when the session is created by the server.
    pub fn set_tick_rate(&mut self, tick_rate_hz: u16) {
        self.tick_rate_hz = tick_rate_hz;
    }

    /// The simulation tick rate (Hz) this session runs at.
    pub fn tick_rate_hz(&self) -> u16 {
        self.tick_rate_hz
    }

    /// Fixed physics timestep in seconds, derived from the tick rate.
    fn dt(&self) -> f32 {
        1.0 / self.tick_rate_hz as f32
    }

    /// Advance the session by one tick
    pub fn tick(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.session.current_tick += 1;
        crate::headlights::update(
            &mut self.session.participants,
            inputs,
            self.session.conditions,
        );

        // Handle game mode specific logic
        match self.session.game_mode {
            GameMode::Lobby => {
                self.tick_lobby();
            }
            GameMode::Sandbox => {
                self.tick_sandbox();
            }
            GameMode::Countdown => {
                self.tick_countdown(inputs);
            }
            GameMode::DemoLap => {
                self.tick_demolap(inputs);
            }
            GameMode::FreePractice => {
                self.tick_free_practice(inputs);
            }
            GameMode::Replay => {
                self.tick_replay();
            }
            GameMode::Race => {
                self.tick_racing(inputs);
            }
            GameMode::Qualification => {
                // Qualification is practice-with-timing for now: free driving
                // with lap timing, no finish-position logic.
                self.tick_free_practice(inputs);
            }
            GameMode::Hotlap => {
                self.tick_hotlap(inputs);
            }
        }
    }

    /// Lobby mode: Players selecting cars, no telemetry sent
    fn tick_lobby(&mut self) {
        // In lobby mode, no simulation occurs and no telemetry is sent
        // Players are selecting cars and waiting for session to start
    }

    /// Sandbox mode: No movement, no telemetry recording, camera exploration only
    fn tick_sandbox(&mut self) {
        // In sandbox mode, nothing moves and no physics updates occur
        // Players can freely move camera around the track
        // No telemetry is recorded or sent
    }

    /// Countdown mode: Players frozen in pit lane, countdown timer running
    fn tick_countdown(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        if let Some(ref mut countdown) = self.session.countdown_ticks_remaining {
            if *countdown > 0 {
                *countdown -= 1;
            } else {
                // Countdown finished: transition to the stored next mode
                // (set by start_countdown_mode). Without a stored next mode
                // we just clear the countdown, as before.
                self.session.countdown_ticks_remaining = None;
                if let Some(next_mode) = self.session.next_mode.take() {
                    self.transition_from_countdown(next_mode);
                }
            }
        }
        // Cars are held where they stand, no physics: the drivers can only
        // pick a gear and rev their engines. The countdown may have just
        // handed over to the next mode, which then owns this tick's cars.
        if self.session.game_mode != GameMode::Countdown {
            return;
        }
        let dt = self.dt();
        for state in self.session.participants.values_mut() {
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                physics::update_car_on_grid(state, config, &input, dt);
            }
        }
    }

    /// Demo lap mode: AI driver demonstrates the track
    fn tick_demolap(&mut self, player_inputs: &HashMap<PlayerId, PlayerInputData>) {
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // Initialize demo lap progress if not set
        if self.session.demo_lap_progress.is_none() {
            self.session.demo_lap_progress = Some(0.0);
        }

        // Use AI-driven demo lap if we have AI drivers
        if !self.session.ai_player_ids.is_empty() {
            // Merge player inputs with AI inputs
            let mut inputs = player_inputs.clone();
            for ai_id in &self.session.ai_player_ids {
                let ai_input = self.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }

            // Update physics for all cars (player + AI)
            let ai_ids: std::collections::HashSet<PlayerId> =
                self.session.ai_player_ids.iter().copied().collect();
            let current_tick = self.session.current_tick;
            let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();
            for state in states.iter_mut() {
                let input = inputs.get(&state.player_id).copied().unwrap_or_default();

                if let Some(config) =
                    simulated_config(&self.car_configs, &self.tuned_configs, state)
                {
                    physics::update_car_3d(state, config, &input, &self.track_config, dt);
                    let is_ai = ai_ids.contains(&state.player_id);
                    sample_ghost_trace(
                        &mut self.lap_traces,
                        state,
                        is_ai,
                        current_tick,
                        self.tick_rate_hz,
                    );
                    if let Some(event) = physics::update_track_progress_3d(
                        state,
                        &self.track_config,
                        current_tick,
                        self.tick_rate_hz,
                    ) {
                        note_lap_event(
                            &mut self.lap_events,
                            &mut self.lap_traces,
                            &mut self.session_best_lap_ms,
                            &mut self.session_best_splits_ms,
                            state.player_id,
                            is_ai,
                            event,
                        );
                    }
                }
            }

            return;
        }

        // Fallback to old camera-following demo lap if no AI
        if self.track_config.raceline.is_empty() {
            // No racing line available, can't do demo lap
            return;
        }

        let raceline_len = self.track_config.raceline.len();

        if let Some(ref mut progress) = self.session.demo_lap_progress {
            // Calculate position on racing line
            let index = (*progress * raceline_len as f32).floor() as usize;
            let next_index = (index + 1) % raceline_len;
            let t = (*progress * raceline_len as f32) - index as f32;

            let p1 = &self.track_config.raceline[index];
            let p2 = &self.track_config.raceline[next_index];

            // Calculate curvature by looking ahead
            let lookahead_distance = 10; // Points to look ahead
            let ahead_index = (index + lookahead_distance) % raceline_len;
            let way_ahead_index = (index + lookahead_distance * 2) % raceline_len;

            let p_ahead = &self.track_config.raceline[ahead_index];
            let p_way_ahead = &self.track_config.raceline[way_ahead_index];

            // Calculate vectors for curvature estimation
            let v1_x = p_ahead.x - p1.x;
            let v1_y = p_ahead.y - p1.y;
            let v2_x = p_way_ahead.x - p_ahead.x;
            let v2_y = p_way_ahead.y - p_ahead.y;

            let len1 = (v1_x * v1_x + v1_y * v1_y).sqrt();
            let len2 = (v2_x * v2_x + v2_y * v2_y).sqrt();

            // Calculate angle change (curvature indicator)
            let mut curvature = 0.0;
            if len1 > 0.001 && len2 > 0.001 {
                // Dot product to find angle between vectors
                let dot = (v1_x * v2_x + v1_y * v2_y) / (len1 * len2);
                let angle_change = dot.clamp(-1.0, 1.0).acos();
                curvature = angle_change;
            }

            // Speed control based on curvature (adjusted for realistic lap times)
            // Max speed on straights: 60 m/s (216 km/h)
            // Min speed in tight corners: 25 m/s (90 km/h)
            let max_speed = 60.0;
            let min_speed = 25.0;

            // Map curvature (0 to ~PI) to speed range
            // High curvature (sharp corner) = low speed
            // Low curvature (straight) = high speed
            let curvature_factor = 1.0 - (curvature / std::f32::consts::PI).min(1.0);
            let target_speed = min_speed + (max_speed - min_speed) * curvature_factor;

            // Get current speed from demo car or use target speed
            let current_speed = self
                .session
                .participants
                .values()
                .next()
                .map(|car| car.speed_mps)
                .unwrap_or(target_speed);

            // Smooth acceleration/braking
            let accel_rate = 15.0; // m/s² acceleration
            let brake_rate = 25.0; // m/s² braking

            let demo_speed = if current_speed < target_speed {
                // Accelerate
                (current_speed + accel_rate * dt).min(target_speed)
            } else {
                // Brake
                (current_speed - brake_rate * dt).max(target_speed)
            };

            // Advance progress along the racing line based on current speed
            *progress += (demo_speed * dt) / raceline_len as f32;

            // Loop back when completing the lap
            if *progress >= 1.0 {
                *progress = 0.0;
            }

            // Interpolate position
            let x = p1.x + (p2.x - p1.x) * t;
            let y = p1.y + (p2.y - p1.y) * t;
            let z = p1.z + (p2.z - p1.z) * t;

            // Update demo car if there's one participant
            // In demo mode, we should have a single demo car
            if let Some(demo_car) = self.session.participants.values_mut().next() {
                demo_car.pos_x = x;
                demo_car.pos_y = y;
                demo_car.pos_z = z + 1.2; // Camera height 1.2m from surface

                // Calculate forward direction
                let dx = p2.x - p1.x;
                let dy = p2.y - p1.y;
                let dz = p2.z - p1.z;
                let len = (dx * dx + dy * dy + dz * dz).sqrt();

                if len > 0.001 {
                    // Set velocity to move forward along racing line
                    demo_car.vel_x = (dx / len) * demo_speed;
                    demo_car.vel_y = (dy / len) * demo_speed;
                    demo_car.vel_z = (dz / len) * demo_speed;
                    demo_car.speed_mps = demo_speed;

                    // Calculate yaw (heading) from velocity direction
                    demo_car.yaw_rad = (dy / len).atan2(dx / len);
                }
            }
        }
    }

    /// Free practice mode: Players drive freely with lap timing
    fn tick_free_practice(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.update_drs();
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // Update each car
        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        let current_tick = self.session.current_tick;
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();

            // Get car config
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                let is_ai = ai_ids.contains(&state.player_id);
                sample_ghost_trace(
                    &mut self.lap_traces,
                    state,
                    is_ai,
                    current_tick,
                    self.tick_rate_hz,
                );

                // Update track progress; a timing line crossed here becomes
                // a split on every driver's screen.
                if let Some(event) = physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    current_tick,
                    self.tick_rate_hz,
                ) {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai,
                        event,
                    );
                }
            }
        }

        // Check collisions in place (BTreeMap iteration order makes the
        // order-dependent solver deterministic; no clone/rebuild needed)
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);
    }

    /// Hotlap mode: free practice for the cars on the track, while the cars
    /// in the garage stand still — not simulated, not collided with, so a
    /// driver tuning in the garage is out of everyone's way.
    fn tick_hotlap(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.update_drs();
        let dt = self.dt();
        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        let current_tick = self.session.current_tick;

        for state in self.session.participants.values_mut() {
            if state.in_garage {
                continue;
            }
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                physics::update_car_3d(state, config, &input, &self.track_config, dt);
                let is_ai = ai_ids.contains(&state.player_id);
                sample_ghost_trace(
                    &mut self.lap_traces,
                    state,
                    is_ai,
                    current_tick,
                    self.tick_rate_hz,
                );
                if let Some(event) = physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    current_tick,
                    self.tick_rate_hz,
                ) {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai,
                        event,
                    );
                }
            }
        }

        // Only the cars on the track take part in the collision passes; the
        // order is still the BTreeMap's, so the solver stays deterministic.
        let mut state_refs: Vec<&mut CarState> = self
            .session
            .participants
            .values_mut()
            .filter(|s| !s.in_garage)
            .collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);
    }

    /// Replay mode: Send telemetry from recorded data (view-only)
    fn tick_replay(&mut self) {
        // Replay mode is not yet implemented
        // This would play back previously recorded telemetry data
        // For now, do nothing
    }

    /// Race mode: full physics with lap counting, finish positions assigned
    /// as cars complete the race distance, session finished when all cars
    /// are classified.
    fn tick_racing(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.update_drs();
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // A human who has finished is looking at the results and stops
        // sending input, while the last input received stays applied. The
        // server drives the car round instead.
        let cooldown_inputs: HashMap<PlayerId, PlayerInputData> = self
            .session
            .participants
            .values()
            .filter(|s| s.finish_position.is_some() && !self.is_ai_player(&s.player_id))
            .map(|s| (s.player_id, self.cooldown_input(&s.player_id)))
            .collect();

        // Update each car
        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        let current_tick = self.session.current_tick;
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = cooldown_inputs
                .get(&state.player_id)
                .or_else(|| inputs.get(&state.player_id))
                .copied()
                .unwrap_or_default();

            // Get car config
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                let is_ai = ai_ids.contains(&state.player_id);
                sample_ghost_trace(
                    &mut self.lap_traces,
                    state,
                    is_ai,
                    current_tick,
                    self.tick_rate_hz,
                );

                // Update track progress; a timing line crossed here becomes
                // a split on every driver's screen.
                if let Some(event) = physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    current_tick,
                    self.tick_rate_hz,
                ) {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai,
                        event,
                    );
                }
            }
        }

        // Check collisions in place (BTreeMap iteration order makes the
        // order-dependent solver deterministic; no clone/rebuild needed)
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);

        // Assign finish positions to cars that just completed the race
        // distance (in crossing order), then finish the session exactly once
        // when every car is classified.
        self.assign_finish_positions();
        if self.session.state != SessionState::Finished && self.is_race_complete() {
            self.session.state = SessionState::Finished;
        }
    }

    /// Start the countdown
    pub fn start_countdown(&mut self) {
        if self.session.state == SessionState::Lobby {
            self.session.state = SessionState::Countdown;
            self.session.countdown_ticks_remaining = Some(self.tick_rate_hz * 5);
            // 5 seconds
        }
    }

    /// Set the game mode
    pub fn set_game_mode(&mut self, mode: GameMode) {
        self.session.game_mode = mode;

        // Initialize mode-specific state
        match mode {
            GameMode::DemoLap => {
                debug!(
                    "[DemoLap] Setting demo lap mode. Participants: {}, AI profiles: {}",
                    self.session.participants.len(),
                    self.ai_profiles.len()
                );

                self.session.demo_lap_progress = Some(0.0);
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;

                // Remove human players from participants (they become spectators)
                // Only AI drivers should be in participants for DemoLap
                let human_player_ids: Vec<PlayerId> = self
                    .session
                    .participants
                    .keys()
                    .filter(|id| !self.session.ai_player_ids.contains(id))
                    .cloned()
                    .collect();
                for player_id in &human_player_ids {
                    self.session.participants.remove(player_id);
                    self.roster_dirty = true;
                    debug!(
                        "[DemoLap] Removed human player {} from participants (now spectator)",
                        player_id
                    );
                }

                // Ensure we have an AI driver for demo lap
                if self.session.ai_player_ids.is_empty() {
                    if !self.ai_profiles.is_empty() {
                        debug!("[DemoLap] Spawning AI from existing profiles");
                        // Temporarily increment ai_count and max_players to allow AI spawn
                        let original_ai_count = self.session.ai_count;
                        let original_max = self.session.max_players;
                        self.session.ai_count = 1;
                        self.session.max_players = 1;
                        self.spawn_ai_drivers();
                        self.session.ai_count = original_ai_count;
                        self.session.max_players = original_max;
                    } else {
                        // No AI profiles configured, create a default demo driver
                        use crate::ai_driver::AiDriverProfile;

                        debug!(
                            "[DemoLap] Creating default demo driver. Host car: {:?}",
                            self.session.host_car_id
                        );

                        let mut demo_profile = AiDriverProfile::new("Demo Driver", 95);
                        demo_profile.preferred_car_id = self.session.host_car_id;

                        let demo_player_id = demo_profile.id;
                        self.ai_profiles.insert(demo_player_id, demo_profile);

                        // Temporarily increment ai_count and max_players to allow AI spawn
                        let original_ai_count = self.session.ai_count;
                        let original_max = self.session.max_players;
                        self.session.ai_count = 1;
                        self.session.max_players = 1;
                        self.spawn_ai_drivers();
                        self.session.ai_count = original_ai_count;
                        self.session.max_players = original_max;

                        debug!(
                            "[DemoLap] After spawn: Participants: {}, AI IDs: {}",
                            self.session.participants.len(),
                            self.session.ai_player_ids.len()
                        );
                    }
                } else {
                    debug!(
                        "[DemoLap] Already have {} AI drivers",
                        self.session.ai_player_ids.len()
                    );
                }
            }
            GameMode::FreePractice => {
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;
            }
            GameMode::Sandbox => {
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;
            }
            GameMode::Countdown => {
                // Default 10 second countdown as per spec
                self.session.countdown_ticks_remaining = Some(self.tick_rate_hz * 10);
                self.session.state = SessionState::Countdown;
            }
            GameMode::Race => {
                // The race starts now: mark the session racing and remember
                // the start tick for race-time bookkeeping.
                self.session.state = SessionState::Racing;
                self.session.race_start_tick = Some(self.session.current_tick);
                self.session.demo_lap_progress = None;
                self.finish_deadline_tick = None;
                // Pole sits on the line, so it never crosses it to start lap
                // 1: its lap starts with the green light.
                let tick = self.session.current_tick;
                for state in self.session.participants.values_mut() {
                    physics::start_lap_on_green(state, &self.track_config, tick);
                    // The grid waits in neutral; the automatic box takes
                    // first with the green light, never before.
                    if state.auto_gearbox && state.gear == 0 {
                        state.gear = 1;
                        state.auto_shift_hold_ticks = 0;
                    }
                }
            }
            GameMode::Qualification => {
                // Practice-with-timing: telemetry must flow
                self.session.state = SessionState::Racing;
                self.session.demo_lap_progress = None;
            }
            GameMode::Hotlap => {
                // Every driver starts in the garage, with the setup screen;
                // the AI (if any) is left where it stands and drives on.
                self.session.state = SessionState::Racing;
                self.session.demo_lap_progress = None;
                self.finish_deadline_tick = None;
                let humans: Vec<PlayerId> = self
                    .session
                    .participants
                    .keys()
                    .filter(|id| !self.session.ai_player_ids.contains(id))
                    .copied()
                    .collect();
                for player_id in humans {
                    let _ = self.hotlap_relocate(&player_id, HotlapDestination::Garage);
                }
            }
            _ => {
                self.session.demo_lap_progress = None;
            }
        }
    }

    /// Move a hotlap driver's car: into the garage (parked on its grid slot,
    /// frozen, out of the collision passes) or out onto the run-up before
    /// the line, on the first free slot of a queue spaced [`HOTLAP_SPACING_M`]
    /// apart, so several drivers can go out together. The car comes back
    /// fresh — no damage, no half-driven lap — but keeps its aids and its
    /// bests. The lap starts, timed, as the car crosses the line.
    pub fn hotlap_relocate(
        &mut self,
        player_id: &PlayerId,
        destination: HotlapDestination,
    ) -> Result<(), &'static str> {
        if self.session.game_mode != GameMode::Hotlap {
            return Err("Not a hotlap session");
        }
        let Some(state) = self.session.participants.get(player_id) else {
            return Err("No car in this session");
        };
        let pose = match destination {
            HotlapDestination::Garage => {
                let slot = self
                    .track_config
                    .start_positions
                    .iter()
                    .find(|s| s.position == state.grid_position)
                    .or_else(|| self.track_config.start_positions.first())
                    .ok_or("Track has no grid")?;
                (slot.x, slot.y, slot.z, slot.yaw_rad)
            }
            HotlapDestination::Track => self
                .hotlap_runup_pose(player_id)
                .ok_or("Track has no centerline")?,
        };
        let in_garage = destination == HotlapDestination::Garage;
        let state = self
            .session
            .participants
            .get_mut(player_id)
            .ok_or("No car in this session")?;
        let slot = GridSlot {
            position: state.grid_position,
            x: pose.0,
            y: pose.1,
            z: pose.2,
            yaw_rad: pose.3,
        };
        let mut fresh = CarState::new(state.player_id, state.car_config_id, &slot);
        fresh.auto_gearbox = state.auto_gearbox;
        fresh.abs = state.abs;
        fresh.traction_control = state.traction_control;
        fresh.steering_assist = state.steering_assist;
        // The timing sheet survives the trip: the panel and the delta are
        // measured against what the driver did before going in.
        fresh.best_lap_time_ms = state.best_lap_time_ms;
        fresh.last_lap_time_ms = state.last_lap_time_ms;
        fresh.laps.last_invalid = state.laps.last_invalid;
        fresh.laps.last_splits_ms = state.laps.last_splits_ms;
        fresh.laps.best_lap_ms = state.laps.best_lap_ms;
        fresh.laps.best_lap_splits_ms = state.laps.best_lap_splits_ms;
        fresh.laps.best_splits_ms = state.laps.best_splits_ms;
        fresh.in_garage = in_garage;
        // Parked cars wait in neutral; a car put on the run-up is in first.
        fresh.gear = if in_garage { 0 } else { 1 };
        physics::seed_track_progress(&mut fresh, &self.track_config);
        *state = fresh;
        // The lap the car was on is abandoned with it.
        self.lap_traces.remove(player_id);
        Ok(())
    }

    /// Where the next car going out is put: on the centerline
    /// [`HOTLAP_RUNUP_M`] before the line, or the first slot behind it not
    /// already taken by a car on the track. `None` without a centerline.
    fn hotlap_runup_pose(&self, going_out: &PlayerId) -> Option<(f32, f32, f32, f32)> {
        let track = &self.track_config;
        let length = crate::laps::track_length_m(track);
        if track.centerline.len() < 2 || length <= 0.0 {
            return None;
        }
        let runup = HOTLAP_RUNUP_M.min(length * 0.2);
        let spacing = HOTLAP_SPACING_M.min(runup * 0.5);
        let mut fallback = None;
        for k in 0..HOTLAP_SLOT_TRIES {
            let station = length - runup - k as f32 * spacing;
            if station < length * 0.5 {
                break;
            }
            let pose = physics::pose_at_station(&track.centerline, station);
            fallback.get_or_insert(pose);
            let taken = self.session.participants.values().any(|other| {
                !other.in_garage
                    && other.player_id != *going_out
                    && (other.pos_x - pose.0).hypot(other.pos_y - pose.1) < HOTLAP_SLOT_CLEARANCE_M
            });
            if !taken {
                return Some(pose);
            }
        }
        fallback
    }

    /// Start countdown mode with custom duration. The stored `next_mode` is
    /// transitioned to automatically when the countdown reaches zero.
    pub fn start_countdown_mode(&mut self, countdown_seconds: u16, next_mode: GameMode) {
        if next_mode == GameMode::Race {
            // A race starts from the grid: after a finished race (or practice)
            // the cars are wherever they stopped, with their laps and finish
            // positions still set.
            self.line_up_on_grid();
        }
        self.session.game_mode = GameMode::Countdown;
        self.session.state = SessionState::Countdown;
        self.session.countdown_ticks_remaining = Some(self.tick_rate_hz * countdown_seconds);
        self.session.next_mode = Some(next_mode);
    }

    /// Transition from Countdown to another mode
    pub fn transition_from_countdown(&mut self, next_mode: GameMode) {
        self.session.countdown_ticks_remaining = None;
        self.session.next_mode = None;
        // set_game_mode performs the per-mode initialization (session state,
        // demo lap progress / demo driver spawn, race start bookkeeping).
        self.set_game_mode(next_mode);
    }

    /// Add a player to the session
    pub fn add_player(&mut self, player_id: PlayerId, car_config_id: CarConfigId) -> Option<u8> {
        if self.session.participants.len() >= self.session.max_players as usize {
            return None;
        }

        // Find available grid position
        let mut used_positions: Vec<u8> = self
            .session
            .participants
            .values()
            .map(|s| s.grid_position)
            .collect();
        used_positions.sort();

        let mut grid_position = 1;
        for pos in &used_positions {
            if *pos == grid_position {
                grid_position += 1;
            } else {
                break;
            }
        }

        // Get grid slot
        if let Some(grid_slot) = self
            .track_config
            .start_positions
            .iter()
            .find(|s| s.position == grid_position)
        {
            let mut car_state = CarState::new(player_id, car_config_id, grid_slot);
            // A client that never sends SetDriverAids would otherwise run the
            // car's own ABS and traction control in a session that forbids them.
            car_state.set_driver_aids(self.session.allowed_assists.clamp(car_state.driver_aids()));
            physics::seed_track_progress(&mut car_state, &self.track_config);
            self.session.participants.insert(player_id, car_state);
            self.roster_dirty = true;
            Some(grid_position)
        } else {
            None
        }
    }

    /// Set a driver's aids to what they asked for, less what the session
    /// forbids; returns what was applied, or `None` for a player not here.
    pub fn set_driver_aids(
        &mut self,
        player_id: &PlayerId,
        asked: DriverAids,
    ) -> Option<DriverAids> {
        let applied = self.session.allowed_assists.clamp(asked);
        let car = self.session.participants.get_mut(player_id)?;
        car.set_driver_aids(applied);
        Some(applied)
    }

    /// Apply a driver's garage setup: every knob clamped into range, the
    /// result baked into the car they are simulated with from now on. A
    /// stock setup drops the tuned copy. `None` when the player has no car
    /// here. The setup takes effect at once, on the grid or mid-lap.
    pub fn set_car_setup(&mut self, player_id: &PlayerId, asked: CarSetup) -> Option<CarSetup> {
        let applied = asked.clamp();
        let car = self.session.participants.get(player_id)?;
        let base = self.car_configs.get(&car.car_config_id)?;
        if applied.is_stock() {
            self.tuned_configs.remove(player_id);
            self.car_setups.remove(player_id);
        } else {
            self.tuned_configs.insert(*player_id, applied.apply(base));
            self.car_setups.insert(*player_id, applied);
        }
        Some(applied)
    }

    /// The clamped setup a driver last sent; stock when they never did.
    pub fn car_setup(&self, player_id: &PlayerId) -> CarSetup {
        self.car_setups.get(player_id).copied().unwrap_or_default()
    }

    /// The config a driver's car is simulated with (their setup applied).
    pub fn simulated_config_for(&self, player_id: &PlayerId) -> Option<&CarConfig> {
        let state = self.session.participants.get(player_id)?;
        simulated_config(&self.car_configs, &self.tuned_configs, state)
    }

    /// Remove a player from the session
    pub fn remove_player(&mut self, player_id: &PlayerId) {
        if self.session.participants.remove(player_id).is_some() {
            self.roster_dirty = true;
        }
        self.tuned_configs.remove(player_id);
        self.car_setups.remove(player_id);
        self.lap_traces.remove(player_id);
        self.liveries.remove(player_id);
    }

    /// The car index this player's telemetry carries, matching the roster.
    pub fn car_index_of(&self, player_id: &PlayerId) -> Option<u8> {
        self.session
            .participants
            .keys()
            .position(|id| id == player_id)
            .map(|idx| idx as u8)
    }

    /// Whether any car crossed a timing line this tick. The game loop drains
    /// the events once a tick, broadcasts them and measures the laps against
    /// the stored records; nothing in the sim reads them.
    pub fn has_lap_events(&self) -> bool {
        !self.lap_events.is_empty()
    }

    /// Timing lines crossed since the last call, drained.
    pub fn take_lap_events(&mut self) -> Vec<SessionLapEvent> {
        std::mem::take(&mut self.lap_events)
    }

    /// Where this session's sector lines are, for the joining client.
    pub fn sector_boundaries_m(&self) -> Vec<f32> {
        crate::laps::sector_boundaries_m(&self.track_config)
    }

    /// Lap length in metres.
    pub fn track_length_m(&self) -> f32 {
        crate::laps::track_length_m(&self.track_config)
    }

    /// The fastest legal lap set in this session so far.
    pub fn session_best_lap_ms(&self) -> Option<u32> {
        self.session_best_lap_ms
    }

    /// Put every car back on its grid slot with a clean race state (laps,
    /// times, finish position, damage), keeping the driver's aids.
    pub fn line_up_on_grid(&mut self) {
        self.finish_deadline_tick = None;
        // A fresh race, a fresh timing sheet: the session's bests and every
        // half-recorded ghost lap belong to the race that just ended.
        self.session_best_lap_ms = None;
        self.session_best_splits_ms = [None; SECTOR_COUNT];
        self.lap_traces.clear();
        self.lap_events.clear();
        for state in self.session.participants.values_mut() {
            let Some(slot) = self
                .track_config
                .start_positions
                .iter()
                .find(|s| s.position == state.grid_position)
            else {
                continue;
            };
            let mut fresh = CarState::new(state.player_id, state.car_config_id, slot);
            // A race starts in neutral, so a driver can rev on the grid.
            fresh.gear = 0;
            fresh.auto_gearbox = state.auto_gearbox;
            fresh.abs = state.abs;
            fresh.traction_control = state.traction_control;
            fresh.steering_assist = self
                .held_steering_assist
                .remove(&state.player_id)
                .unwrap_or(state.steering_assist);
            physics::seed_track_progress(&mut fresh, &self.track_config);
            *state = fresh;
        }
        self.held_steering_assist.clear();
    }

    /// Input for a finished human's car: a gentle server driver on the line,
    /// seeded from the player's id so the sim stays deterministic. Public so
    /// tests can drive a human's car round without a controller.
    pub fn cooldown_input(&self, player_id: &PlayerId) -> PlayerInputData {
        let Some(state) = self.session.participants.get(player_id) else {
            return PlayerInputData::default();
        };
        let Some(car_config) = self.car_configs.get(&state.car_config_id) else {
            return PlayerInputData::default();
        };
        let mut profile = AiDriverProfile::new("Cool-down", COOLDOWN_SKILL);
        profile.id = *player_id;
        self.ai_input_for(&profile, state, car_config)
    }

    /// The DRS rule for this tick (`crate::drs`): which cars may open the
    /// flap where they are now. Before the physics, so the input the AI
    /// generated against last tick's `drs_allowed` and the human's button
    /// both meet an up-to-date answer.
    fn update_drs(&mut self) {
        crate::drs::update(
            &mut self.session.participants,
            &self.car_configs,
            &self.track_config,
            self.session.game_mode,
            self.session.conditions.weather,
        );
    }

    /// Input from `profile` driving `state` among the rest of the field.
    fn ai_input_for(
        &self,
        profile: &AiDriverProfile,
        state: &CarState,
        car_config: &CarConfig,
    ) -> PlayerInputData {
        let controller = AiDriverController::new(profile, &self.track_config, car_config)
            .with_speed_profile(self.ai_speed_profiles.get(&state.car_config_id));
        // BTreeMap order: the traffic list, and so the input, is
        // deterministic.
        let traffic: Vec<TrafficCar> = self
            .session
            .participants
            .values()
            .filter(|other| other.player_id != state.player_id)
            .filter_map(|other| {
                let config = self.car_configs.get(&other.car_config_id)?;
                Some(TrafficCar {
                    state: other,
                    length_m: config.length_m,
                    width_m: config.width_m,
                })
            })
            .collect();
        controller.generate_input_in_traffic(
            state,
            &traffic,
            self.session.current_tick,
            self.tick_rate_hz,
        )
    }

    /// Generate AI input for a player using their AI profile.
    ///
    /// Returns default input if the player is not an AI or has no profile.
    pub fn generate_ai_input(&self, player_id: &PlayerId) -> PlayerInputData {
        // Check if this player has an AI profile
        if let Some(profile) = self.ai_profiles.get(player_id) {
            if let Some(state) = self.session.participants.get(player_id) {
                // Get the car config for this AI player
                if let Some(car_config) = self.car_configs.get(&state.car_config_id) {
                    return self.ai_input_for(profile, state, car_config);
                }
            }
        }

        // Fallback: no AI profile found, return default (coasting)
        PlayerInputData::default()
    }

    /// Check if a player is an AI driver.
    pub fn is_ai_player(&self, player_id: &PlayerId) -> bool {
        self.ai_profiles.contains_key(player_id)
    }

    /// Get the AI profile for a player, if they are an AI.
    pub fn get_ai_profile(&self, player_id: &PlayerId) -> Option<&AiDriverProfile> {
        self.ai_profiles.get(player_id)
    }

    /// Get telemetry for broadcast
    pub fn get_telemetry(&self) -> ServerMessage {
        let car_states: Vec<CarStateTelemetry> = self
            .session
            .participants
            .values()
            .map(CarStateTelemetry::from)
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / self.tick_rate_hz as f32) * 1000.0) as u16);

        let telemetry = crate::network::Telemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
        };

        ServerMessage::Telemetry(telemetry)
    }

    /// Compact wire telemetry (protocol v2): cars are identified by their
    /// position in the deterministic `participants` iteration order, matching
    /// the indices announced in the most recent `SessionRoster`.
    pub fn get_compact_telemetry(&self) -> CompactTelemetry {
        let car_states: Vec<CompactCarState> = self
            .session
            .participants
            .values()
            .enumerate()
            .map(|(idx, s)| CompactCarState::from_car_state(s, idx as u8))
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / self.tick_rate_hz as f32) * 1000.0) as u16);

        CompactTelemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
        }
    }

    /// The livery a driver wears, as they picked it. Clamped to the car's
    /// own list when the roster is built, so a stale pick is harmless.
    pub fn set_livery(&mut self, player_id: PlayerId, livery: u8) {
        let old = self.liveries.insert(player_id, livery).unwrap_or(0);
        if old != livery {
            self.roster_dirty = true;
        }
    }

    /// Whether session membership changed since the last roster broadcast.
    pub fn roster_is_dirty(&self) -> bool {
        self.roster_dirty
    }

    /// True once per membership change: returns whether a fresh roster needs
    /// broadcasting and clears the flag.
    pub fn take_roster_dirty(&mut self) -> bool {
        std::mem::take(&mut self.roster_dirty)
    }

    /// Build the car-index → player mapping for compact telemetry. Human
    /// player names are looked up in `names` (lobby data); AI names come from
    /// their profiles.
    pub fn build_roster(&self, names: &HashMap<PlayerId, String>) -> SessionRosterData {
        // AI drivers are dealt liveries in grid order, each model's cars
        // taking the next one of its list, so a field of the same car is not
        // a row of clones.
        let mut dealt: HashMap<CarConfigId, u8> = HashMap::new();
        let entries = self
            .session
            .participants
            .iter()
            .enumerate()
            .map(|(idx, (player_id, state))| {
                let livery_count = self
                    .car_configs
                    .get(&state.car_config_id)
                    .map_or(0, |c| c.livery_names.len().min(u8::MAX as usize - 1) as u8);
                let is_ai = self.ai_profiles.contains_key(player_id);
                let player_name = self
                    .ai_profiles
                    .get(player_id)
                    .map(|p| p.name.clone())
                    .or_else(|| names.get(player_id).cloned())
                    .unwrap_or_else(|| format!("Player-{}", &player_id.to_string()[..8]));
                let livery = if is_ai {
                    let next = dealt.entry(state.car_config_id).or_insert(0);
                    let pick = *next % (livery_count + 1);
                    *next = next.wrapping_add(1);
                    pick
                } else {
                    let asked = self.liveries.get(player_id).copied().unwrap_or(0);
                    if asked <= livery_count {
                        asked
                    } else {
                        0
                    }
                };
                RosterEntry {
                    car_index: idx as u8,
                    player_id: *player_id,
                    player_name,
                    is_ai,
                    car_config_id: state.car_config_id,
                    livery,
                }
            })
            .collect();

        SessionRosterData {
            session_id: self.session.id,
            entries,
        }
    }

    /// Race is complete once every car has been classified with a finish
    /// position (i.e. completed the race distance).
    fn is_race_complete(&self) -> bool {
        if self.session.participants.is_empty() {
            return false;
        }
        // The winner's finish started the clock on everyone else.
        if self
            .finish_deadline_tick
            .is_some_and(|deadline| self.session.current_tick >= deadline)
        {
            return true;
        }

        // A car counts as done when it is classified (finished the race
        // distance) OR is a DNF (undrivable — it can never finish, and must
        // not keep the race running forever).
        self.session
            .participants
            .values()
            .all(|s| s.finish_position.is_some() || !s.damage.is_drivable)
    }

    /// Assign finish positions incrementally, in the order cars complete the
    /// race distance (`current_lap > lap_limit` means the car has completed
    /// all `lap_limit` laps). Positions are unique and never reassigned;
    /// cars finishing on the same tick are ordered by laps then progress
    /// (ties broken by deterministic BTreeMap player order).
    fn assign_finish_positions(&mut self) {
        let lap_limit = self.session.lap_limit as u16;

        let assigned = self
            .session
            .participants
            .values()
            .filter(|s| s.finish_position.is_some())
            .count() as u8;

        let mut new_finishers: Vec<(PlayerId, u16, f32)> = self
            .session
            .participants
            .iter()
            .filter(|(_, s)| s.finish_position.is_none() && s.current_lap > lap_limit)
            .map(|(id, s)| (*id, s.current_lap, s.track_progress))
            .collect();

        if new_finishers.is_empty() {
            return;
        }

        // Sort by laps (descending), then by progress (descending); stable
        // sort preserves BTreeMap order for exact ties.
        new_finishers.sort_by(|a, b| {
            b.1.cmp(&a.1)
                .then_with(|| b.2.partial_cmp(&a.2).unwrap_or(std::cmp::Ordering::Equal))
        });

        for (offset, (player_id, _, _)) in new_finishers.iter().enumerate() {
            if let Some(state) = self.session.participants.get_mut(player_id) {
                state.finish_position = Some(assigned + offset as u8 + 1);
            }
        }

        if assigned == 0 {
            // The winner is in: the rest of the field is on the clock.
            let race_ticks = self
                .session
                .current_tick
                .saturating_sub(self.session.race_start_tick.unwrap_or(0));
            let average_lap_ticks = race_ticks as f32 / lap_limit.max(1) as f32;
            let grace_ticks = ((average_lap_ticks * FINISH_GRACE_LAPS) as u32)
                .max(FINISH_GRACE_MIN_SECONDS * self.tick_rate_hz as u32);
            self.finish_deadline_tick = Some(self.session.current_tick + grace_ticks);
        }

        // Humans who just finished hand their car to the cool-down driver,
        // which steers the rack directly and plans on the car's own speeds.
        for (player_id, _, _) in &new_finishers {
            if self.is_ai_player(player_id) {
                continue;
            }
            let Some(state) = self.session.participants.get_mut(player_id) else {
                continue;
            };
            self.held_steering_assist
                .insert(*player_id, state.steering_assist);
            state.steering_assist = false;
            let car_id = state.car_config_id;
            if !self.ai_speed_profiles.contains_key(&car_id) {
                if let Some(speeds) = self
                    .car_configs
                    .get(&car_id)
                    .and_then(|car| racing_line::build(&self.track_config, car))
                {
                    self.ai_speed_profiles.insert(car_id, speeds);
                }
            }
        }
    }

    /// Spawn AI drivers using the provided profiles.
    ///
    /// AI drivers will be added to the session up to the configured ai_count.
    /// Each AI uses their preferred car (if set) or the default car.
    pub fn spawn_ai_drivers(&mut self) {
        let current_ai_count = self.session.ai_player_ids.len() as u8;
        let ai_to_spawn = self.session.ai_count.saturating_sub(current_ai_count);

        if ai_to_spawn == 0 {
            return;
        }

        // Collect profile data we need before mutating self
        let profiles_to_spawn: Vec<(PlayerId, Option<CarConfigId>)> = self
            .ai_profiles
            .values()
            .filter(|p| !self.session.ai_player_ids.contains(&p.id))
            .take(ai_to_spawn as usize)
            .map(|p| (p.id, p.preferred_car_id))
            .collect();

        // The field races in the host car's class; without a host car, the
        // smallest id anchors it (HashMap order varies per run).
        let anchor = self
            .session
            .host_car_id
            .filter(|id| self.car_configs.contains_key(id))
            .or_else(|| self.car_configs.keys().min().copied());
        let Some(anchor) = anchor else {
            tracing::error!("Cannot spawn AI drivers: no car configurations loaded");
            return;
        };
        let field = class_field(&self.car_configs, anchor);
        // Carry on the rotation from the AI already on the grid, so a later
        // spawn does not stack the same car again.
        let mut next_car = self.session.ai_player_ids.len();

        for (ai_id, preferred_car) in profiles_to_spawn {
            if self.session.participants.len() >= self.session.max_players as usize {
                break;
            }

            let car_id = preferred_car.unwrap_or_else(|| {
                let car = field[next_car % field.len()];
                next_car += 1;
                car
            });

            if self.add_player(ai_id, car_id).is_some() {
                self.session.ai_player_ids.push(ai_id);
                if !self.ai_speed_profiles.contains_key(&car_id) {
                    if let Some(speeds) = self
                        .car_configs
                        .get(&car_id)
                        .and_then(|car| racing_line::build(&self.track_config, car))
                    {
                        self.ai_speed_profiles.insert(car_id, speeds);
                    }
                }
            }
        }
    }

    /// Add AI profiles to the session.
    ///
    /// This should be called when setting up the session in the lobby.
    pub fn set_ai_profiles(&mut self, profiles: Vec<AiDriverProfile>) {
        self.ai_profiles = profiles.into_iter().map(|p| (p.id, p)).collect();
    }
}

/// The cars an AI field is dealt from, in dealing order: every car of
/// `anchor`'s class, ordered by id and starting just after `anchor`, so a
/// GT3 host races a mixed GT3 field with its own model coming round last.
/// A car with no class races only against itself. Never empty: `anchor` is
/// always in it.
pub fn class_field(
    car_configs: &HashMap<CarConfigId, CarConfig>,
    anchor: CarConfigId,
) -> Vec<CarConfigId> {
    let class = car_configs
        .get(&anchor)
        .map(|car| car.class.trim())
        .unwrap_or_default();
    let mut pool: Vec<CarConfigId> = if class.is_empty() {
        vec![anchor]
    } else {
        car_configs
            .values()
            .filter(|car| car.class.trim().eq_ignore_ascii_case(class))
            .map(|car| car.id)
            .collect()
    };
    if !pool.contains(&anchor) {
        pool.push(anchor);
    }
    pool.sort();
    let at = pool.iter().position(|id| *id == anchor).unwrap_or(0);
    let len = pool.len();
    pool.rotate_left((at + 1) % len);
    pool
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn create_test_session() -> GameSession {
        let track = TrackConfig::default();
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 0, 3);

        GameSession::new(session, track, car_configs)
    }

    #[test]
    fn test_game_session_creation() {
        let game_session = create_test_session();
        assert_eq!(game_session.session.state, SessionState::Lobby);
        assert_eq!(game_session.session.current_tick, 0);
    }

    #[test]
    fn test_add_player() {
        let mut game_session = create_test_session();
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;

        let position = game_session.add_player(player_id, car_id);
        assert!(position.is_some());
        assert_eq!(position.unwrap(), 1);
        assert_eq!(game_session.session.participants.len(), 1);
    }

    #[test]
    fn test_start_countdown() {
        let mut game_session = create_test_session();
        game_session.start_countdown();

        assert_eq!(game_session.session.state, SessionState::Countdown);
        assert!(game_session.session.countdown_ticks_remaining.is_some());
    }

    #[test]
    fn test_tick_countdown() {
        let mut game_session = create_test_session();
        // Use the new game mode system
        game_session.set_game_mode(GameMode::Countdown);

        let initial_countdown = game_session.session.countdown_ticks_remaining.unwrap();

        let inputs = HashMap::new();
        game_session.tick(&inputs);

        assert_eq!(
            game_session.session.countdown_ticks_remaining.unwrap(),
            initial_countdown - 1
        );
    }

    #[test]
    fn test_car_setup_tunes_only_that_driver() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let tuned_driver = Uuid::new_v4();
        let stock_driver = Uuid::new_v4();
        game_session.add_player(tuned_driver, car_id);
        game_session.add_player(stock_driver, car_id);
        let base_spring = game_session.car_configs[&car_id]
            .suspension
            .spring_rate_front_n_per_m;

        // Out of range on purpose: the applied setup is the clamped one.
        let asked = CarSetup {
            spring_front: 9,
            rev_limiter: 2,
            ..Default::default()
        };
        let applied = game_session.set_car_setup(&tuned_driver, asked).unwrap();
        assert_eq!(applied.spring_front, 5);
        assert_eq!(applied.rev_limiter, 0);
        assert_eq!(game_session.car_setup(&tuned_driver), applied);

        let tuned = game_session.simulated_config_for(&tuned_driver).unwrap();
        assert!((tuned.suspension.spring_rate_front_n_per_m - base_spring * 1.2).abs() < 1e-2);
        let stock = game_session.simulated_config_for(&stock_driver).unwrap();
        assert_eq!(stock.suspension.spring_rate_front_n_per_m, base_spring);
        assert_eq!(
            game_session.car_configs[&car_id]
                .suspension
                .spring_rate_front_n_per_m,
            base_spring
        );

        // A stock setup drops the tuned copy; so does leaving.
        game_session.set_car_setup(&tuned_driver, CarSetup::default());
        assert!(game_session.tuned_configs.is_empty());
        assert!(game_session.car_setup(&tuned_driver).is_stock());
        game_session.set_car_setup(&tuned_driver, applied);
        game_session.remove_player(&tuned_driver);
        assert!(game_session.tuned_configs.is_empty());

        // Nobody's car: nothing applied.
        assert!(game_session
            .set_car_setup(&Uuid::new_v4(), applied)
            .is_none());
    }

    #[test]
    fn test_car_setup_changes_the_simulated_car() {
        // Two identical drivers, one with a shorter final drive: after a
        // second at full throttle from the grid the tuned car has pulled a
        // different speed, so the tick loop really reads the tuned copy.
        let mut a = create_test_session();
        let car_id = a.car_configs.values().next().unwrap().id;
        let driver = Uuid::new_v4();
        a.add_player(driver, car_id);
        let mut b = create_test_session();
        b.add_player(driver, car_id);
        b.set_car_setup(
            &driver,
            CarSetup {
                torque_map: -5,
                ..Default::default()
            },
        );
        for session in [&mut a, &mut b] {
            session.set_game_mode(GameMode::FreePractice);
        }
        let mut inputs = HashMap::new();
        inputs.insert(
            driver,
            PlayerInputData {
                throttle: 1.0,
                gear: Some(1),
                ..Default::default()
            },
        );
        for _ in 0..240 {
            a.tick(&inputs);
            b.tick(&inputs);
        }
        let speed = |s: &GameSession| s.session.participants[&driver].speed_mps;
        assert!(speed(&a) > 1.0, "the stock car moved: {}", speed(&a));
        assert!(
            speed(&b) < speed(&a),
            "80% torque should be slower: tuned {} vs stock {}",
            speed(&b),
            speed(&a)
        );
    }

    #[test]
    fn test_ai_input_generation() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;

        // Create an AI profile
        let ai_profile = AiDriverProfile::new("Test AI", 90);
        let ai_player_id = ai_profile.id;

        // Add AI profile to the session
        game_session.set_ai_profiles(vec![ai_profile]);

        // Add the AI player to the session
        game_session.add_player(ai_player_id, car_id);

        let ai_input = game_session.generate_ai_input(&ai_player_id);

        // AI should generate valid inputs
        assert!(ai_input.throttle >= 0.0 && ai_input.throttle <= 1.0);
        assert!(ai_input.brake >= 0.0 && ai_input.brake <= 1.0);
        assert!(ai_input.steering >= -1.0 && ai_input.steering <= 1.0);
    }

    #[test]
    fn test_ai_spawn_with_profiles() {
        use crate::ai_driver::generate_default_ai_profiles;

        let track = TrackConfig::default();
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        // Create session with 2 AI drivers
        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 2, 3);
        let ai_profiles = generate_default_ai_profiles(2);

        let mut game_session =
            GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);

        // Spawn AI drivers
        game_session.spawn_ai_drivers();

        // Should have 2 AI participants
        assert_eq!(game_session.session.participants.len(), 2);
        assert_eq!(game_session.session.ai_player_ids.len(), 2);

        // All should be recognized as AI players
        for ai_id in &game_session.session.ai_player_ids {
            assert!(game_session.is_ai_player(ai_id));
        }
    }

    #[test]
    fn roster_carries_picked_liveries_and_deals_the_ai_theirs() {
        use crate::ai_driver::generate_default_ai_profiles;

        let track = TrackConfig::default();
        let car = CarConfig {
            livery_names: vec!["Blue".to_string(), "Gold".to_string()],
            ..CarConfig::default()
        };
        let car_id = car.id;
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car);
        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 4, 3);
        let mut game_session = GameSession::with_ai_profiles(
            session,
            track,
            car_configs,
            generate_default_ai_profiles(4),
        );
        game_session.spawn_ai_drivers();
        let (picked, stale, stock) = (Uuid::new_v4(), Uuid::new_v4(), Uuid::new_v4());
        for p in [picked, stale, stock] {
            game_session.add_player(p, car_id);
        }
        game_session.set_livery(picked, 2);
        game_session.set_livery(stale, 7);

        let roster = game_session.build_roster(&HashMap::new());
        let livery_of = |id: PlayerId| {
            roster
                .entries
                .iter()
                .find(|e| e.player_id == id)
                .unwrap()
                .livery
        };
        assert_eq!(livery_of(picked), 2, "a pick inside the car's list is kept");
        assert_eq!(
            livery_of(stale),
            0,
            "a pick past the end falls back to the car as authored"
        );
        assert_eq!(livery_of(stock), 0);
        let mut ai: Vec<u8> = roster
            .entries
            .iter()
            .filter(|e| e.is_ai)
            .map(|e| e.livery)
            .collect();
        ai.sort();
        assert_eq!(
            ai,
            vec![0, 0, 1, 2],
            "four AI in one model wear its three liveries in turn"
        );

        game_session.remove_player(&picked);
        game_session.add_player(picked, car_id);
        let roster = game_session.build_roster(&HashMap::new());
        assert_eq!(
            roster
                .entries
                .iter()
                .find(|e| e.player_id == picked)
                .unwrap()
                .livery,
            0,
            "leaving the session forgets the pick"
        );
    }

    // --- Game Mode Tests ---

    #[test]
    fn test_default_game_mode_is_lobby() {
        let game_session = create_test_session();
        assert_eq!(game_session.session.game_mode, GameMode::Lobby);
    }

    #[test]
    fn test_lobby_mode_tick() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Lobby);

        let initial_tick = game_session.session.current_tick;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Tick counter should increment
        assert_eq!(game_session.session.current_tick, initial_tick + 1);

        // No participants should move in lobby mode
        assert_eq!(game_session.session.participants.len(), 0);
    }

    #[test]
    fn test_sandbox_mode_tick() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Sandbox);

        // Add a player
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        let initial_pos = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Position should not change in sandbox mode
        let final_pos = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;
        assert_eq!(initial_pos, final_pos);
    }

    #[test]
    fn test_countdown_mode_decrements() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);

        let initial_countdown = game_session.session.countdown_ticks_remaining.unwrap();
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        assert_eq!(
            game_session.session.countdown_ticks_remaining.unwrap(),
            initial_countdown - 1
        );
    }

    #[test]
    fn test_countdown_mode_finishes() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);
        game_session.session.countdown_ticks_remaining = Some(1);

        let inputs = HashMap::new();
        game_session.tick(&inputs); // Decrements to 0

        // Should be 0 now
        assert_eq!(game_session.session.countdown_ticks_remaining, Some(0));

        game_session.tick(&inputs); // Clears to None

        // Countdown should be None after finishing
        assert_eq!(game_session.session.countdown_ticks_remaining, None);
    }

    #[test]
    fn test_demolap_mode_initializes_progress() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        game_session.set_game_mode(GameMode::DemoLap);

        assert!(game_session.session.demo_lap_progress.is_some());
        assert_eq!(game_session.session.demo_lap_progress.unwrap(), 0.0);
    }

    #[test]
    fn test_demolap_mode_advances_progress() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        // Add racing line to track
        game_session.track_config.raceline = vec![
            RacelinePoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 100.0,
                y: 100.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 0.0,
                y: 100.0,
                z: 0.0,
            },
        ];

        game_session.set_game_mode(GameMode::DemoLap);

        // With the AI-driven demo lap, we check that the AI car moves
        // (demo_lap_progress is only used in the camera-following fallback)
        let ai_player_id = *game_session
            .session
            .ai_player_ids
            .first()
            .expect("DemoLap should spawn an AI driver");

        let initial_pos = game_session
            .session
            .participants
            .get(&ai_player_id)
            .map(|s| (s.pos_x, s.pos_y))
            .unwrap();

        let inputs = HashMap::new();

        // Run enough ticks for the car to launch from standstill and move
        // 240 ticks = 1 second of simulation time at 240Hz
        for _ in 0..240 {
            game_session.tick(&inputs);
        }

        // AI car should have moved
        let final_pos = game_session
            .session
            .participants
            .get(&ai_player_id)
            .map(|s| (s.pos_x, s.pos_y))
            .unwrap();

        let distance_moved =
            ((final_pos.0 - initial_pos.0).powi(2) + (final_pos.1 - initial_pos.1).powi(2)).sqrt();

        assert!(
            distance_moved > 0.1,
            "AI car should have moved in DemoLap mode, moved: {}m",
            distance_moved
        );
    }

    #[test]
    fn test_demolap_mode_loops() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        // Add racing line
        game_session.track_config.raceline = vec![
            RacelinePoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
            },
        ];

        game_session.set_game_mode(GameMode::DemoLap);

        // With AI-driven demo lap, verify the AI driver exists and is moving
        assert!(
            !game_session.session.ai_player_ids.is_empty(),
            "DemoLap should create an AI driver"
        );

        let ai_player_id = *game_session.session.ai_player_ids.first().unwrap();

        let inputs = HashMap::new();

        // Run simulation for enough ticks to allow the car to launch from standstill
        // (physics now supports generating tire force from rest via slip ratio)
        for _ in 0..100 {
            game_session.tick(&inputs);
        }

        // AI should be driving (speed > 0) after launching from standstill
        let ai_state = game_session
            .session
            .participants
            .get(&ai_player_id)
            .unwrap();
        assert!(
            ai_state.speed_mps > 0.0,
            "AI should gain speed during demo lap (launched from standstill)"
        );
    }

    #[test]
    fn test_free_practice_mode_updates_physics() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::FreePractice);

        // Add a player
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        let initial_pos_x = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;

        // Apply throttle input
        let mut inputs = HashMap::new();
        inputs.insert(
            player_id,
            PlayerInputData {
                throttle: 1.0,
                brake: 0.0,
                steering: 0.0,
                gear: None,
                clutch: None,
                drs: false,
                headlights: None,
                flash: false,
            },
        );

        // Run several ticks to allow physics to update
        for _ in 0..240 {
            game_session.tick(&inputs);
        }

        // Car should have moved after applying throttle for 1 second
        let final_pos_x = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;
        assert_ne!(initial_pos_x, final_pos_x);
    }

    #[test]
    fn test_set_game_mode() {
        let mut game_session = create_test_session();

        game_session.set_game_mode(GameMode::Sandbox);
        assert_eq!(game_session.session.game_mode, GameMode::Sandbox);

        game_session.set_game_mode(GameMode::Countdown);
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert!(game_session.session.countdown_ticks_remaining.is_some());

        game_session.set_game_mode(GameMode::DemoLap);
        assert_eq!(game_session.session.game_mode, GameMode::DemoLap);
        assert!(game_session.session.demo_lap_progress.is_some());
    }

    #[test]
    fn test_start_countdown_mode() {
        let mut game_session = create_test_session();

        game_session.start_countdown_mode(10, GameMode::FreePractice);

        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(
            game_session.session.countdown_ticks_remaining,
            Some(240 * 10)
        );
    }

    /// Two humans racing on the default oval, lap limit 3.
    fn racing_session_with_two_humans() -> (GameSession, PlayerId, PlayerId) {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.add_player(b, car_id).unwrap();
        game_session.set_game_mode(GameMode::Race);
        (game_session, a, b)
    }

    #[test]
    fn test_race_grid_waits_in_neutral_and_revs_until_the_green_light() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (auto, manual) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(auto, car_id).unwrap();
        game_session.add_player(manual, car_id).unwrap();
        game_session
            .session
            .participants
            .get_mut(&auto)
            .unwrap()
            .auto_gearbox = true;
        game_session.start_countdown_mode(2, GameMode::Race);
        let config = game_session.car_configs[&car_id].clone();
        let start = |s: &GameSession, id: &PlayerId| {
            let car = &s.session.participants[id];
            (car.pos_x, car.pos_y, car.gear)
        };
        assert_eq!(start(&game_session, &auto).2, 0);
        assert_eq!(start(&game_session, &manual).2, 0);
        let grid = (start(&game_session, &auto), start(&game_session, &manual));

        // Flat out on the grid for a second: the engines rev, nothing moves,
        // and the automatic box stays in neutral.
        let full = PlayerInputData {
            throttle: 1.0,
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> = [(auto, full), (manual, full)].into();
        for _ in 0..240 {
            game_session.tick(&inputs);
        }
        let car = &game_session.session.participants[&auto];
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(car.gear, 0);
        assert!(
            car.engine_rpm > config.redline_rpm * 0.8,
            "revving on the grid, rpm={}",
            car.engine_rpm
        );
        assert!((car.throttle_input - 1.0).abs() < 1e-6);
        assert_eq!(
            (start(&game_session, &auto), start(&game_session, &manual)),
            grid
        );

        // The manual driver selects first on the grid, then waits.
        let first = PlayerInputData {
            gear: Some(1),
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(auto, PlayerInputData::default()), (manual, first)].into();
        game_session.tick(&inputs);
        assert_eq!(game_session.session.participants[&manual].gear, 1);
        let coast: HashMap<PlayerId, PlayerInputData> = HashMap::new();
        while game_session.session.game_mode == GameMode::Countdown {
            game_session.tick(&coast);
        }

        // Green: the automatic box takes first by itself.
        assert_eq!(game_session.session.game_mode, GameMode::Race);
        assert_eq!(game_session.session.participants[&auto].gear, 1);
        assert_eq!(game_session.session.participants[&manual].gear, 1);
    }

    #[test]
    fn test_winner_finishing_starts_the_clock_on_the_rest() {
        let (mut game_session, winner, _) = racing_session_with_two_humans();
        game_session
            .session
            .participants
            .get_mut(&winner)
            .unwrap()
            .current_lap = 4;

        game_session.tick(&HashMap::new());
        assert_eq!(
            game_session.session.participants[&winner].finish_position,
            Some(1)
        );
        assert_eq!(game_session.session.state, SessionState::Racing);

        // A quick race: the floor applies.
        let deadline = game_session.finish_deadline_tick.expect("deadline set");
        assert!(deadline >= game_session.session.current_tick + FINISH_GRACE_MIN_SECONDS * 240);

        game_session.session.current_tick = deadline - 1;
        game_session.tick(&HashMap::new());
        assert_eq!(
            game_session.session.state,
            SessionState::Finished,
            "the race ends at the deadline with the second car unclassified"
        );
    }

    #[test]
    fn test_finished_human_is_driven_by_the_server_and_gets_aids_back() {
        let (mut game_session, winner, _) = racing_session_with_two_humans();
        let car = game_session.session.participants.get_mut(&winner).unwrap();
        car.current_lap = 4;
        car.steering_assist = true;

        // The client's last input stays in the server's map after it stops
        // sending: here, standing on the brake. Only the cool-down driver can
        // get the car moving.
        let stale = PlayerInputData {
            brake: 1.0,
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> = [(winner, stale)].into();
        for _ in 0..480 {
            game_session.tick(&inputs);
        }
        let car = &game_session.session.participants[&winner];
        assert!(car.finish_position.is_some());
        assert!(
            car.speed_mps > 2.0,
            "the stale input must not be driving the car, speed={}",
            car.speed_mps
        );
        assert!(
            !car.steering_assist,
            "the cool-down driver steers without the aid"
        );

        game_session.start_countdown_mode(5, GameMode::Race);
        let car = &game_session.session.participants[&winner];
        assert!(car.steering_assist, "lining up again restores the aid");
        assert_eq!(car.current_lap, 0);
        assert_eq!(car.finish_position, None);
        assert_eq!(game_session.finish_deadline_tick, None);
    }

    #[test]
    fn test_transition_from_countdown() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);

        game_session.transition_from_countdown(GameMode::FreePractice);

        assert_eq!(game_session.session.game_mode, GameMode::FreePractice);
        assert_eq!(game_session.session.countdown_ticks_remaining, None);
    }

    #[test]
    fn test_transition_from_countdown_to_demolap() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);

        game_session.transition_from_countdown(GameMode::DemoLap);

        assert_eq!(game_session.session.game_mode, GameMode::DemoLap);
        assert!(game_session.session.demo_lap_progress.is_some());
    }

    #[test]
    fn test_replay_mode_does_nothing() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Replay);

        let initial_tick = game_session.session.current_tick;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Tick should increment but nothing else happens
        assert_eq!(game_session.session.current_tick, initial_tick + 1);
    }

    #[test]
    fn test_demolap_without_raceline() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        // Clear raceline
        game_session.track_config.raceline.clear();

        game_session.set_game_mode(GameMode::DemoLap);

        let inputs = HashMap::new();
        game_session.tick(&inputs);

        // Should not crash when raceline is empty
        assert_eq!(game_session.session.game_mode, GameMode::DemoLap);
    }

    #[test]
    fn test_mode_persists_across_ticks() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Sandbox);

        let inputs = HashMap::new();
        for _ in 0..10 {
            game_session.tick(&inputs);
        }

        // Mode should still be Sandbox
        assert_eq!(game_session.session.game_mode, GameMode::Sandbox);
    }

    #[test]
    fn test_ai_driver_integration_demo_lap() {
        use crate::ai_driver::generate_default_ai_profiles;

        // Create a track with a simple racing line
        let mut track = TrackConfig::default();

        // Create a simple circular track layout for testing
        // 100m radius circle, 628m total length
        let num_points = 32;
        let radius = 100.0;
        let mut raceline = Vec::new();
        let mut centerline = Vec::new();

        for i in 0..num_points {
            let angle = (i as f32 / num_points as f32) * 2.0 * std::f32::consts::PI;
            let x = angle.cos() * radius;
            let y = angle.sin() * radius;
            let distance = (i as f32 / num_points as f32) * 2.0 * std::f32::consts::PI * radius;

            raceline.push(RacelinePoint { x, y, z: 0.0 });
            centerline.push(crate::data::TrackPoint {
                x,
                y,
                z: 0.0,
                distance_from_start_m: distance,
                width_left_m: 10.0,
                width_right_m: 10.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: angle + std::f32::consts::FRAC_PI_2,
                grip_modifier: 1.0,
                surface_type: SurfaceType::Asphalt,
            });
        }

        track.raceline = raceline;
        track.centerline = centerline;
        track.rebuild_raceline_distances();

        // Create session with one AI driver
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Practice, 8, 1, 1);
        let ai_profiles = generate_default_ai_profiles(1);

        let mut game_session =
            GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);

        // Spawn the AI driver
        game_session.spawn_ai_drivers();
        assert_eq!(game_session.session.participants.len(), 1);

        // Get the AI player ID
        let ai_player_id = *game_session.session.ai_player_ids.first().unwrap();

        // Set to FreePractice mode to test AI driving
        game_session.set_game_mode(GameMode::FreePractice);

        // Give the AI car initial speed to avoid stall (physics limitation)
        // In a real sim, clutch modulation would handle launch
        if let Some(ai_state) = game_session.session.participants.get_mut(&ai_player_id) {
            ai_state.speed_mps = 5.0; // Start with 5 m/s (18 km/h)
            ai_state.vel_x = 5.0;
            ai_state.engine_rpm = 2000.0; // Start engine above idle
        }

        // Run simulation for 2 seconds (480 ticks at 240Hz)
        for _ in 0..480 {
            // Generate AI inputs for all AI players
            let mut inputs = HashMap::new();
            for ai_id in &game_session.session.ai_player_ids {
                let ai_input = game_session.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }
            game_session.tick(&inputs);
        }

        // Verify AI driver state
        let ai_state = game_session
            .session
            .participants
            .get(&ai_player_id)
            .unwrap();

        // AI should have moved from starting position
        let start_pos = &game_session.track_config.start_positions[0];
        let distance_moved = ((ai_state.pos_x - start_pos.x).powi(2)
            + (ai_state.pos_y - start_pos.y).powi(2))
        .sqrt();

        assert!(
            distance_moved > 10.0,
            "AI should have moved at least 10m from start position (started at 5m/s), moved: {}m",
            distance_moved
        );

        // AI should have positive speed
        assert!(
            ai_state.speed_mps > 0.0,
            "AI should be moving, speed: {} m/s",
            ai_state.speed_mps
        );

        // AI position should be reasonably close to the track centerline
        // Find nearest track point
        let nearest_track_point = game_session
            .track_config
            .centerline
            .iter()
            .min_by_key(|p| {
                let dx = p.x - ai_state.pos_x;
                let dy = p.y - ai_state.pos_y;
                ((dx * dx + dy * dy) * 1000.0) as i32
            })
            .unwrap();

        let distance_from_centerline = ((ai_state.pos_x - nearest_track_point.x).powi(2)
            + (ai_state.pos_y - nearest_track_point.y).powi(2))
        .sqrt();

        // AI should stay within 50m of centerline (generous tolerance for test)
        assert!(
            distance_from_centerline < 50.0,
            "AI should stay close to track centerline, distance: {}m",
            distance_from_centerline
        );

        // AI should be generating valid inputs
        let ai_input = game_session.generate_ai_input(&ai_player_id);
        assert!(ai_input.throttle >= 0.0 && ai_input.throttle <= 1.0);
        assert!(ai_input.brake >= 0.0 && ai_input.brake <= 1.0);
        assert!(ai_input.steering >= -1.0 && ai_input.steering <= 1.0);
        assert!(ai_input.gear.is_some());

        // AI should be in a reasonable gear
        if let Some(gear) = ai_input.gear {
            assert!(
                (1..=6).contains(&gear),
                "AI gear should be between 1 and 6, got: {}",
                gear
            );
        }
    }

    #[test]
    fn test_ai_driver_follows_racing_line() {
        use crate::ai_driver::AiDriverProfile;

        // Create a straight track for easier validation
        let mut track = TrackConfig::default();

        // Create a 500m straight track
        let num_points = 50;
        let mut raceline = Vec::new();
        let mut centerline = Vec::new();

        for i in 0..num_points {
            let x = i as f32 * 10.0; // 10m spacing
            let y = 0.0;
            let distance = i as f32 * 10.0;

            raceline.push(RacelinePoint { x, y, z: 0.0 });
            centerline.push(crate::data::TrackPoint {
                x,
                y,
                z: 0.0,
                distance_from_start_m: distance,
                width_left_m: 10.0,
                width_right_m: 10.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0, // Straight track, heading east
                grip_modifier: 1.0,
                surface_type: SurfaceType::Asphalt,
            });
        }

        track.raceline = raceline;
        track.centerline = centerline;
        track.rebuild_raceline_distances();

        // Create high-skill AI (should be very precise)
        let ai_profile = AiDriverProfile::new("Test AI", 105);
        let ai_player_id = ai_profile.id;

        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Practice, 8, 1, 1);

        let mut game_session =
            GameSession::with_ai_profiles(session, track, car_configs, vec![ai_profile]);

        // Spawn AI and add to session
        game_session.spawn_ai_drivers();

        // Start in free practice mode
        game_session.set_game_mode(GameMode::FreePractice);

        // Give the AI car initial speed to avoid stall
        if let Some(ai_state) = game_session.session.participants.get_mut(&ai_player_id) {
            ai_state.speed_mps = 10.0; // Start with 10 m/s
            ai_state.vel_x = 10.0;
            ai_state.engine_rpm = 3000.0;
        }

        // Run for 3 seconds to let AI stabilize
        for _ in 0..720 {
            let mut inputs = HashMap::new();
            for ai_id in &game_session.session.ai_player_ids {
                let ai_input = game_session.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }
            game_session.tick(&inputs);
        }

        // Check AI position over next 1 second, verifying it stays on line
        let mut max_lateral_deviation = 0.0f32;

        for _ in 0..240 {
            let mut inputs = HashMap::new();
            for ai_id in &game_session.session.ai_player_ids {
                let ai_input = game_session.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }
            game_session.tick(&inputs);

            if let Some(ai_state) = game_session.session.participants.get(&ai_player_id) {
                // Y should be close to 0 for straight track
                let lateral_deviation = ai_state.pos_y.abs();
                max_lateral_deviation = max_lateral_deviation.max(lateral_deviation);
            }
        }

        // High-skill AI should stay within 20m of the racing line on a straight
        assert!(
            max_lateral_deviation < 20.0,
            "High-skill AI should stay close to racing line, max deviation: {}m",
            max_lateral_deviation
        );

        // Verify AI is making forward progress
        let final_state = game_session
            .session
            .participants
            .get(&ai_player_id)
            .unwrap();
        assert!(
            final_state.pos_x > 50.0,
            "AI should have made significant forward progress, x position: {}m",
            final_state.pos_x
        );
    }

    fn classed_car(class: &str, id: u128) -> CarConfig {
        CarConfig {
            id: Uuid::from_u128(id),
            class: class.to_string(),
            ..CarConfig::default()
        }
    }

    #[test]
    fn class_field_deals_the_host_class_starting_after_the_host() {
        let cars: HashMap<CarConfigId, CarConfig> = [
            classed_car("GT3", 1),
            classed_car("LMP2", 2),
            classed_car("gt3", 3),
            classed_car("GT3", 4),
            classed_car("", 5),
        ]
        .into_iter()
        .map(|c| (c.id, c))
        .collect();

        let gt3 = class_field(&cars, Uuid::from_u128(3));
        assert_eq!(
            gt3,
            vec![Uuid::from_u128(4), Uuid::from_u128(1), Uuid::from_u128(3)],
            "every GT3 (class compared case-blind), host's model last"
        );
        assert_eq!(
            class_field(&cars, Uuid::from_u128(2)),
            vec![Uuid::from_u128(2)]
        );
        assert_eq!(
            class_field(&cars, Uuid::from_u128(5)),
            vec![Uuid::from_u128(5)],
            "an unclassed car races only against itself"
        );
    }

    #[test]
    fn ai_field_is_a_mix_of_the_host_class() {
        let cars = [
            classed_car("GT3", 1),
            classed_car("GT3", 2),
            classed_car("LMP2", 3),
            classed_car("GT3", 4),
        ];
        let car_configs: HashMap<CarConfigId, CarConfig> =
            cars.iter().map(|c| (c.id, c.clone())).collect();
        let track = TrackConfig::default();
        let mut session =
            RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 5, 3);
        session.host_car_id = Some(Uuid::from_u128(2));
        let mut game_session = GameSession::with_ai_profiles(
            session,
            track,
            car_configs,
            crate::ai_driver::generate_default_ai_profiles(5),
        );
        game_session.spawn_ai_drivers();

        let driven: Vec<CarConfigId> = game_session
            .session
            .ai_player_ids
            .iter()
            .map(|id| game_session.session.participants[id].car_config_id)
            .collect();
        assert_eq!(driven.len(), 5);
        assert!(
            !driven.contains(&Uuid::from_u128(3)),
            "no LMP2 in a GT3 field"
        );
        for gt3 in [1, 2, 4] {
            assert!(
                driven.contains(&Uuid::from_u128(gt3)),
                "GT3 {gt3} is on the grid"
            );
        }

        let roster = game_session.build_roster(&HashMap::new());
        for entry in &roster.entries {
            assert_eq!(
                entry.car_config_id,
                game_session.session.participants[&entry.player_id].car_config_id
            );
        }
    }

    #[test]
    fn test_demo_driver_uses_host_car() {
        // Create a session with a specific host car
        let track = TrackConfig::default();
        let car1 = CarConfig::default();
        let car2 = CarConfig {
            id: Uuid::new_v4(), // Different ID
            name: "Test Car 2".to_string(),
            ..CarConfig::default()
        };

        let mut car_configs = HashMap::new();
        car_configs.insert(car1.id, car1.clone());
        car_configs.insert(car2.id, car2.clone());

        let host_id = Uuid::new_v4();
        let mut session = RaceSession::new(host_id, track.id, SessionKind::Multiplayer, 8, 0, 3);
        session.host_car_id = Some(car2.id); // Host selected car2

        let mut game_session = GameSession::new(session, track, car_configs);

        // Set to DemoLap mode, which should create a demo driver with the host's car
        game_session.set_game_mode(GameMode::DemoLap);

        // Verify demo driver was created
        assert_eq!(game_session.session.ai_player_ids.len(), 1);

        // Verify the demo driver has a car state
        let demo_driver_id = game_session.session.ai_player_ids[0];
        let car_state = game_session
            .session
            .participants
            .get(&demo_driver_id)
            .expect("Demo driver should have a car state");

        // Verify the demo driver is using the host's selected car (car2)
        assert_eq!(
            car_state.car_config_id, car2.id,
            "Demo driver should use the host's selected car"
        );
    }

    /// A session that forbids an aid pins it off for every driver, whatever
    /// they ask for and whatever the car's own file says; one that allows
    /// them passes the request through.
    #[test]
    fn a_session_forbidding_aids_pins_them_off() {
        let everything = DriverAids {
            auto_gearbox: true,
            steering_assist: true,
            abs: Some(true),
            traction_control: Some(TractionControl::High),
        };

        let mut strict = create_test_session();
        strict.session.allowed_assists = AllowedAssists::NONE;
        let player = Uuid::new_v4();
        let car_id = strict.car_configs.values().next().unwrap().id;
        strict.add_player(player, car_id).unwrap();
        let seated = strict.session.participants[&player].driver_aids();
        assert_eq!(
            seated.abs,
            Some(false),
            "a client that never sends its aids must not get the car's ABS"
        );
        assert_eq!(seated.traction_control, Some(TractionControl::Off));

        let applied = strict.set_driver_aids(&player, everything).unwrap();
        assert_eq!(
            applied,
            DriverAids {
                auto_gearbox: false,
                steering_assist: false,
                abs: Some(false),
                traction_control: Some(TractionControl::Off),
            }
        );
        assert_eq!(strict.session.participants[&player].driver_aids(), applied);

        strict.line_up_on_grid();
        assert_eq!(
            strict.session.participants[&player].driver_aids(),
            applied,
            "lining up again keeps the clamped aids"
        );

        let mut open = create_test_session();
        open.add_player(player, car_id).unwrap();
        let seated = open.session.participants[&player].driver_aids();
        assert_eq!(
            seated.abs, None,
            "allowed: the car's own until the client says"
        );
        assert_eq!(open.set_driver_aids(&player, everything), Some(everything));

        let mut no_tc = create_test_session();
        no_tc.session.allowed_assists = AllowedAssists {
            traction_control: false,
            ..AllowedAssists::ALL
        };
        no_tc.add_player(player, car_id).unwrap();
        let applied = no_tc.set_driver_aids(&player, everything).unwrap();
        assert_eq!(applied.traction_control, Some(TractionControl::Off));
        assert_eq!(applied.abs, Some(true));
        assert!(applied.auto_gearbox && applied.steering_assist);
        assert_eq!(no_tc.set_driver_aids(&Uuid::new_v4(), everything), None);
    }
}

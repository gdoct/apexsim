use crate::ai_driver::{AiDriverController, AiDriverProfile};
use crate::data::*;
use crate::network::*;
use crate::physics;
use std::collections::HashMap;
use tracing::debug;

/// Default simulation tick rate; used when no explicit rate is configured
/// (tests, benches) so behavior matches the historical hardcoded 240Hz.
pub const DEFAULT_TICK_RATE_HZ: u16 = 240;

pub struct GameSession {
    pub session: RaceSession,
    pub track_config: TrackConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
    /// AI driver profiles indexed by their player ID
    pub ai_profiles: std::collections::BTreeMap<PlayerId, AiDriverProfile>,
    /// Simulation tick rate (Hz); the fixed timestep is `1 / tick_rate_hz`.
    tick_rate_hz: u16,
    /// Set when session membership changed since the last roster broadcast;
    /// starts true so the roster goes out once when the session first ticks.
    roster_dirty: bool,
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

        // Handle game mode specific logic
        match self.session.game_mode {
            GameMode::Lobby => {
                self.tick_lobby();
            }
            GameMode::Sandbox => {
                self.tick_sandbox();
            }
            GameMode::Countdown => {
                self.tick_countdown();
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
    fn tick_countdown(&mut self) {
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
        // Players are frozen, no physics updates
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
            let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();
            for state in states.iter_mut() {
                let input = inputs.get(&state.player_id).copied().unwrap_or_default();

                if let Some(config) = self.car_configs.get(&state.car_config_id) {
                    physics::update_car_3d(state, config, &input, &self.track_config, dt);
                    physics::update_track_progress_3d(
                        state,
                        &self.track_config,
                        self.session.current_tick,
                        self.tick_rate_hz,
                    );
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
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // Update each car
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();

            // Get car config
            if let Some(config) = self.car_configs.get(&state.car_config_id) {
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                // Update track progress
                physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    self.session.current_tick,
                    self.tick_rate_hz,
                );
            }
        }

        // Check collisions in place (BTreeMap iteration order makes the
        // order-dependent solver deterministic; no clone/rebuild needed)
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
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
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // Update each car
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();

            // Get car config
            if let Some(config) = self.car_configs.get(&state.car_config_id) {
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                // Update track progress
                physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    self.session.current_tick,
                    self.tick_rate_hz,
                );
            }
        }

        // Check collisions in place (BTreeMap iteration order makes the
        // order-dependent solver deterministic; no clone/rebuild needed)
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);

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
            }
            GameMode::Qualification => {
                // Practice-with-timing: telemetry must flow
                self.session.state = SessionState::Racing;
                self.session.demo_lap_progress = None;
            }
            _ => {
                self.session.demo_lap_progress = None;
            }
        }
    }

    /// Start countdown mode with custom duration. The stored `next_mode` is
    /// transitioned to automatically when the countdown reaches zero.
    pub fn start_countdown_mode(&mut self, countdown_seconds: u16, next_mode: GameMode) {
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
            let car_state = CarState::new(player_id, car_config_id, grid_slot);
            self.session.participants.insert(player_id, car_state);
            self.roster_dirty = true;
            Some(grid_position)
        } else {
            None
        }
    }

    /// Remove a player from the session
    pub fn remove_player(&mut self, player_id: &PlayerId) {
        if self.session.participants.remove(player_id).is_some() {
            self.roster_dirty = true;
        }
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
                    let controller =
                        AiDriverController::new(profile, &self.track_config, car_config);
                    return controller.generate_input(
                        state,
                        self.session.current_tick,
                        self.tick_rate_hz,
                    );
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
        let entries = self
            .session
            .participants
            .keys()
            .enumerate()
            .map(|(idx, player_id)| {
                let is_ai = self.ai_profiles.contains_key(player_id);
                let player_name = self
                    .ai_profiles
                    .get(player_id)
                    .map(|p| p.name.clone())
                    .or_else(|| names.get(player_id).cloned())
                    .unwrap_or_else(|| format!("Player-{}", &player_id.to_string()[..8]));
                RosterEntry {
                    car_index: idx as u8,
                    player_id: *player_id,
                    player_name,
                    is_ai,
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

        // Deterministic default car: HashMap iteration order varies per run,
        // so pick the smallest ID instead of whatever comes first.
        let default_car_id = self.car_configs.keys().min().copied();

        for (ai_id, preferred_car) in profiles_to_spawn {
            if self.session.participants.len() >= self.session.max_players as usize {
                break;
            }

            // Use preferred car or default; without any car configs we
            // cannot spawn AI at all.
            let Some(car_id) = preferred_car.or(default_car_id) else {
                tracing::error!("Cannot spawn AI drivers: no car configurations loaded");
                return;
            };

            if self.add_player(ai_id, car_id).is_some() {
                self.session.ai_player_ids.push(ai_id);
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
}

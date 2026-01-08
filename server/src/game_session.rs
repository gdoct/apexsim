use crate::ai_driver::{AiDriverController, AiDriverProfile};
use crate::data::*;
use crate::network::*;
use crate::physics;
use std::collections::HashMap;

pub struct GameSession {
    pub session: RaceSession,
    pub track_config: TrackConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
    /// AI driver profiles indexed by their player ID
    pub ai_profiles: HashMap<PlayerId, AiDriverProfile>,
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
            ai_profiles: HashMap::new(),
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
        let ai_profiles_map: HashMap<PlayerId, AiDriverProfile> = ai_profiles
            .into_iter()
            .map(|p| (p.id, p))
            .collect();
        
        Self {
            session,
            track_config,
            car_configs,
            ai_profiles: ai_profiles_map,
        }
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
                self.tick_demolap();
            }
            GameMode::FreePractice => {
                self.tick_free_practice(inputs);
            }
            GameMode::Replay => {
                self.tick_replay();
            }
            GameMode::Qualification | GameMode::Race => {
                // These modes are not yet implemented
                // For now, treat them like FreePractice
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
                // Countdown finished, transition to next mode
                // The mode transition should be specified externally
                // For now, we just clear the countdown
                self.session.countdown_ticks_remaining = None;
            }
        }
        // Players are frozen, no physics updates
    }

    /// Demo lap mode: Server drives a single demo car along the racing line
    fn tick_demolap(&mut self) {
        let dt = 1.0 / 240.0; // Fixed timestep at 240Hz

        // Initialize demo lap progress if not set
        if self.session.demo_lap_progress.is_none() {
            self.session.demo_lap_progress = Some(0.0);
        }

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
            
            // Speed control based on curvature
            // Max speed on straights: 80 m/s (288 km/h)
            // Min speed in tight corners: 30 m/s (108 km/h)
            let max_speed = 80.0;
            let min_speed = 30.0;
            
            // Map curvature (0 to ~PI) to speed range
            // High curvature (sharp corner) = low speed
            // Low curvature (straight) = high speed
            let curvature_factor = 1.0 - (curvature / std::f32::consts::PI).min(1.0);
            let target_speed = min_speed + (max_speed - min_speed) * curvature_factor;
            
            // Get current speed from demo car or use target speed
            let current_speed = self.session.participants.values()
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
        let dt = 1.0 / 240.0; // Fixed timestep at 240Hz

        // Update each car
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = inputs
                .get(&state.player_id)
                .copied()
                .unwrap_or_default();

            // Get car config
            if let Some(config) = self.car_configs.get(&state.car_config_id) {
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                // Update track progress
                physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    self.session.current_tick,
                );
            }
        }

        // Check collisions
        let mut state_vec: Vec<CarState> = self.session.participants.values().cloned().collect();
        physics::check_aabb_collisions_3d(&mut state_vec, &self.car_configs);

        // Update states back
        for state in state_vec {
            self.session.participants.insert(state.player_id, state);
        }
    }

    /// Replay mode: Send telemetry from recorded data (view-only)
    fn tick_replay(&mut self) {
        // Replay mode is not yet implemented
        // This would play back previously recorded telemetry data
        // For now, do nothing
    }

    #[allow(dead_code)]
    fn tick_racing(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        let dt = 1.0 / 240.0; // Fixed timestep at 240Hz

        // Update each car
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = inputs
                .get(&state.player_id)
                .copied()
                .unwrap_or_default();

            // Get car config
            if let Some(config) = self.car_configs.get(&state.car_config_id) {
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                // Update track progress
                physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    self.session.current_tick,
                );
            }
        }

        // Check collisions
        let mut state_vec: Vec<CarState> = self.session.participants.values().cloned().collect();
        physics::check_aabb_collisions_3d(&mut state_vec, &self.car_configs);

        // Update states back
        for state in state_vec {
            self.session.participants.insert(state.player_id, state);
        }

        // Check if race is complete
        if self.is_race_complete() {
            self.session.state = SessionState::Finished;
            self.assign_finish_positions();
        }
    }

    /// Start the countdown
    pub fn start_countdown(&mut self) {
        if self.session.state == SessionState::Lobby {
            self.session.state = SessionState::Countdown;
            self.session.countdown_ticks_remaining = Some(240 * 5); // 5 seconds at 240Hz
        }
    }

    /// Set the game mode
    pub fn set_game_mode(&mut self, mode: GameMode) {
        self.session.game_mode = mode;

        // Initialize mode-specific state
        match mode {
            GameMode::DemoLap => {
                self.session.demo_lap_progress = Some(0.0);
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;
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
                self.session.countdown_ticks_remaining = Some(240 * 10);
                self.session.state = SessionState::Countdown;
            }
            _ => {
                self.session.demo_lap_progress = None;
            }
        }
    }

    /// Start countdown mode with custom duration and specify next mode
    pub fn start_countdown_mode(&mut self, countdown_seconds: u16, _next_mode: GameMode) {
        self.session.game_mode = GameMode::Countdown;
        self.session.countdown_ticks_remaining = Some(240 * countdown_seconds);
        // TODO: Store next_mode to transition to when countdown finishes
    }

    /// Transition from Countdown to another mode
    pub fn transition_from_countdown(&mut self, next_mode: GameMode) {
        self.session.game_mode = next_mode;
        self.session.countdown_ticks_remaining = None;

        // Initialize the next mode
        match next_mode {
            GameMode::DemoLap => {
                self.session.demo_lap_progress = Some(0.0);
            }
            _ => {}
        }
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
            Some(grid_position)
        } else {
            None
        }
    }

    /// Remove a player from the session
    pub fn remove_player(&mut self, player_id: &PlayerId) {
        self.session.participants.remove(player_id);
    }

    /// Generate AI input for a player using their AI profile.
    ///
    /// Returns default input if the player is not an AI or has no profile.
    pub fn generate_ai_input(&self, player_id: &PlayerId) -> PlayerInputData {
        // Check if this player has an AI profile
        if let Some(profile) = self.ai_profiles.get(player_id) {
            if let Some(state) = self.session.participants.get(player_id) {
                let controller = AiDriverController::new(profile, &self.track_config);
                return controller.generate_input(state, self.session.current_tick);
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
            .map(|s| CarStateTelemetry::from(s))
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / 240.0) * 1000.0) as u16);

        let telemetry = crate::network::Telemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
        };

        ServerMessage::Telemetry(telemetry)
    }

    #[allow(dead_code)]
    fn is_race_complete(&self) -> bool {
        // Race is complete if all cars have finished required laps
        if self.session.participants.is_empty() {
            return false;
        }

        self.session
            .participants
            .values()
            .all(|s| s.current_lap > self.session.lap_limit as u16)
    }

    #[allow(dead_code)]
    fn assign_finish_positions(&mut self) {
        let mut finishers: Vec<(PlayerId, u16, f32)> = self
            .session
            .participants
            .iter()
            .map(|(id, state)| (*id, state.current_lap, state.track_progress))
            .collect();

        // Sort by laps (descending), then by progress (descending)
        finishers.sort_by(|a, b| {
            b.1.cmp(&a.1).then_with(|| {
                b.2.partial_cmp(&a.2).unwrap_or(std::cmp::Ordering::Equal)
            })
        });

        // Assign positions
        for (position, (player_id, _, _)) in finishers.iter().enumerate() {
            if let Some(state) = self.session.participants.get_mut(player_id) {
                state.finish_position = Some((position + 1) as u8);
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
        let profiles_to_spawn: Vec<(PlayerId, Option<CarConfigId>)> = self.ai_profiles
            .values()
            .filter(|p| !self.session.ai_player_ids.contains(&p.id))
            .take(ai_to_spawn as usize)
            .map(|p| (p.id, p.preferred_car_id))
            .collect();
        
        let default_car_id = self.car_configs.values().next().map(|c| c.id);
        
        for (ai_id, preferred_car) in profiles_to_spawn {
            if self.session.participants.len() >= self.session.max_players as usize {
                break;
            }
            
            // Use preferred car or default
            let car_id = preferred_car
                .or(default_car_id)
                .expect("No car configuration available");
            
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
        
        let mut game_session = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
        
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

        let initial_pos = game_session.session.participants.get(&player_id).unwrap().pos_x;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Position should not change in sandbox mode
        let final_pos = game_session.session.participants.get(&player_id).unwrap().pos_x;
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
            RacelinePoint { x: 0.0, y: 0.0, z: 0.0 },
            RacelinePoint { x: 100.0, y: 0.0, z: 0.0 },
            RacelinePoint { x: 100.0, y: 100.0, z: 0.0 },
            RacelinePoint { x: 0.0, y: 100.0, z: 0.0 },
        ];

        game_session.set_game_mode(GameMode::DemoLap);

        let initial_progress = game_session.session.demo_lap_progress.unwrap();
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Progress should advance
        assert!(game_session.session.demo_lap_progress.unwrap() > initial_progress);
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
            RacelinePoint { x: 0.0, y: 0.0, z: 0.0 },
            RacelinePoint { x: 100.0, y: 0.0, z: 0.0 },
        ];

        game_session.set_game_mode(GameMode::DemoLap);

        // Set progress near the end
        game_session.session.demo_lap_progress = Some(0.99);

        let inputs = HashMap::new();
        
        // Run multiple ticks to ensure loop happens (variable speed means it might take multiple ticks)
        for _ in 0..20 {
            game_session.tick(&inputs);
            if game_session.session.demo_lap_progress.unwrap() < 0.5 {
                break;
            }
        }

        let final_progress = game_session.session.demo_lap_progress.unwrap();
        
        // Should loop back to near 0
        assert!(final_progress < 0.5);
    }

    #[test]
    fn test_free_practice_mode_updates_physics() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::FreePractice);

        // Add a player
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        let initial_pos_x = game_session.session.participants.get(&player_id).unwrap().pos_x;

        // Apply throttle input
        let mut inputs = HashMap::new();
        inputs.insert(player_id, PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
        });

        // Run several ticks to allow physics to update
        for _ in 0..240 {
            game_session.tick(&inputs);
        }

        // Car should have moved after applying throttle for 1 second
        let final_pos_x = game_session.session.participants.get(&player_id).unwrap().pos_x;
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
        assert_eq!(game_session.session.countdown_ticks_remaining, Some(240 * 10));
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
}

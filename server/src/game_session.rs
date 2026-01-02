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

        match self.session.state {
            SessionState::Lobby => {
                // Nothing to do in lobby
            }
            SessionState::Countdown => {
                if let Some(ref mut countdown) = self.session.countdown_ticks_remaining {
                    if *countdown > 0 {
                        *countdown -= 1;
                    } else {
                        // Start racing
                        self.session.state = SessionState::Racing;
                        self.session.race_start_tick = Some(self.session.current_tick);
                        self.session.countdown_ticks_remaining = None;
                    }
                }
            }
            SessionState::Racing => {
                self.tick_racing(inputs);
            }
            SessionState::Finished => {
                // Race is done
            }
        }
    }

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
            countdown_ms,
            car_states,
        };

        ServerMessage::Telemetry(telemetry)
    }

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
        game_session.start_countdown();

        let initial_countdown = game_session.session.countdown_ticks_remaining.unwrap();
        
        let inputs = HashMap::new();
        game_session.tick(&inputs);

        assert_eq!(
            game_session.session.countdown_ticks_remaining.unwrap(),
            initial_countdown - 1
        );
    }

    #[test]
    fn test_tick_to_racing() {
        let mut game_session = create_test_session();
        game_session.session.state = SessionState::Countdown;
        game_session.session.countdown_ticks_remaining = Some(0);

        let inputs = HashMap::new();
        game_session.tick(&inputs);

        assert_eq!(game_session.session.state, SessionState::Racing);
        assert!(game_session.session.race_start_tick.is_some());
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
}

use crate::data::*;
use crate::network::*;
use crate::physics;
use std::collections::HashMap;
use uuid::Uuid;

pub struct GameSession {
    pub session: RaceSession,
    pub track_config: TrackConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
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
        let track_length = self.get_track_length();

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
                // Update physics
                physics::update_car_2d(state, config, &input, dt);

                // Update track progress
                physics::update_track_progress(
                    state,
                    &self.track_config.centerline,
                    track_length,
                    self.session.current_tick,
                );
            }
        }

        // Check collisions
        let mut state_vec: Vec<CarState> = self.session.participants.values().cloned().collect();
        physics::check_aabb_collisions(&mut state_vec, &self.car_configs);

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

    /// Generate AI input for a player
    pub fn generate_ai_input(&self, player_id: &PlayerId) -> PlayerInputData {
        if let Some(state) = self.session.participants.get(player_id) {
            // Simple AI: follow the centerline
            let look_ahead_distance = 20.0; // meters
            let target_speed = 30.0; // m/s

            // Find target point ahead on centerline
            let target_progress = state.track_progress + look_ahead_distance;
            let track_length = self.get_track_length();
            let wrapped_progress = target_progress % track_length;

            let target_point = self
                .track_config
                .centerline
                .iter()
                .min_by_key(|p| {
                    ((p.distance_from_start_m - wrapped_progress).abs() * 1000.0) as i32
                })
                .unwrap();

            // Calculate steering
            let dx = target_point.x - state.pos_x;
            let dy = target_point.y - state.pos_y;
            let target_angle = dy.atan2(dx);
            let angle_diff = target_angle - state.yaw_rad;
            
            // Normalize angle difference to -PI to PI
            let angle_diff = ((angle_diff + std::f32::consts::PI) % (2.0 * std::f32::consts::PI)) - std::f32::consts::PI;

            let steering = (angle_diff * 2.0).clamp(-1.0, 1.0);

            // Calculate throttle/brake
            let (throttle, brake) = if state.speed_mps < target_speed {
                (0.8, 0.0)
            } else {
                (0.0, 0.3)
            };

            PlayerInputData {
                throttle,
                brake,
                steering,
            }
        } else {
            PlayerInputData::default()
        }
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

        ServerMessage::Telemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            countdown_ms,
            car_states,
        }
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

    fn get_track_length(&self) -> f32 {
        self.track_config
            .centerline
            .last()
            .map(|p| p.distance_from_start_m)
            .unwrap_or(1000.0)
    }

    /// Spawn AI drivers to fill empty grid slots
    pub fn spawn_ai_drivers(&mut self) {
        let ai_to_spawn = self.session.ai_count.saturating_sub(
            self.session
                .participants
                .values()
                .filter(|_s| {
                    // Check if player is AI (we need to track this separately)
                    false // For now, we don't track AI flag in CarState
                })
                .count() as u8,
        );

        for _ in 0..ai_to_spawn {
            if self.session.participants.len() >= self.session.max_players as usize {
                break;
            }

            let ai_id = Uuid::new_v4();
            let default_car = self.car_configs.values().next().unwrap().id;
            self.add_player(ai_id, default_car);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn create_test_session() -> GameSession {
        let track = TrackConfig::default();
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, 8, 0, 3);

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
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;

        game_session.add_player(player_id, car_id);
        let ai_input = game_session.generate_ai_input(&player_id);

        // AI should generate valid inputs
        assert!(ai_input.throttle >= 0.0 && ai_input.throttle <= 1.0);
        assert!(ai_input.brake >= 0.0 && ai_input.brake <= 1.0);
        assert!(ai_input.steering >= -1.0 && ai_input.steering <= 1.0);
    }
}

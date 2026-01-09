use crate::data::*;
use crate::network::{LobbyPlayer, SessionSummary};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;
use tracing::{info, warn};

/// Represents a player in the lobby (not in any session)
#[derive(Debug, Clone)]
pub struct LobbyPlayerState {
    pub player_id: PlayerId,
    pub player_name: String,
    pub connection_id: ConnectionId,
    pub selected_car: Option<CarConfigId>,
}

/// Session visibility settings
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionVisibility {
    Public,      // Anyone can see and join
    Private,     // Invite-only, not listed publicly
    Protected,   // Listed but requires password
}

/// Extended session metadata for lobby
#[derive(Debug, Clone)]
pub struct LobbySessionInfo {
    pub session_id: SessionId,
    pub host_player_id: PlayerId,
    pub host_name: String,
    pub track_name: String,
    pub track_file: String,
    pub track_config_id: TrackConfigId,
    pub session_kind: SessionKind,
    pub max_players: u8,
    pub current_player_count: u8,
    pub spectator_count: u8,
    pub state: SessionState,
    pub visibility: SessionVisibility,
    pub password_hash: Option<String>,
    pub created_at: std::time::Instant,
}

/// Manages the lobby state and player matchmaking
pub struct LobbyManager {
    /// Players currently in the lobby (not in any session)
    players: Arc<RwLock<HashMap<PlayerId, LobbyPlayerState>>>,

    /// Active sessions visible to lobby
    sessions: Arc<RwLock<HashMap<SessionId, LobbySessionInfo>>>,

    /// Players currently in sessions (for quick lookup)
    player_sessions: Arc<RwLock<HashMap<PlayerId, SessionId>>>,

    /// Spectators in sessions (player_id -> session_id)
    spectators: Arc<RwLock<HashMap<PlayerId, SessionId>>>,
}

impl LobbyManager {
    pub fn new() -> Self {
        Self {
            players: Arc::new(RwLock::new(HashMap::new())),
            sessions: Arc::new(RwLock::new(HashMap::new())),
            player_sessions: Arc::new(RwLock::new(HashMap::new())),
            spectators: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    /// Add a player to the lobby
    pub async fn add_player(&self, player: LobbyPlayerState) {
        let player_id = player.player_id;
        let player_name = player.player_name.clone();

        self.players.write().await.insert(player_id, player);
        info!("Player {} added to lobby", player_name);
    }

    /// Remove a player from the lobby
    pub async fn remove_player(&self, player_id: PlayerId) -> (Option<LobbyPlayerState>, Option<SessionId>) {
        let player = self.players.write().await.remove(&player_id);
        let mut empty_session_id = None;

        if let Some(ref p) = player {
            info!("Player {} removed from lobby", p.player_name);

            // Also remove from any session
            if let Some(session_id) = self.player_sessions.write().await.remove(&player_id) {
                if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
                    session.current_player_count = session.current_player_count.saturating_sub(1);
                    if session.current_player_count == 0 && session.spectator_count == 0 {
                        empty_session_id = Some(session_id);
                    }
                }
            }
            
            if let Some(session_id) = self.spectators.write().await.remove(&player_id) {
                if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
                    session.spectator_count = session.spectator_count.saturating_sub(1);
                    if session.current_player_count == 0 && session.spectator_count == 0 {
                        empty_session_id = Some(session_id);
                    }
                }
            }
        }

        (player, empty_session_id)
    }

    /// Update a player's selected car
    pub async fn set_player_car(&self, player_id: PlayerId, car_config_id: CarConfigId) {
        if let Some(player) = self.players.write().await.get_mut(&player_id) {
            player.selected_car = Some(car_config_id);
        }
    }

    /// Get a player's selected car
    pub async fn get_player_car(&self, player_id: PlayerId) -> Option<CarConfigId> {
        self.players.read().await.get(&player_id).and_then(|p| p.selected_car)
    }

    /// Register a new session in the lobby
    pub async fn register_session(&self, session_info: LobbySessionInfo) {
        let session_id = session_info.session_id;
        let host_name = session_info.host_name.clone();

        self.sessions.write().await.insert(session_id, session_info);
        info!("Session {} registered in lobby (host: {})", session_id, host_name);
    }

    /// Unregister a session from the lobby
    pub async fn unregister_session(&self, session_id: SessionId) {
        if let Some(_session) = self.sessions.write().await.remove(&session_id) {
            info!("Session {} unregistered from lobby", session_id);

            // Remove all players from this session
            let mut player_sessions = self.player_sessions.write().await;
            player_sessions.retain(|_, sid| *sid != session_id);

            // Remove all spectators from this session
            let mut spectators = self.spectators.write().await;
            spectators.retain(|_, sid| *sid != session_id);
        }
    }

    /// Update session information (player count, state, etc.)
    pub async fn update_session(&self, session_id: SessionId, player_count: u8, state: SessionState) {
        if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
            session.current_player_count = player_count;
            session.state = state;
        }
    }

    /// Add a player to a session (as participant)
    pub async fn join_session(&self, player_id: PlayerId, session_id: SessionId) -> bool {
        // Check if session exists and has space
        let sessions = self.sessions.read().await;
        if let Some(session) = sessions.get(&session_id) {
            if session.current_player_count >= session.max_players {
                warn!("Session {} is full", session_id);
                return false;
            }

            if session.state != SessionState::Lobby {
                warn!("Session {} is not in lobby state", session_id);
                return false;
            }
        } else {
            warn!("Session {} does not exist", session_id);
            return false;
        }
        drop(sessions);

        // Track player's session membership (but keep them in the players list)
        let players = self.players.read().await;
        if players.contains_key(&player_id) {
            drop(players);
            self.player_sessions.write().await.insert(player_id, session_id);

            // Update session player count
            if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
                session.current_player_count += 1;
            }

            info!("Player {} joined session {}", player_id, session_id);
            true
        } else {
            warn!("Player {} not in lobby", player_id);
            false
        }
    }

    /// Add a player as spectator to a session
    pub async fn join_as_spectator(&self, player_id: PlayerId, session_id: SessionId) -> bool {
        // Check if session exists
        let sessions = self.sessions.read().await;
        if !sessions.contains_key(&session_id) {
            warn!("Session {} does not exist", session_id);
            return false;
        }
        drop(sessions);

        // Keep player in lobby, but track them as a spectator
        if self.players.read().await.contains_key(&player_id) {
            self.spectators.write().await.insert(player_id, session_id);

            // Update spectator count
            if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
                session.spectator_count += 1;
            }

            info!("Player {} joined session {} as spectator", player_id, session_id);
            true
        } else {
            warn!("Player {} not in lobby", player_id);
            false
        }
    }

    /// Remove a player from a session (back to lobby)
    pub async fn leave_session(&self, player_id: PlayerId, _connection_id: ConnectionId) -> Option<SessionId> {
        let mut empty_session_id = None;

        // Check if player is in a session
        if let Some(session_id) = self.player_sessions.write().await.remove(&player_id) {
            // Update session player count
            if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
                session.current_player_count = session.current_player_count.saturating_sub(1);
                if session.current_player_count == 0 && session.spectator_count == 0 {
                    empty_session_id = Some(session_id);
                }
            }

            info!("Player {} left session {}", player_id, session_id);
        }

        // Check if player is spectating
        if let Some(session_id) = self.spectators.write().await.remove(&player_id) {
            // Update spectator count
            if let Some(session) = self.sessions.write().await.get_mut(&session_id) {
                session.spectator_count = session.spectator_count.saturating_sub(1);
                if session.current_player_count == 0 && session.spectator_count == 0 {
                    empty_session_id = Some(session_id);
                }
            }

            info!("Spectator {} left session {}", player_id, session_id);
        }

        empty_session_id
        // Player remains in the lobby players list, just no longer in a session
    }

    /// Get all public sessions visible in lobby
    pub async fn get_available_sessions(&self) -> Vec<SessionSummary> {
        let sessions = self.sessions.read().await;

        sessions
            .values()
            .filter(|s| s.visibility == SessionVisibility::Public)
            .map(|s| SessionSummary {
                id: s.session_id,
                track_name: s.track_name.clone(),
                track_file: s.track_file.clone(),
                host_name: s.host_name.clone(),
                session_kind: s.session_kind,
                player_count: s.current_player_count,
                max_players: s.max_players,
                state: s.state,
            })
            .collect()
    }

    /// Get all players currently in the lobby
    pub async fn get_lobby_players(&self) -> Vec<LobbyPlayer> {
        let players = self.players.read().await;
        let player_sessions = self.player_sessions.read().await;

        players
            .values()
            .map(|p| LobbyPlayer {
                id: p.player_id,
                name: p.player_name.clone(),
                selected_car: p.selected_car,
                in_session: player_sessions.get(&p.player_id).copied(),
            })
            .collect()
    }

    /// Check if a player is in a session
    pub async fn get_player_session(&self, player_id: PlayerId) -> Option<SessionId> {
        self.player_sessions.read().await.get(&player_id).copied()
    }

    /// Check if a player is spectating a session
    pub async fn is_spectator(&self, player_id: PlayerId) -> bool {
        self.spectators.read().await.contains_key(&player_id)
    }

    /// Get session a player is spectating
    pub async fn get_spectating_session(&self, player_id: PlayerId) -> Option<SessionId> {
        self.spectators.read().await.get(&player_id).copied()
    }

    /// Get number of players in lobby
    pub async fn get_lobby_count(&self) -> usize {
        self.players.read().await.len()
    }

    /// Get number of active sessions
    pub async fn get_session_count(&self) -> usize {
        self.sessions.read().await.len()
    }
}

impl Default for LobbyManager {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    #[tokio::test]
    async fn test_lobby_add_remove_player() {
        let lobby = LobbyManager::new();
        let player_id = Uuid::new_v4();

        let player = LobbyPlayerState {
            player_id,
            player_name: "TestPlayer".to_string(),
            connection_id: Uuid::new_v4(),
            selected_car: None,
        };

        lobby.add_player(player).await;
        assert_eq!(lobby.get_lobby_count().await, 1);

        let removed = lobby.remove_player(player_id).await;
        assert!(removed.0.is_some());
        assert_eq!(lobby.get_lobby_count().await, 0);
    }

    #[tokio::test]
    async fn test_session_registration() {
        let lobby = LobbyManager::new();
        let session_id = Uuid::new_v4();

        let session_info = LobbySessionInfo {
            session_id,
            host_player_id: Uuid::new_v4(),
            host_name: "Host".to_string(),
            track_name: "Test Track".to_string(),
            track_file: "tracks/TestTrack.yaml".to_string(),
            track_config_id: Uuid::new_v4(),
            session_kind: SessionKind::Multiplayer,
            max_players: 8,
            current_player_count: 0,
            spectator_count: 0,
            state: SessionState::Lobby,
            visibility: SessionVisibility::Public,
            password_hash: None,
            created_at: std::time::Instant::now(),
        };

        lobby.register_session(session_info).await;
        assert_eq!(lobby.get_session_count().await, 1);

        let sessions = lobby.get_available_sessions().await;
        assert_eq!(sessions.len(), 1);

        lobby.unregister_session(session_id).await;
        assert_eq!(lobby.get_session_count().await, 0);
    }

    #[tokio::test]
    async fn test_join_session() {
        let lobby = LobbyManager::new();
        let player_id = Uuid::new_v4();
        let session_id = Uuid::new_v4();

        // Add player to lobby
        let player = LobbyPlayerState {
            player_id,
            player_name: "TestPlayer".to_string(),
            connection_id: Uuid::new_v4(),
            selected_car: None,
        };
        lobby.add_player(player).await;

        // Register session
        let session_info = LobbySessionInfo {
            session_id,
            host_player_id: Uuid::new_v4(),
            host_name: "Host".to_string(),
            track_name: "Test Track".to_string(),
            track_file: "tracks/TestTrack.yaml".to_string(),
            track_config_id: Uuid::new_v4(),
            session_kind: SessionKind::Multiplayer,
            max_players: 8,
            current_player_count: 0,
            spectator_count: 0,
            state: SessionState::Lobby,
            visibility: SessionVisibility::Public,
            password_hash: None,
            created_at: std::time::Instant::now(),
        };
        lobby.register_session(session_info).await;

        // Join session
        let joined = lobby.join_session(player_id, session_id).await;
        assert!(joined);

        // Player should no longer be in lobby
        assert_eq!(lobby.get_lobby_count().await, 0);

        // Player should be in session
        let player_session = lobby.get_player_session(player_id).await;
        assert_eq!(player_session, Some(session_id));
    }

    #[tokio::test]
    async fn test_spectator_mode() {
        let lobby = LobbyManager::new();
        let player_id = Uuid::new_v4();
        let session_id = Uuid::new_v4();

        // Add player to lobby
        let player = LobbyPlayerState {
            player_id,
            player_name: "Spectator".to_string(),
            connection_id: Uuid::new_v4(),
            selected_car: None,
        };
        lobby.add_player(player).await;

        // Register session
        let session_info = LobbySessionInfo {
            session_id,
            host_player_id: Uuid::new_v4(),
            host_name: "Host".to_string(),
            track_name: "Test Track".to_string(),
            track_file: "tracks/TestTrack.yaml".to_string(),
            track_config_id: Uuid::new_v4(),
            session_kind: SessionKind::Multiplayer,
            max_players: 8,
            current_player_count: 2,
            spectator_count: 0,
            state: SessionState::Racing,
            visibility: SessionVisibility::Public,
            password_hash: None,
            created_at: std::time::Instant::now(),
        };
        lobby.register_session(session_info).await;

        // Join as spectator
        let joined = lobby.join_as_spectator(player_id, session_id).await;
        assert!(joined);

        // Should be spectating
        assert!(lobby.is_spectator(player_id).await);
        assert_eq!(lobby.get_spectating_session(player_id).await, Some(session_id));
    }
}

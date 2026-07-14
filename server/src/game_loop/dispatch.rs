//! Inbound event dispatch: one focused handler per client message. Handlers
//! take short state/transport locks and never hold a state write lock and a
//! transport lock at the same time (state is dropped before sends where the
//! two would otherwise overlap; sends only ever take a transport read lock).

use super::{broadcast, lifecycle, GameLoopCtx};
use crate::data::*;
use crate::lobby::{LobbyPlayerState, LobbySessionInfo, SessionVisibility};
use crate::network::{ClientMessage, ServerMessage, SessionJoinedData};
use crate::transport::TransportEvent;
use std::collections::HashMap;
use tracing::{debug, warn};

/// Entry point for one drained transport event.
pub(crate) async fn handle_event(
    ctx: &GameLoopCtx,
    event: TransportEvent,
    player_inputs: &mut HashMap<PlayerId, PlayerInputData>,
) {
    match event {
        TransportEvent::Message(connection_id, msg) => {
            handle_message(ctx, connection_id, msg, player_inputs).await;
        }
        TransportEvent::Disconnected {
            connection_id,
            player_id,
            session_id,
        } => {
            debug!(
                "Connection {} closed (player {}), running disconnect lifecycle",
                connection_id, player_id
            );
            lifecycle::handle_player_disconnect(ctx, player_id, session_id).await;
        }
    }
}

/// The per-message dispatch previously inlined in the game loop.
pub(crate) async fn handle_message(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    msg: ClientMessage,
    player_inputs: &mut HashMap<PlayerId, PlayerInputData>,
) {
    match msg {
        ClientMessage::Authenticate { player_name, .. } => {
            handle_authenticate(ctx, connection_id, player_name).await;
        }
        ClientMessage::SelectCar { car_config_id } => {
            handle_select_car(ctx, connection_id, car_config_id).await;
        }
        ClientMessage::RequestLobbyState => {
            if let Err(e) = broadcast::send_lobby_state(ctx, connection_id).await {
                warn!("Failed to send lobby state: {:?}", e);
            }
        }
        ClientMessage::CreateSession {
            track_config_id,
            max_players,
            ai_count,
            lap_limit,
            session_kind,
        } => {
            handle_create_session(
                ctx,
                connection_id,
                track_config_id,
                session_kind,
                max_players,
                ai_count,
                lap_limit,
            )
            .await;
        }
        ClientMessage::JoinSession { session_id } => {
            handle_join_session(ctx, connection_id, session_id).await;
        }
        ClientMessage::JoinAsSpectator { session_id } => {
            handle_join_as_spectator(ctx, connection_id, session_id).await;
        }
        ClientMessage::LeaveSession => {
            handle_leave_session(ctx, connection_id).await;
        }
        ClientMessage::StartSession => {
            handle_start_session(ctx, connection_id).await;
        }
        ClientMessage::SetGameMode { mode } => {
            handle_set_game_mode(ctx, connection_id, mode).await;
        }
        ClientMessage::StartCountdown {
            countdown_seconds,
            next_mode,
        } => {
            handle_start_countdown(ctx, connection_id, countdown_seconds, next_mode).await;
        }
        ClientMessage::Disconnect => {
            handle_disconnect(ctx, connection_id).await;
        }
        ClientMessage::PlayerInput {
            throttle,
            brake,
            steering,
            ..
        } => {
            handle_player_input(ctx, connection_id, throttle, brake, steering, player_inputs).await;
        }
        _ => {
            // Other messages (Heartbeat, etc.) are handled in the transport layer
        }
    }
}

async fn handle_authenticate(ctx: &GameLoopCtx, connection_id: ConnectionId, player_name: String) {
    // Add player to lobby after authentication
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    {
        let state_read = ctx.state.read().await;
        let lobby_player = LobbyPlayerState {
            player_id: conn_info.player_id,
            player_name,
            connection_id,
            selected_car: None,
        };
        state_read.lobby.add_player(lobby_player).await;
    }

    // Send initial lobby state
    if let Err(e) = broadcast::send_lobby_state(ctx, connection_id).await {
        warn!("Failed to send lobby state: {:?}", e);
    }
}

async fn handle_select_car(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    car_config_id: CarConfigId,
) {
    if let Some(conn_info) = ctx.connection(connection_id).await {
        debug!(
            "SelectCar: player_id={}, car_config_id={}",
            conn_info.player_id, car_config_id
        );
        let state_read = ctx.state.read().await;
        state_read
            .lobby
            .set_player_car(conn_info.player_id, car_config_id)
            .await;
        debug!("SelectCar: Car set successfully");
    } else {
        warn!(
            "SelectCar: No connection info found for connection_id={}",
            connection_id
        );
    }
}

#[allow(clippy::too_many_arguments)]
async fn handle_create_session(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    track_config_id: TrackConfigId,
    session_kind: SessionKind,
    max_players: u8,
    ai_count: u8,
    lap_limit: u8,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;

    // Get host's selected car
    let Some(car_id) = state_write.lobby.get_player_car(conn_info.player_id).await else {
        drop(state_write);
        ctx.send_error(
            connection_id,
            400,
            "Must select a car before creating session",
        )
        .await;
        return;
    };

    // Create session
    let Some(session_id) = state_write.create_session(
        conn_info.player_id,
        car_id,
        track_config_id,
        session_kind,
        max_players,
        ai_count,
        lap_limit,
    ) else {
        warn!(
            "Failed to create session for player {}: track_id={}",
            conn_info.player_id, track_config_id
        );

        // Check why it failed
        let (code, message) =
            if state_write.sessions.len() >= state_write.config.server.max_sessions as usize {
                (503, "Server is at max session capacity")
            } else if !state_write.track_configs.contains_key(&track_config_id) {
                (404, "Track configuration not found")
            } else {
                (500, "Failed to create session")
            };
        drop(state_write);
        ctx.send_error(connection_id, code, message).await;
        return;
    };

    debug!(
        "Session {} created by player {}",
        session_id, conn_info.player_name
    );

    // Register session in lobby
    let track_name = state_write
        .track_configs
        .get(&track_config_id)
        .map(|t| t.name.clone())
        .unwrap_or_else(|| "Unknown Track".to_string());

    let track_file = state_write
        .track_configs
        .get(&track_config_id)
        .and_then(|t| t.source_path.clone())
        .unwrap_or_else(|| "tracks/unknown.yaml".to_string());

    let session_info = LobbySessionInfo {
        session_id,
        host_player_id: conn_info.player_id,
        host_name: conn_info.player_name.clone(),
        track_name,
        track_file,
        session_kind,
        track_config_id,
        max_players,
        current_player_count: 0, // join_session will increment this
        spectator_count: 0,
        state: SessionState::Lobby,
        visibility: SessionVisibility::Public,
        password_hash: None,
        created_at: std::time::Instant::now(),
    };

    state_write.lobby.register_session(session_info).await;

    // Join host to their own session (lobby and game session)
    let joined = state_write
        .lobby
        .join_session(conn_info.player_id, session_id)
        .await;

    if !joined {
        warn!(
            "Player {} failed to join lobby for session {}",
            conn_info.player_id, session_id
        );
        drop(state_write);
        ctx.send_error(connection_id, 500, "Failed to join session lobby")
            .await;
        return;
    }

    // Add host to the actual game session
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        warn!("Session {} not found in sessions map", session_id);
        drop(state_write);
        ctx.send_error(connection_id, 500, "Internal error: session not found")
            .await;
        return;
    };

    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
        drop(state_write);
        let _ = ctx
            .send(
                connection_id,
                ServerMessage::SessionJoined(SessionJoinedData {
                    session_id,
                    your_grid_position: grid_pos,
                }),
            )
            .await;
        // Track that player is in a session
        ctx.set_player_session(connection_id, Some(session_id))
            .await;
    } else {
        warn!(
            "Failed to add player {} to game session {}",
            conn_info.player_id, session_id
        );
        drop(state_write);
        ctx.send_error(connection_id, 500, "Failed to add player to game session")
            .await;
    }
}

async fn handle_join_session(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    session_id: SessionId,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;

    // Get player's selected car
    let selected_car = state_write.lobby.get_player_car(conn_info.player_id).await;

    let joined = state_write
        .lobby
        .join_session(conn_info.player_id, session_id)
        .await;

    if !joined {
        drop(state_write);
        ctx.send_error(
            connection_id,
            400,
            "Unable to join session (full or not in lobby state)",
        )
        .await;
        return;
    }

    // Add player to the actual game session
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        return;
    };

    let Some(car_id) = selected_car else {
        // No car selected
        rollback_failed_join(ctx, &mut state_write, conn_info.player_id, connection_id).await;
        drop(state_write);
        ctx.send_error(
            connection_id,
            400,
            "Must select a car before joining session",
        )
        .await;
        return;
    };

    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
        debug!(
            "Player {} joined session {} at grid position {}",
            conn_info.player_name, session_id, grid_pos
        );
        drop(state_write);
        let _ = ctx
            .send(
                connection_id,
                ServerMessage::SessionJoined(SessionJoinedData {
                    session_id,
                    your_grid_position: grid_pos,
                }),
            )
            .await;
        // Track that player is in a session
        ctx.set_player_session(connection_id, Some(session_id))
            .await;
    } else {
        // Failed to add to session (full)
        rollback_failed_join(ctx, &mut state_write, conn_info.player_id, connection_id).await;
        drop(state_write);
        ctx.send_error(connection_id, 400, "Session is full").await;
    }
}

/// Undo a lobby join that could not be completed in the game session; removes
/// the session entirely if it ended up empty.
async fn rollback_failed_join(
    _ctx: &GameLoopCtx,
    state_write: &mut crate::server::ServerState,
    player_id: PlayerId,
    connection_id: ConnectionId,
) {
    let empty_session = state_write
        .lobby
        .leave_session(player_id, connection_id)
        .await;
    if let Some(session_id) = empty_session {
        debug!(
            "Session {} is empty after failed join, removing it",
            session_id
        );
        state_write.sessions.remove(&session_id);
        state_write.lobby.unregister_session(session_id).await;
    }
}

async fn handle_join_as_spectator(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    session_id: SessionId,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let joined = {
        let state_read = ctx.state.read().await;
        state_read
            .lobby
            .join_as_spectator(conn_info.player_id, session_id)
            .await
    };

    if joined {
        debug!(
            "Player {} joined session {} as spectator",
            conn_info.player_name, session_id
        );
        let _ = ctx
            .send(
                connection_id,
                ServerMessage::SessionJoined(SessionJoinedData {
                    session_id,
                    your_grid_position: 0, // 0 indicates spectator
                }),
            )
            .await;
        // Track that player is in a session
        ctx.set_player_session(connection_id, Some(session_id))
            .await;
    } else {
        ctx.send_error(connection_id, 404, "Session not found")
            .await;
    }
}

async fn handle_leave_session(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;

    // Remove from game session
    if let Some(sid) = conn_info.in_session {
        if let Some(game_session) = state_write.sessions.get_mut(&sid) {
            game_session.remove_player(&conn_info.player_id);
        }
    }

    let empty_session = state_write
        .lobby
        .leave_session(conn_info.player_id, connection_id)
        .await;

    if let Some(session_id) = empty_session {
        debug!(
            "Session {} has no human players left, removing it",
            session_id
        );
        state_write.sessions.remove(&session_id);
        state_write.lobby.unregister_session(session_id).await;
    }

    debug!("Player {} left their session", conn_info.player_name);
    drop(state_write);
    let _ = ctx.send(connection_id, ServerMessage::SessionLeft).await;
    // Clear session tracking
    ctx.set_player_session(connection_id, None).await;
}

async fn handle_start_session(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;

    // Find which session the player is in
    let Some(session_id) = state_write
        .lobby
        .get_player_session(conn_info.player_id)
        .await
    else {
        return;
    };
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        return;
    };

    // Only host can start the session
    if game_session.session.host_player_id != conn_info.player_id {
        drop(state_write);
        ctx.send_error(connection_id, 403, "Only the host can start the session")
            .await;
        return;
    }

    game_session.start_countdown();
    debug!(
        "Player {} started session {}",
        conn_info.player_name, session_id
    );

    // Notify all participants
    let participants: Vec<PlayerId> = game_session.session.participants.keys().cloned().collect();
    debug!(
        "Broadcasting SessionStarting to {} participants",
        participants.len()
    );
    drop(state_write);

    let msg = ServerMessage::SessionStarting {
        countdown_seconds: 5,
    };
    for player_id in participants {
        if let Some(conn_id) = ctx.player_connection(player_id).await {
            debug!(
                "Sending SessionStarting to player {} (connection {})",
                player_id, conn_id
            );
            let _ = ctx.send(conn_id, msg.clone()).await;
        } else {
            warn!("Could not find connection for player {}", player_id);
        }
    }
}

async fn handle_set_game_mode(ctx: &GameLoopCtx, connection_id: ConnectionId, mode: GameMode) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    // Check if player is in a session
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let mut state_write = ctx.state.write().await;

    // Collect data we need before mutable borrow
    let (is_host, human_player_ids) = {
        if let Some(game_session) = state_write.sessions.get(&session_id) {
            let is_host = game_session.session.host_player_id == conn_info.player_id;
            let human_ids: Vec<PlayerId> = game_session
                .session
                .participants
                .keys()
                .filter(|id| !game_session.session.ai_player_ids.contains(id))
                .cloned()
                .collect();
            (is_host, human_ids)
        } else {
            (false, Vec::new())
        }
    };

    if !is_host {
        drop(state_write);
        ctx.send_error(
            connection_id,
            403,
            "Only the session host can change game mode",
        )
        .await;
        return;
    }

    // If switching to DemoLap, add human players as spectators first
    if mode == GameMode::DemoLap {
        for player_id in &human_player_ids {
            state_write
                .lobby
                .join_as_spectator(*player_id, session_id)
                .await;
            debug!("Player {} added as spectator for DemoLap mode", player_id);
        }
    }

    // Now do the mutable borrow
    let current_participant_ids: Option<Vec<PlayerId>> =
        if let Some(game_session) = state_write.sessions.get_mut(&session_id) {
            game_session.set_game_mode(mode);
            Some(game_session.session.participants.keys().cloned().collect())
        } else {
            None
        };
    drop(state_write);

    let Some(current_participant_ids) = current_participant_ids else {
        return;
    };

    // Notify all participants
    let mode_changed_msg = ServerMessage::GameModeChanged { mode };
    for participant_id in current_participant_ids {
        if let Some(conn_id) = ctx.player_connection(participant_id).await {
            if let Err(e) = ctx.send(conn_id, mode_changed_msg.clone()).await {
                warn!("Failed to send mode change to player: {:?}", e);
            }
        }
    }

    // Also notify spectators (for DemoLap)
    for player_id in &human_player_ids {
        if let Some(conn_id) = ctx.player_connection(*player_id).await {
            if let Err(e) = ctx.send(conn_id, mode_changed_msg.clone()).await {
                warn!("Failed to send mode change to spectator: {:?}", e);
            }
        }
    }
}

async fn handle_start_countdown(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    countdown_seconds: u16,
    next_mode: GameMode,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    // Check if player is in a session
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let mut state_write = ctx.state.write().await;
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        return;
    };

    // Only host can start countdown
    if game_session.session.host_player_id != conn_info.player_id {
        drop(state_write);
        ctx.send_error(
            connection_id,
            403,
            "Only the session host can start countdown",
        )
        .await;
        return;
    }

    game_session.start_countdown_mode(countdown_seconds, next_mode);
    let participant_ids: Vec<PlayerId> =
        game_session.session.participants.keys().cloned().collect();
    drop(state_write);

    // Notify all participants
    let countdown_msg = ServerMessage::GameModeChanged {
        mode: GameMode::Countdown,
    };
    for participant_id in participant_ids {
        if let Some(conn_id) = ctx.player_connection(participant_id).await {
            if let Err(e) = ctx.send(conn_id, countdown_msg.clone()).await {
                warn!("Failed to send countdown start to player: {:?}", e);
            }
        }
    }
}

/// Protocol-level disconnect (explicit `Disconnect` message from a client).
/// Stream teardown produces a `TransportEvent::Disconnected` instead; both
/// paths converge on the same lifecycle function.
async fn handle_disconnect(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    if let Some(conn_info) = ctx.connection(connection_id).await {
        lifecycle::handle_player_disconnect(ctx, conn_info.player_id, conn_info.in_session).await;
    }
}

async fn handle_player_input(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    throttle: f32,
    brake: f32,
    steering: f32,
    player_inputs: &mut HashMap<PlayerId, PlayerInputData>,
) {
    // Sanitize before the values reach physics: drop NaN/Inf,
    // clamp to the valid input ranges.
    if !throttle.is_finite() || !brake.is_finite() || !steering.is_finite() {
        warn!(
            "Dropping PlayerInput with non-finite values from connection {}",
            connection_id
        );
        return;
    }
    if let Some(conn_info) = ctx.connection(connection_id).await {
        let input = PlayerInputData {
            throttle: throttle.clamp(0.0, 1.0),
            brake: brake.clamp(0.0, 1.0),
            steering: steering.clamp(-1.0, 1.0),
            gear: None,
            clutch: None,
        };
        player_inputs.insert(conn_info.player_id, input);
        ctx.metrics
            .input_messages_received
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
}

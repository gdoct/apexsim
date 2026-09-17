//! Inbound event dispatch: one focused handler per client message. Handlers
//! take short state/transport locks and never hold a state write lock and a
//! transport lock at the same time (state is dropped before sends where the
//! two would otherwise overlap; sends only ever take a transport read lock).

use super::{broadcast, lifecycle, GameLoopCtx};
use crate::car_setup::CarSetup;
use crate::data::*;
use crate::game_session::GameSession;
use crate::lobby::{LobbyPlayerState, LobbySessionInfo, SessionVisibility};
use crate::network::{
    ClientMessage, GhostLapData, LapRecordData, RacingLineData, ServerMessage, SessionJoinedData,
    TrackSectorsData,
};
use crate::racing_line;
use crate::transport::{ConnectionInfo, TransportEvent};
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
            allowed_assists,
            conditions,
        } => {
            handle_create_session(
                ctx,
                connection_id,
                track_config_id,
                session_kind,
                max_players,
                ai_count,
                lap_limit,
                allowed_assists,
                conditions,
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
        ClientMessage::SetDriverAids {
            auto_gearbox,
            steering_assist,
            abs,
            traction_control,
        } => {
            handle_set_driver_aids(
                ctx,
                connection_id,
                DriverAids {
                    auto_gearbox,
                    steering_assist,
                    abs,
                    traction_control,
                },
            )
            .await;
        }
        ClientMessage::SetCarSetup(setup) => {
            handle_set_car_setup(ctx, connection_id, setup).await;
        }
        ClientMessage::StartCountdown {
            countdown_seconds,
            next_mode,
        } => {
            handle_start_countdown(ctx, connection_id, countdown_seconds, next_mode).await;
        }
        ClientMessage::HotlapRelocate { destination } => {
            handle_hotlap_relocate(ctx, connection_id, destination).await;
        }
        ClientMessage::RequestGhost => {
            handle_request_ghost(ctx, connection_id).await;
        }
        ClientMessage::Disconnect => {
            handle_disconnect(ctx, connection_id).await;
        }
        ClientMessage::PlayerInput {
            throttle,
            brake,
            steering,
            gear,
            clutch,
            ..
        } => {
            handle_player_input(
                ctx,
                connection_id,
                throttle,
                brake,
                steering,
                gear,
                clutch,
                player_inputs,
            )
            .await;
        }
        _ => {
            // Other messages (Heartbeat, UdpHandshake, etc.) are handled in
            // the transport layer
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
    allowed_assists: AllowedAssists,
    conditions: SessionConditions,
) {
    let conditions = conditions.clamp();
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;

    // A demo is watched, not driven, so it needs no car of the host's: it
    // falls back to the default AI car (smallest id, for determinism).
    let is_demo = session_kind == SessionKind::Demo;
    let selected_car = state_write.lobby.get_player_car(conn_info.player_id).await;
    let fallback_car = || state_write.car_configs.keys().min().copied();
    let car_id = if is_demo {
        selected_car.or_else(fallback_car)
    } else {
        selected_car
    };
    // Nobody but the AI takes a seat in a demo.
    let max_players = if is_demo {
        ai_count.max(1)
    } else {
        max_players
    };

    // Get host's selected car
    let Some(car_id) = car_id else {
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
        allowed_assists,
        conditions,
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
        conditions,
        track_config_id,
        max_players,
        current_player_count: 0, // join_session will increment this
        spectator_count: 0,
        state: SessionState::Lobby,
        // Unlisted: a demo is one client's menu backdrop.
        visibility: if is_demo {
            SessionVisibility::Private
        } else {
            SessionVisibility::Public
        },
        password_hash: None,
        created_at: std::time::Instant::now(),
    };

    state_write.lobby.register_session(session_info).await;

    if is_demo {
        start_demo_session(ctx, state_write, &conn_info, connection_id, session_id).await;
        return;
    }

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
    // The record store is cloned out first: the session below borrows
    // `state_write` mutably for the rest of the join.
    let records = state_write.records.clone();
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        warn!("Session {} not found in sessions map", session_id);
        drop(state_write);
        ctx.send_error(connection_id, 500, "Internal error: session not found")
            .await;
        return;
    };

    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
        let racing_line = racing_line_message(game_session, session_id, car_id);
        let timing = timing_messages(
            game_session,
            session_id,
            car_id,
            &conn_info.player_name,
            &records,
        );
        let allowed_assists = game_session.session.allowed_assists;
        let conditions = game_session.session.conditions;
        drop(state_write);
        let _ = ctx
            .send(
                connection_id,
                ServerMessage::SessionJoined(SessionJoinedData {
                    session_id,
                    your_grid_position: grid_pos,
                    session_kind,
                    allowed_assists,
                    conditions,
                }),
            )
            .await;
        if let Some(msg) = racing_line {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in timing {
            let _ = ctx.send(connection_id, msg).await;
        }
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

/// Seconds of grid before a demo race goes green: long enough for the
/// client's broadcast camera to open on the grid.
const DEMO_COUNTDOWN_SECONDS: u16 = 8;

/// Second half of creating a `SessionKind::Demo`: the host watches as a
/// spectator (so no car of theirs is on the grid) and the AI field is counted
/// straight into a race, since nobody will ever press start.
async fn start_demo_session(
    ctx: &GameLoopCtx,
    mut state_write: tokio::sync::RwLockWriteGuard<'_, crate::server::ServerState>,
    conn_info: &ConnectionInfo,
    connection_id: ConnectionId,
    session_id: SessionId,
) {
    let conditions = state_write
        .sessions
        .get(&session_id)
        .map(|s| s.session.conditions)
        .unwrap_or_default();
    let joined = state_write
        .lobby
        .join_as_spectator(conn_info.player_id, session_id)
        .await;
    let started = joined
        && match state_write.sessions.get_mut(&session_id) {
            Some(game_session) => {
                game_session.start_countdown_mode(DEMO_COUNTDOWN_SECONDS, GameMode::Race);
                true
            }
            None => false,
        };
    if !started {
        warn!(
            "Demo session {} for player {} could not be started",
            session_id, conn_info.player_id
        );
        state_write
            .lobby
            .leave_session(conn_info.player_id, connection_id)
            .await;
        state_write.sessions.remove(&session_id);
        state_write.lobby.unregister_session(session_id).await;
        drop(state_write);
        ctx.send_error(connection_id, 500, "Failed to start demo session")
            .await;
        return;
    }
    drop(state_write);

    debug!(
        "Demo session {} started for player {}",
        session_id, conn_info.player_name
    );
    let _ = ctx
        .send(
            connection_id,
            ServerMessage::SessionJoined(SessionJoinedData {
                session_id,
                your_grid_position: 0, // 0 indicates spectator
                session_kind: SessionKind::Demo,
                allowed_assists: AllowedAssists::ALL,
                conditions,
            }),
        )
        .await;
    ctx.set_player_session(connection_id, Some(session_id))
        .await;
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
    // The record store is cloned out first: the session below borrows
    // `state_write` mutably for the rest of the join.
    let records = state_write.records.clone();
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

    let game_session_kind = game_session.session.session_kind;
    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
        debug!(
            "Player {} joined session {} at grid position {}",
            conn_info.player_name, session_id, grid_pos
        );
        let racing_line = racing_line_message(game_session, session_id, car_id);
        let timing = timing_messages(
            game_session,
            session_id,
            car_id,
            &conn_info.player_name,
            &records,
        );
        let allowed_assists = game_session.session.allowed_assists;
        let conditions = game_session.session.conditions;
        drop(state_write);
        let _ = ctx
            .send(
                connection_id,
                ServerMessage::SessionJoined(SessionJoinedData {
                    session_id,
                    your_grid_position: grid_pos,
                    session_kind: game_session_kind,
                    allowed_assists,
                    conditions,
                }),
            )
            .await;
        if let Some(msg) = racing_line {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in timing {
            let _ = ctx.send(connection_id, msg).await;
        }
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

/// The racing line for `car_id` on the session's track, for the client's
/// racing-line overlay; `None` when the session does not allow the aid, the
/// car is unknown or the track has no usable line. Built under the state
/// lock, which costs well under a millisecond for a full circuit.
fn racing_line_message(
    game_session: &GameSession,
    session_id: SessionId,
    car_id: CarConfigId,
) -> Option<ServerMessage> {
    if !game_session.session.allowed_assists.racing_line {
        return None;
    }
    let car = game_session.car_configs.get(&car_id)?;
    let profile = racing_line::build(&game_session.track_config, car)?;
    Some(ServerMessage::RacingLine(RacingLineData::from_profile(
        session_id, &profile,
    )))
}

/// What the joining driver needs to read a lap: where the sector lines are,
/// and the record they are driving against (their own best here in this car,
/// and the fastest anyone has gone). Sent once, with the racing line.
fn timing_messages(
    game_session: &GameSession,
    session_id: SessionId,
    car_id: CarConfigId,
    player_name: &str,
    records: &crate::records::RecordStore,
) -> Vec<ServerMessage> {
    let track_id = game_session.track_config.id;
    let mut out = vec![ServerMessage::TrackSectors(TrackSectorsData {
        session_id,
        track_length_m: game_session.track_length_m(),
        boundaries_m: game_session.sector_boundaries_m(),
    })];
    out.push(ServerMessage::LapRecord(lap_record_message(
        records,
        player_name,
        track_id,
        car_id,
        None,
    )));
    out
}

/// The `LapRecord` for one driver: `record` is a lap just set (the message
/// then announces a new record), or `None` to report the stored one.
pub(crate) fn lap_record_message(
    records: &crate::records::RecordStore,
    player_name: &str,
    track_id: crate::data::TrackConfigId,
    car_id: CarConfigId,
    record: Option<crate::records::LapRecord>,
) -> LapRecordData {
    let is_new = record.is_some();
    let best = record.or_else(|| records.best(player_name, track_id, car_id));
    let track_best = records.track_best(track_id, car_id);
    LapRecordData {
        player_name: player_name.to_string(),
        track_id,
        car_config_id: car_id,
        lap_time_ms: best.as_ref().map(|r| r.lap_time_ms).unwrap_or(0),
        splits_ms: best
            .as_ref()
            .map(|r| r.splits_ms.to_vec())
            .unwrap_or_default(),
        is_new,
        track_record_ms: track_best.as_ref().map(|r| r.lap_time_ms).unwrap_or(0),
        track_record_holder: track_best.map(|r| r.player).unwrap_or_default(),
        has_ghost: best.map(|r| r.ghost_file.is_some()).unwrap_or(false),
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
    let (joined, session_kind, allowed_assists, conditions) = {
        let state_read = ctx.state.read().await;
        let (session_kind, allowed_assists, conditions) = state_read
            .sessions
            .get(&session_id)
            .map(|s| {
                (
                    s.session.session_kind,
                    s.session.allowed_assists,
                    s.session.conditions,
                )
            })
            .unwrap_or_default();
        let joined = state_read
            .lobby
            .join_as_spectator(conn_info.player_id, session_id)
            .await;
        (joined, session_kind, allowed_assists, conditions)
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
                    session_kind,
                    allowed_assists,
                    conditions,
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

async fn handle_set_driver_aids(ctx: &GameLoopCtx, connection_id: ConnectionId, aids: DriverAids) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let mut state_write = ctx.state.write().await;
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        return;
    };
    if let Some(applied) = game_session.set_driver_aids(&conn_info.player_id, aids) {
        let on_off = |on: bool| if on { "on" } else { "off" };
        tracing::debug!(
            "Player {} auto gearbox {}, steering assist {}, abs {:?}, traction control {:?}",
            conn_info.player_id,
            on_off(applied.auto_gearbox),
            on_off(applied.steering_assist),
            applied.abs,
            applied.traction_control
        );
    }
}

async fn handle_set_car_setup(ctx: &GameLoopCtx, connection_id: ConnectionId, setup: CarSetup) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let mut state_write = ctx.state.write().await;
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        return;
    };
    if let Some(applied) = game_session.set_car_setup(&conn_info.player_id, setup) {
        tracing::debug!(
            "Player {} car setup {:?}{}",
            conn_info.player_id,
            applied.clicks(),
            if applied == setup { "" } else { " (clamped)" }
        );
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

/// A hotlap driver going into the garage or out onto the track. Any
/// participant may ask; the answer is in their next telemetry frame
/// (`lap_flags` bit 2), an `Error` when the session is not a hotlap.
async fn handle_hotlap_relocate(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    destination: HotlapDestination,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let result = {
        let mut state_write = ctx.state.write().await;
        match state_write.sessions.get_mut(&session_id) {
            Some(game_session) => game_session.hotlap_relocate(&conn_info.player_id, destination),
            None => Err("Session not found"),
        }
    };
    match result {
        Ok(()) => debug!(
            "Player {} relocated to {:?}",
            conn_info.player_name, destination
        ),
        Err(reason) => ctx.send_error(connection_id, 400, reason).await,
    }
}

/// The trace of the driver's record lap on this session's track in their
/// car, read from the record store off the loop, and an empty `GhostLap`
/// when there is none. Only a participant with a car gets one.
async fn handle_request_ghost(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let (track_id, car_id, store) = {
        let state_read = ctx.state.read().await;
        let Some(game_session) = state_read.sessions.get(&session_id) else {
            return;
        };
        let Some(car) = game_session.session.participants.get(&conn_info.player_id) else {
            return;
        };
        (
            game_session.track_config.id,
            car.car_config_id,
            state_read.records.clone(),
        )
    };
    let player_name = conn_info.player_name.clone();
    let data = tokio::task::spawn_blocking(move || {
        store
            .best(&player_name, track_id, car_id)
            .and_then(|record| store.ghost(&record))
            .map(|lap| GhostLapData::from_lap(track_id, car_id, &lap))
            .unwrap_or_else(|| GhostLapData::empty(track_id, car_id))
    })
    .await
    .unwrap_or_else(|_| GhostLapData::empty(track_id, car_id));
    debug!(
        "Ghost lap for {}: {} samples",
        conn_info.player_name,
        data.sample_count()
    );
    let _ = ctx.send(connection_id, ServerMessage::GhostLap(data)).await;
}

/// Protocol-level disconnect (explicit `Disconnect` message from a client).
/// Stream teardown produces a `TransportEvent::Disconnected` instead; both
/// paths converge on the same lifecycle function.
async fn handle_disconnect(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    if let Some(conn_info) = ctx.connection(connection_id).await {
        lifecycle::handle_player_disconnect(ctx, conn_info.player_id, conn_info.in_session).await;
    }
}

#[allow(clippy::too_many_arguments)]
async fn handle_player_input(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    throttle: f32,
    brake: f32,
    steering: f32,
    gear: Option<i8>,
    clutch: Option<f32>,
    player_inputs: &mut HashMap<PlayerId, PlayerInputData>,
) {
    // Sanitize before the values reach physics: drop NaN/Inf,
    // clamp to the valid input ranges.
    if !throttle.is_finite()
        || !brake.is_finite()
        || !steering.is_finite()
        || clutch.is_some_and(|c| !c.is_finite())
    {
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
            // Gear requests outside the plausible range (-1 = reverse up to
            // 10 forward gears) are ignored rather than reaching physics.
            gear: gear.filter(|g| (-1..=10).contains(g)),
            clutch: clutch.map(|c| c.clamp(0.0, 1.0)),
        };
        player_inputs.insert(conn_info.player_id, input);
        ctx.metrics
            .input_messages_received
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
}

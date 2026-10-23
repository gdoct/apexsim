//! Inbound event dispatch: one focused handler per client message. Handlers
//! take short state/transport locks and never hold a state write lock and a
//! transport lock at the same time (state is dropped before sends where the
//! two would otherwise overlap; sends only ever take a transport read lock).

use super::{broadcast, lifecycle, showcase, GameLoopCtx, HeldInput};
use crate::car_setup::CarSetup;
use crate::data::*;
use crate::game_session::GameSession;
use crate::lobby::{LobbyPlayerState, LobbySessionInfo, SessionVisibility};
use crate::network::{
    ClientMessage, GhostLapData, LapRecordData, RacingLineData, RecordBytes, ServerMessage,
    SessionJoinedData, TrackBundleChunkData, TrackSectorsData,
};
use crate::racing_line;
use crate::session_wire::SessionWire;
use crate::transport::{ConnectionInfo, TransportEvent};
use std::collections::HashMap;
use tracing::{debug, info, warn};

/// Entry point for one drained transport event.
pub(crate) async fn handle_event(
    ctx: &GameLoopCtx,
    event: TransportEvent,
    player_inputs: &mut HashMap<PlayerId, HeldInput>,
) {
    match event {
        TransportEvent::Message(connection_id, msg) => {
            handle_message(ctx, connection_id, *msg, player_inputs).await;
        }
        TransportEvent::Disconnected {
            connection_id,
            player_id,
            session_id,
            keep_seat,
        } => {
            debug!(
                "Connection {} closed (player {}), running disconnect lifecycle",
                connection_id, player_id
            );
            // The last input stops applying: an away car is the server's.
            player_inputs.remove(&player_id);
            lifecycle::handle_player_disconnect(
                ctx,
                connection_id,
                player_id,
                session_id,
                keep_seat,
            )
            .await;
        }
    }
}

/// The per-message dispatch previously inlined in the game loop.
pub(crate) async fn handle_message(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    msg: ClientMessage,
    player_inputs: &mut HashMap<PlayerId, HeldInput>,
) {
    // Taking part in a session ends watching a showcase: one or the other
    // has the client's race view.
    if matches!(
        msg,
        ClientMessage::CreateSession { .. }
            | ClientMessage::JoinSession { .. }
            | ClientMessage::JoinAsSpectator { .. }
            | ClientMessage::SpectateSession { .. }
    ) {
        showcase::handle_leave(ctx, connection_id).await;
    }
    match msg {
        ClientMessage::Authenticate { player_name, .. } => {
            handle_authenticate(ctx, connection_id, player_name).await;
        }
        ClientMessage::SelectCar {
            car_config_id,
            livery,
        } => {
            handle_select_car(ctx, connection_id, car_config_id, livery).await;
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
            damage,
            ai_skill,
            race_seconds,
            grid_order,
            qualifying_seconds,
            weekend,
            watch: _,
            qualifying_cutoff_pct,
            mandatory_stops,
            compound_change,
            qualifying_format,
            qualifying_lap_allowance,
            race_rules,
            classes,
            ballast,
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
                damage,
                ai_skill,
                race_seconds,
                grid_order,
                qualifying_seconds,
                weekend,
                qualifying_cutoff_pct,
                crate::data::PitRule::clamped(mandatory_stops, compound_change),
                (qualifying_format, qualifying_lap_allowance),
                race_rules,
                classes,
                ballast,
            )
            .await;
        }
        ClientMessage::RequestLeaderboard {
            track_config_id,
            car_config_id,
        } => {
            handle_request_leaderboard(ctx, connection_id, track_config_id, car_config_id).await;
        }
        ClientMessage::RequestQualifyingResults { track_config_id } => {
            handle_request_qualifying(ctx, connection_id, track_config_id).await;
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
            pit_autopilot,
        } => {
            handle_set_driver_aids(
                ctx,
                connection_id,
                DriverAids {
                    auto_gearbox,
                    steering_assist,
                    abs,
                    traction_control,
                    pit_autopilot,
                },
            )
            .await;
        }
        ClientMessage::SetCarSetup(setup) => {
            handle_set_car_setup(ctx, connection_id, setup).await;
        }
        ClientMessage::SetPitPlan(plan) => {
            handle_set_pit_plan(ctx, connection_id, plan).await;
        }
        ClientMessage::StartCountdown {
            countdown_seconds,
            next_mode,
        } => {
            handle_start_countdown(ctx, connection_id, countdown_seconds, next_mode).await;
        }
        ClientMessage::HotlapRelocate {
            destination,
            cold_tyres,
        } => {
            handle_hotlap_relocate(ctx, connection_id, destination, cold_tyres).await;
        }
        ClientMessage::RecoverCar { destination } => {
            handle_recover_car(ctx, connection_id, destination).await;
        }
        ClientMessage::RequestGhost => {
            handle_request_ghost(ctx, connection_id).await;
        }
        ClientMessage::RequestTrackBundle { crc, chunk } => {
            handle_request_track_bundle(ctx, connection_id, crc, chunk).await;
        }
        ClientMessage::ListShowcases => {
            showcase::handle_list(ctx, connection_id).await;
        }
        ClientMessage::SpectateShowcase { id } => {
            showcase::handle_spectate(ctx, connection_id, id).await;
        }
        ClientMessage::LeaveSpectate => {
            showcase::handle_leave(ctx, connection_id).await;
        }
        ClientMessage::SpectateSession { session_id } => {
            showcase::handle_spectate_session(ctx, connection_id, session_id).await;
        }
        ClientMessage::Disconnect => {
            handle_disconnect(ctx, connection_id).await;
        }
        ClientMessage::GiveUpSeat => {
            handle_give_up_seat(ctx, connection_id).await;
        }
        ClientMessage::AdvanceWeekend => {
            handle_advance_weekend(ctx, connection_id).await;
        }
        ClientMessage::OpenGrid { countdown_seconds } => {
            handle_open_grid(ctx, connection_id, countdown_seconds).await;
        }
        ClientMessage::SetReady { ready } => {
            handle_set_ready(ctx, connection_id, ready).await;
        }
        ClientMessage::SetLivery { livery } => {
            handle_set_livery(ctx, connection_id, livery).await;
        }
        ClientMessage::Chat { text } => {
            handle_chat(ctx, connection_id, &text).await;
        }
        ClientMessage::Rewind { seconds } => {
            handle_rewind(ctx, connection_id, seconds).await;
        }
        ClientMessage::PlayerInput {
            throttle,
            brake,
            steering,
            gear,
            clutch,
            drs,
            headlights,
            flash,
            ers_mode,
            ers_boost,
            handbrake,
            seq,
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
                drs.unwrap_or(false),
                headlights,
                flash.unwrap_or(false),
                (ers_mode, ers_boost.unwrap_or(false)),
                handbrake.unwrap_or(false),
                seq,
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

    // A resumed player with a seat in a session is offered it back, after
    // the lobby state that lists the session.
    let rejoin = {
        let mut state_write = ctx.state.write().await;
        lifecycle::hold_seat_for_rejoin(&mut state_write, conn_info.player_id).await
    };
    if let Some(msg) = rejoin {
        debug!("Offering player {} their seat back", conn_info.player_id);
        let _ = ctx.send(connection_id, msg).await;
    }
}

/// The seat this connection's player holds in a session they lost the
/// connection to, if any: a seat in the lobby's books that this connection
/// is not driving.
async fn held_seat(
    state: &crate::server::ServerState,
    conn_info: &ConnectionInfo,
) -> Option<SessionId> {
    let seat = state.lobby.get_player_session(conn_info.player_id).await?;
    (conn_info.in_session != Some(seat)).then_some(seat)
}

/// The menu's rejoin banner turned down: the held seat is given up.
async fn handle_give_up_seat(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;
    if let Some(seat) = held_seat(&state_write, &conn_info).await {
        lifecycle::give_up_seat(&mut state_write, conn_info.player_id, seat).await;
    }
}

async fn handle_select_car(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    car_config_id: CarConfigId,
    livery: u8,
) {
    if let Some(conn_info) = ctx.connection(connection_id).await {
        debug!(
            "SelectCar: player_id={}, car_config_id={}, livery={}",
            conn_info.player_id, car_config_id, livery
        );
        let state_read = ctx.state.read().await;
        if state_read.disabled_content.contains(&car_config_id) {
            drop(state_read);
            ctx.send_error(
                connection_id,
                403,
                "That car is switched off on this server",
            )
            .await;
            return;
        }
        state_read
            .lobby
            .set_player_car(conn_info.player_id, car_config_id)
            .await;
        state_read
            .lobby
            .set_player_livery(conn_info.player_id, livery)
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
    damage: DamageLevel,
    ai_skill: Option<u8>,
    race_seconds: Option<u32>,
    grid_order: Vec<String>,
    qualifying_seconds: Option<u32>,
    weekend: Option<crate::weekend::WeekendFormat>,
    qualifying_cutoff_pct: Option<u16>,
    pit_rule: crate::data::PitRule,
    (qualifying_format, qualifying_lap_allowance): (crate::data::QualifyingFormat, u8),
    race_rules: crate::data::RaceRules,
    classes: Vec<ClassEntry>,
    ballast: Vec<crate::data::DriverBallast>,
) {
    let conditions = conditions.clamp();
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    // The menu's backdrop and Watch hotlap are simulated by the game itself
    // (showcases, `hotlap_watch::LocalWatch`); the kinds stay decodable.
    if session_kind.is_watch_only() {
        ctx.send_error(
            connection_id,
            400,
            "This server does not run demo or watched-hotlap sessions; the game makes its own",
        )
        .await;
        return;
    }
    let mut state_write = ctx.state.write().await;

    // Driving in a new session gives up a seat held in another.
    if let Some(seat) = held_seat(&state_write, &conn_info).await {
        lifecycle::give_up_seat(&mut state_write, conn_info.player_id, seat).await;
    }

    let car_id = state_write.lobby.get_player_car(conn_info.player_id).await;
    // The dashboard's switched-off content is refused.
    let switched_off = state_write.disabled_content.contains(&track_config_id)
        || car_id.is_some_and(|id| state_write.disabled_content.contains(&id));
    if switched_off {
        drop(state_write);
        ctx.send_error(
            connection_id,
            403,
            "That track or car is switched off on this server",
        )
        .await;
        return;
    }

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
        ai_skill,
        race_seconds,
        classes,
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

    // The host's damage rule, for every car: the AI seated with the session
    // take it now, everyone who joins later as they are seated.
    if let Some(game_session) = state_write.sessions.get_mut(&session_id) {
        game_session.set_damage(damage);
        game_session.set_grid_order(grid_order);
        game_session.set_weekend(weekend);
        game_session.set_qualifying_seconds(qualifying_seconds);
        game_session.set_qualifying_cutoff(qualifying_cutoff_pct);
        game_session.set_pit_rule(pit_rule.mandatory_stops, pit_rule.compound_change);
        game_session.set_qualifying_format(qualifying_format);
        game_session.set_qualifying_lap_allowance(qualifying_lap_allowance);
        game_session.set_race_rules(race_rules);
        game_session.set_ballast(ballast);
        if !game_session.session.ballast.is_empty() {
            info!(
                "Session {} ballast by driver: {}",
                session_id,
                game_session
                    .session
                    .ballast
                    .iter()
                    .map(|b| format!("{} {} kg", b.driver, b.kg))
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
        game_session.set_driver_name(conn_info.player_id, &conn_info.player_name);
        game_session.set_driver_id(conn_info.player_id, conn_info.driver_id);
    }
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

    // Listed as the session simulates it: the air resolved on create, the
    // race's length clamped (and no lap limit when it is timed).
    let (conditions, lap_limit, race_seconds, classes) = state_write
        .sessions
        .get(&session_id)
        .map_or((conditions, lap_limit, race_seconds, Vec::new()), |s| {
            (
                s.session.conditions,
                s.session.lap_limit,
                s.session.race_seconds,
                s.session.classes.iter().map(|c| c.class.clone()).collect(),
            )
        });
    let session_info = LobbySessionInfo {
        session_id,
        host_player_id: conn_info.player_id,
        host_name: conn_info.player_name.clone(),
        track_name,
        track_file,
        session_kind,
        conditions,
        lap_limit,
        race_seconds,
        classes,
        weekend_waiting: false,
        pit_rule,
        race_rules,
        track_config_id,
        damage,
        max_players,
        current_player_count: 0, // join_session will increment this
        spectator_count: 0,
        state: SessionState::Lobby,
        visibility: SessionVisibility::Public,
        password_hash: None,
        created_at: std::time::Instant::now(),
        persistent: false,
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
    // The record store is cloned out first: the session below borrows
    // `state_write` mutably for the rest of the join.
    let livery = state_write
        .lobby
        .get_player_livery(conn_info.player_id)
        .await;
    let records = state_write.records.clone();
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        warn!("Session {} not found in sessions map", session_id);
        drop(state_write);
        ctx.send_error(connection_id, 500, "Internal error: session not found")
            .await;
        return;
    };

    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
        game_session.set_livery(conn_info.player_id, livery);
        let racing_line =
            racing_line_message(game_session, session_id, &conn_info.player_id, car_id);
        let setup_sheet = setup_sheet_message(game_session, session_id, car_id);
        let timing = timing_messages(
            game_session,
            session_id,
            car_id,
            (&conn_info.player_name, conn_info.driver_id),
            &records,
        );
        let road = road_state_full(game_session, session_id);
        let allowed_assists = game_session.session.allowed_assists;
        let conditions = game_session.session.conditions;
        let damage = game_session.session.damage;
        let ai_skill = game_session.session.ai_skill;
        let race_seconds = game_session.session.race_seconds;
        let qualifying_seconds = game_session.session.qualifying_seconds;
        let weekend = game_session.session.weekend.clone();
        let qualifying_cutoff_pct = game_session.session.qualifying_cutoff_pct;
        let pit_rule = game_session.session.pit_rule;
        let qualifying_format = game_session.session.qualifying_format;
        let qualifying_lap_allowance = game_session.session.qualifying_lap_allowance;
        let race_rules = game_session.session.race_rules;
        let classes = game_session.session.classes.clone();
        let ballast = game_session.session.ballast.clone();
        let physics = game_session.session_physics();
        let track_id = game_session.track_config.id;
        let phase_states = game_session.phase_state_messages();
        let mut physics =
            physics.with_bundle(state_write.track_content.bundle(track_id).as_deref());
        // The car as the server balances it, for the client's copy of the step.
        physics.balance = state_write
            .car_configs
            .get(&car_id)
            .and_then(|car| crate::network::BalanceFigures::of(&state_write.config.balance, car));
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
                    damage,
                    ai_skill,
                    race_seconds,
                    qualifying_seconds,
                    weekend,
                    qualifying_cutoff_pct,
                    mandatory_stops: pit_rule.mandatory_stops,
                    compound_change: pit_rule.compound_change,
                    qualifying_format,
                    qualifying_lap_allowance,
                    race_rules,
                    physics: Some(physics),
                    classes,
                    ballast,
                }),
            )
            .await;
        for msg in phase_states {
            let _ = ctx.send(connection_id, msg).await;
        }
        if let Some(msg) = racing_line {
            let _ = ctx.send(connection_id, msg).await;
        }
        if let Some(msg) = setup_sheet {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in timing {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in road {
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

pub(crate) async fn handle_join_session(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    session_id: SessionId,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    // An event takes drivers by a proved driver id: one whose key is bound
    // here and whose connection proved it (`driver_keys`).
    let proved = match conn_info.driver_id {
        Some(id) => ctx
            .transport
            .read()
            .await
            .driver_keys()
            .key_of(id)
            .is_some(),
        None => false,
    };
    let mut state_write = ctx.state.write().await;

    // A seat held here is taken back; one held elsewhere is given up.
    if let Some(seat) = held_seat(&state_write, &conn_info).await {
        if seat == session_id {
            rejoin_session(ctx, state_write, &conn_info, connection_id, session_id).await;
            return;
        }
        lifecycle::give_up_seat(&mut state_write, conn_info.player_id, seat).await;
    }

    // Get player's selected car
    let selected_car = state_write.lobby.get_player_car(conn_info.player_id).await;
    let livery = state_write
        .lobby
        .get_player_livery(conn_info.player_id)
        .await;

    // The session itself decides whether it takes another driver now: a
    // race under way does not (watch it with `JoinAsSpectator`), a running
    // practice or hotlap does.
    if let Some(reason) = state_write
        .sessions
        .get(&session_id)
        .and_then(|s| s.refuses_drivers())
    {
        drop(state_write);
        ctx.send_error(connection_id, 409, reason).await;
        return;
    }

    // An event's entrant drives their entry's car and livery; a driver it
    // does not take is refused (`events::EventStore::admit`).
    let (selected_car, livery) = match state_write.events.admit(
        session_id,
        conn_info.driver_id,
        proved,
        &conn_info.player_name,
        selected_car,
        livery,
    ) {
        crate::events::Admission::NotAnEvent => (selected_car, livery),
        crate::events::Admission::Seat { car_id, livery } => (Some(car_id), livery),
        crate::events::Admission::Refused(reason) => {
            drop(state_write);
            ctx.send_error(connection_id, 403, reason).await;
            return;
        }
    };

    let joined = state_write
        .lobby
        .join_session(conn_info.player_id, session_id)
        .await;

    if !joined {
        drop(state_write);
        ctx.send_error(
            connection_id,
            400,
            "Unable to join session (full, finished or not joinable)",
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
    game_session.set_driver_name(conn_info.player_id, &conn_info.player_name);
    game_session.set_driver_id(conn_info.player_id, conn_info.driver_id);
    if let Some(grid_pos) = game_session.add_player(conn_info.player_id, car_id) {
        game_session.set_livery(conn_info.player_id, livery);
        // A driver joining a hotlap or qualifying session under way starts
        // in the garage, as the drivers there when it began did.
        let _ =
            game_session.hotlap_relocate(&conn_info.player_id, HotlapDestination::Garage, false);
        debug!(
            "Player {} joined session {} at grid position {}",
            conn_info.player_name, session_id, grid_pos
        );
        let racing_line =
            racing_line_message(game_session, session_id, &conn_info.player_id, car_id);
        let road = road_state_full(game_session, session_id);
        let timing = timing_messages(
            game_session,
            session_id,
            car_id,
            (&conn_info.player_name, conn_info.driver_id),
            &records,
        );
        let allowed_assists = game_session.session.allowed_assists;
        let conditions = game_session.session.conditions;
        let damage = game_session.session.damage;
        let ai_skill = game_session.session.ai_skill;
        let race_seconds = game_session.session.race_seconds;
        let qualifying_seconds = game_session.session.qualifying_seconds;
        let weekend = game_session.session.weekend.clone();
        let qualifying_cutoff_pct = game_session.session.qualifying_cutoff_pct;
        let pit_rule = game_session.session.pit_rule;
        let qualifying_format = game_session.session.qualifying_format;
        let qualifying_lap_allowance = game_session.session.qualifying_lap_allowance;
        let race_rules = game_session.session.race_rules;
        let classes = game_session.session.classes.clone();
        let ballast = game_session.session.ballast.clone();
        let physics = game_session.session_physics();
        let track_id = game_session.track_config.id;
        let phase_states = game_session.phase_state_messages();
        let mut physics =
            physics.with_bundle(state_write.track_content.bundle(track_id).as_deref());
        // The car as the server balances it, for the client's copy of the step.
        physics.balance = state_write
            .car_configs
            .get(&car_id)
            .and_then(|car| crate::network::BalanceFigures::of(&state_write.config.balance, car));
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
                    damage,
                    ai_skill,
                    race_seconds,
                    qualifying_seconds,
                    weekend,
                    qualifying_cutoff_pct,
                    mandatory_stops: pit_rule.mandatory_stops,
                    compound_change: pit_rule.compound_change,
                    qualifying_format,
                    qualifying_lap_allowance,
                    race_rules,
                    physics: Some(physics),
                    classes,
                    ballast,
                }),
            )
            .await;
        for msg in phase_states {
            let _ = ctx.send(connection_id, msg).await;
        }
        if let Some(msg) = racing_line {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in timing {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in road {
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

/// A driver back in the seat they held: the car they left, wherever it is
/// now, with everything a join sends (the session's rules, the racing line,
/// the setup sheet, the sectors and their record). The roster goes out
/// again on the next tick.
async fn rejoin_session(
    ctx: &GameLoopCtx,
    mut state_write: tokio::sync::RwLockWriteGuard<'_, crate::server::ServerState>,
    conn_info: &ConnectionInfo,
    connection_id: ConnectionId,
    session_id: SessionId,
) {
    let records = state_write.records.clone();
    let track_content = state_write.track_content.clone();
    let balance_tables = state_write.config.balance.clone();
    let Some(game_session) = state_write.sessions.get_mut(&session_id) else {
        return;
    };
    let Some((grid_pos, car_id)) = game_session.resume_seat(&conn_info.player_id) else {
        drop(state_write);
        ctx.send_error(connection_id, 409, "No seat held in that session")
            .await;
        return;
    };
    game_session.set_driver_name(conn_info.player_id, &conn_info.player_name);
    game_session.set_driver_id(conn_info.player_id, conn_info.driver_id);
    debug!(
        "Player {} rejoined session {} (grid position {})",
        conn_info.player_name, session_id, grid_pos
    );
    let joined = ServerMessage::SessionJoined(SessionJoinedData {
        session_id,
        your_grid_position: grid_pos,
        session_kind: game_session.session.session_kind,
        allowed_assists: game_session.session.allowed_assists,
        conditions: game_session.session.conditions,
        damage: game_session.session.damage,
        ai_skill: game_session.session.ai_skill,
        race_seconds: game_session.session.race_seconds,
        qualifying_seconds: game_session.session.qualifying_seconds,
        weekend: game_session.session.weekend.clone(),
        qualifying_cutoff_pct: game_session.session.qualifying_cutoff_pct,
        mandatory_stops: game_session.session.pit_rule.mandatory_stops,
        compound_change: game_session.session.pit_rule.compound_change,
        qualifying_format: game_session.session.qualifying_format,
        qualifying_lap_allowance: game_session.session.qualifying_lap_allowance,
        race_rules: game_session.session.race_rules,
        classes: game_session.session.classes.clone(),
        physics: Some(crate::network::SessionPhysics {
            balance: game_session
                .car_configs
                .get(&car_id)
                .and_then(|car| crate::network::BalanceFigures::of(&balance_tables, car)),
            ..game_session.session_physics().with_bundle(
                track_content
                    .bundle(game_session.track_config.id)
                    .as_deref(),
            )
        }),
        ballast: game_session.session.ballast.clone(),
    });
    let mut messages = vec![joined];
    messages.extend(game_session.phase_state_messages());
    messages.extend(racing_line_message(
        game_session,
        session_id,
        &conn_info.player_id,
        car_id,
    ));
    messages.extend(setup_sheet_message(game_session, session_id, car_id));
    messages.extend(timing_messages(
        game_session,
        session_id,
        car_id,
        (&conn_info.player_name, conn_info.driver_id),
        &records,
    ));
    messages.extend(road_state_full(game_session, session_id));
    drop(state_write);
    for msg in messages {
        let _ = ctx.send(connection_id, msg).await;
    }
    ctx.set_player_session(connection_id, Some(session_id))
        .await;
}

/// The racing line for `car_id` on the session's track, for the client's
/// racing-line overlay; `None` when the session does not allow the aid, the
/// car is unknown or the track has no usable line. Built under the state
/// lock, which costs well under a millisecond for a full circuit. The line
/// is planned again for the car as it is at each lap's end
/// (`GameSession::refreshed_racing_line`, from the tick).
fn racing_line_message(
    game_session: &mut GameSession,
    session_id: SessionId,
    player_id: &PlayerId,
    car_id: CarConfigId,
) -> Option<ServerMessage> {
    if !game_session.session.allowed_assists.racing_line {
        return None;
    }
    game_session.note_joining_racing_line(player_id);
    let car = game_session.car_configs.get(&car_id)?;
    let profile = racing_line::build(&game_session.track_config, car)?;
    Some(ServerMessage::RacingLine(RacingLineData::from_profile(
        session_id, &profile,
    )))
}

/// One slice of the session's road state: `count` cells from `first_cell`
/// (wrapping the lap), with every piece of debris on the road now. `None`
/// when the session has no road state (an older track, before creation).
pub(crate) fn road_state_message(
    game_session: &GameSession,
    session_id: SessionId,
    first_cell: usize,
    count: usize,
) -> Option<ServerMessage> {
    let road = game_session.track_config.road_state.as_ref()?;
    let n = road.cell_count();
    if n == 0 {
        return None;
    }
    let count = count.min(n);
    let mut rows = Vec::with_capacity(count * crate::road_state::ROW_BYTES);
    road.write_rows(first_cell % n, count, &mut rows);
    let mut geometry = Vec::with_capacity(count * crate::road_state::GEOMETRY_BYTES);
    road.write_geometry(first_cell % n, count, &mut geometry);
    let mut snow_rows = Vec::new();
    if road.has_snow() {
        snow_rows.reserve(count * crate::road_state::SNOW_ROW_BYTES);
        road.write_snow_rows(first_cell % n, count, &mut snow_rows);
    }
    let mut lane_snow = Vec::new();
    if road.has_snow() {
        road.write_lane_snow(&mut lane_snow);
    }
    let mut water_rows = Vec::new();
    if road.has_puddles() {
        water_rows.reserve(count * crate::road_state::BINS);
        road.write_water_rows(first_cell % n, count, &mut water_rows);
    }
    let debris = game_session
        .debris
        .iter()
        .map(|d| [d.x, d.y])
        .collect::<Vec<_>>();
    Some(ServerMessage::RoadState(crate::network::RoadStateData {
        session_id,
        lap_m: road.lap_m,
        cell_m: crate::road_state::CELL_M,
        bins: crate::road_state::BINS as u8,
        half_span_m: crate::road_state::HALF_SPAN_M,
        first_cell: (first_cell % n) as u32,
        rows: crate::network::RecordBytes(rows),
        debris,
        geometry: crate::network::RecordBytes(geometry),
        snow_rows: crate::network::RecordBytes(snow_rows),
        water_rows: crate::network::RecordBytes(water_rows),
        lane_snow: crate::network::RecordBytes(lane_snow),
    }))
}

/// The whole lap's road state, in messages of at most `ROAD_CELLS_PER_JOIN`
/// cells: what a joining client gets at once, so the host's start rubber is
/// on the road from its first frame.
fn road_state_full(game_session: &GameSession, session_id: SessionId) -> Vec<ServerMessage> {
    let Some(road) = game_session.track_config.road_state.as_ref() else {
        return Vec::new();
    };
    let n = road.cell_count();
    let mut out = Vec::new();
    let mut first = 0;
    while first < n {
        let count = crate::road_state::ROAD_CELLS_PER_JOIN.min(n - first);
        if let Some(msg) = road_state_message(game_session, session_id, first, count) {
            out.push(msg);
        }
        first += count;
    }
    out
}

/// The garage's reference card for `car_id`: every setup knob in real units
/// (`setup_sheet.rs`). The fuel is quoted for a hotlap's fill, which is
/// where the garage is; `None` for an unknown car. The lap's fuel is planned
/// once per car and cached, so this costs a racing-line build at most once.
fn setup_sheet_message(
    game_session: &mut GameSession,
    session_id: SessionId,
    car_id: CarConfigId,
) -> Option<ServerMessage> {
    let lap_fuel = game_session.lap_fuel_liters(car_id).unwrap_or(0.0);
    let car = game_session.car_configs.get(&car_id)?;
    Some(ServerMessage::CarSetupSheet(crate::setup_sheet::build(
        session_id,
        car,
        lap_fuel,
        crate::game_session::HOTLAP_FUEL_LAPS,
    )))
}

/// What the joining driver needs to read a lap: where the sector lines are,
/// and the record they are driving against (their own best here in this car,
/// and the fastest anyone has gone). Sent once, with the racing line.
fn timing_messages(
    game_session: &GameSession,
    session_id: SessionId,
    car_id: CarConfigId,
    driver: (&str, Option<crate::data::DriverId>),
    records: &crate::records::RecordStore,
) -> Vec<ServerMessage> {
    let track_id = game_session.track_config.id;
    let mut out = vec![ServerMessage::TrackSectors(TrackSectorsData {
        session_id,
        track_length_m: game_session.track_length_m(),
        boundaries_m: game_session.sector_boundaries_m(),
    })];
    out.push(ServerMessage::LapRecord(lap_record_message(
        records, driver, track_id, car_id, None,
    )));
    out
}

/// The `LapRecord` for one driver: `record` is a lap just set (the message
/// then announces a new record), or `None` to report the stored one.
pub(crate) fn lap_record_message(
    records: &crate::records::RecordStore,
    (player_name, driver_id): (&str, Option<crate::data::DriverId>),
    track_id: crate::data::TrackConfigId,
    car_id: CarConfigId,
    record: Option<crate::records::LapRecord>,
) -> LapRecordData {
    let is_new = record.is_some();
    let best = record.or_else(|| records.best(player_name, driver_id, track_id, car_id));
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

pub(crate) async fn handle_join_as_spectator(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    session_id: SessionId,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let (
        joined,
        session_kind,
        allowed_assists,
        conditions,
        damage,
        ai_skill,
        race_seconds,
        qualifying_seconds,
        weekend,
        (
            qualifying_cutoff_pct,
            pit_rule,
            (qualifying_format, qualifying_lap_allowance, race_rules, classes, ballast),
        ),
        phase_states,
        sectors,
        road,
    ) = {
        let mut state_write = ctx.state.write().await;
        let joined = state_write
            .lobby
            .join_as_spectator(conn_info.player_id, session_id)
            .await;
        let (
            session_kind,
            allowed_assists,
            conditions,
            damage,
            ai_skill,
            race_seconds,
            qualifying_seconds,
            weekend,
            (
                qualifying_cutoff_pct,
                pit_rule,
                (qualifying_format, qualifying_lap_allowance, race_rules, classes, ballast),
            ),
            phase_states,
            sectors,
            road,
        ) = state_write
            .sessions
            .get_mut(&session_id)
            .map(|s| {
                // A spectator arriving mid-session has no roster, and the
                // client drops telemetry it cannot place: send it again.
                if joined {
                    s.mark_roster_dirty();
                }
                (
                    s.session.session_kind,
                    s.session.allowed_assists,
                    s.session.conditions,
                    s.session.damage,
                    s.session.ai_skill,
                    s.session.race_seconds,
                    s.session.qualifying_seconds,
                    s.session.weekend.clone(),
                    (
                        s.session.qualifying_cutoff_pct,
                        s.session.pit_rule,
                        (
                            s.session.qualifying_format,
                            s.session.qualifying_lap_allowance,
                            s.session.race_rules,
                            s.session.classes.clone(),
                            s.session.ballast.clone(),
                        ),
                    ),
                    s.phase_state_messages(),
                    Some(ServerMessage::TrackSectors(TrackSectorsData {
                        session_id,
                        track_length_m: s.track_length_m(),
                        boundaries_m: s.sector_boundaries_m(),
                    })),
                    road_state_full(s, session_id),
                )
            })
            .unwrap_or_default();
        (
            joined,
            session_kind,
            allowed_assists,
            conditions,
            damage,
            ai_skill,
            race_seconds,
            qualifying_seconds,
            weekend,
            (
                qualifying_cutoff_pct,
                pit_rule,
                (
                    qualifying_format,
                    qualifying_lap_allowance,
                    race_rules,
                    classes,
                    ballast,
                ),
            ),
            phase_states,
            sectors,
            road,
        )
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
                    damage,
                    ai_skill,
                    race_seconds,
                    qualifying_seconds,
                    weekend,
                    qualifying_cutoff_pct,
                    mandatory_stops: pit_rule.mandatory_stops,
                    compound_change: pit_rule.compound_change,
                    qualifying_format,
                    qualifying_lap_allowance,
                    race_rules,
                    physics: None,
                    classes,
                    ballast,
                }),
            )
            .await;
        for msg in phase_states {
            let _ = ctx.send(connection_id, msg).await;
        }
        // Where the sector lines are, so the spectator's timing board can
        // place every car; the roster follows with the next tick.
        if let Some(msg) = sectors {
            let _ = ctx.send(connection_id, msg).await;
        }
        for msg in road {
            let _ = ctx.send(connection_id, msg).await;
        }
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

    // Leaving what the connection is in: watching a session (a menu
    // backdrop) or driving in it. A seat held in another session (the
    // player lost the connection to it) is left alone: the menu sends this
    // with nothing joined too.
    let watching = state_write
        .lobby
        .get_spectating_session(conn_info.player_id)
        .await
        .filter(|sid| conn_info.in_session == Some(*sid));
    let empty_session = if watching.is_some() {
        state_write
            .lobby
            .leave_spectating(conn_info.player_id)
            .await
    } else if let Some(sid) = conn_info.in_session {
        // Remove from game session
        if let Some(game_session) = state_write.sessions.get_mut(&sid) {
            game_session.remove_player(&conn_info.player_id);
        }
        state_write
            .lobby
            .leave_session(conn_info.player_id, connection_id)
            .await
    } else {
        None
    };

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

/// A chat line (`crate::chat`): to the session the player drives in or
/// watches, else to everyone in the lobby outside a session; refused with
/// 429 when too fast and 403 when muted.
async fn handle_chat(ctx: &GameLoopCtx, connection_id: ConnectionId, text: &str) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let player = conn_info.player_id;
    let (verdict, session) = {
        let mut st = ctx.state.write().await;
        let verdict = st.chat.check(player, text, std::time::Instant::now());
        let session = match st.lobby.get_player_session(player).await {
            Some(id) => Some(id),
            None => st.lobby.get_spectating_session(player).await,
        };
        (verdict, session)
    };
    let line = match verdict {
        crate::chat::Verdict::Send(line) => line,
        crate::chat::Verdict::Empty => return,
        crate::chat::Verdict::Muted => {
            ctx.send_error(connection_id, 403, "The operator has muted your chat")
                .await;
            return;
        }
        crate::chat::Verdict::TooFast => {
            ctx.send_error(
                connection_id,
                429,
                "Slow down: a few lines every ten seconds",
            )
            .await;
            return;
        }
    };
    let audience: Vec<PlayerId> = match session {
        Some(id) => crate::admin::hosted::session_audience(&*ctx.state.read().await, id).await,
        None => {
            // The lobby: everyone connected who is in no session.
            let conns = ctx.transport.read().await.connection_snapshots().await;
            let st = ctx.state.read().await;
            let mut out = Vec::new();
            for c in conns {
                if st.lobby.get_player_session(c.player_id).await.is_none()
                    && st.lobby.get_spectating_session(c.player_id).await.is_none()
                {
                    out.push(c.player_id);
                }
            }
            out
        }
    };
    tracing::info!(
        "Chat {} {}: {}",
        conn_info.player_name,
        session
            .map(|s| format!("in {s}"))
            .unwrap_or_else(|| "in the lobby".into()),
        line
    );
    let msg = ServerMessage::Chat(crate::network::ChatData {
        player_id: player,
        from: conn_info.player_name.clone(),
        text: line,
        in_session: session.is_some(),
    });
    for to in audience {
        if let Some(conn) = ctx.player_connection(to).await {
            let _ = ctx.send(conn, msg.clone()).await;
        }
    }
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

    if !game_session.start_countdown() {
        drop(state_write);
        ctx.send_error(connection_id, 409, "Session has already started")
            .await;
        return;
    }
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
        countdown_seconds: crate::game_session::START_SESSION_COUNTDOWN_SECONDS as u8,
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
            "Player {} auto gearbox {}, steering assist {}, abs {:?}, traction control {:?}, pit autopilot {:?}",
            conn_info.player_id,
            on_off(applied.auto_gearbox),
            on_off(applied.steering_assist),
            applied.abs,
            applied.traction_control,
            applied.pit_autopilot
        );
    }
}

async fn handle_set_pit_plan(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    plan: crate::pit::PitPlan,
) {
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
    if let Some(kept) = game_session.set_pit_plan(&conn_info.player_id, plan) {
        tracing::debug!("Player {} pit plan {:?}", conn_info.player_id, kept);
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

/// The host puts the race's grid up (`GameSession::open_grid`): the
/// session hears `GameModeChanged { Grid }`; the countdown follows in
/// telemetry once every driver is ready. Anyone else gets `Error 403`.
async fn handle_open_grid(ctx: &GameLoopCtx, connection_id: ConnectionId, countdown_seconds: u16) {
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
    if game_session.session.host_player_id != conn_info.player_id {
        drop(state_write);
        ctx.send_error(
            connection_id,
            403,
            "Only the session host can open the grid",
        )
        .await;
        return;
    }
    game_session.open_grid(countdown_seconds);
    debug!(
        "Player {} opened the grid of session {} ({} s countdown)",
        conn_info.player_name, session_id, countdown_seconds
    );
    let participants: Vec<PlayerId> = game_session.session.participants.keys().copied().collect();
    let spectators = state_write.lobby.get_session_spectators(session_id).await;
    drop(state_write);

    let msg = ServerMessage::GameModeChanged {
        mode: GameMode::Grid,
    };
    for player_id in participants.into_iter().chain(spectators) {
        if let Some(conn_id) = ctx.player_connection(player_id).await {
            let _ = ctx.send(conn_id, msg.clone()).await;
        }
    }
}

/// A driver on the grid is ready, or not (`GameSession::set_ready`); the
/// next roster says so. Ignored outside the grid.
async fn handle_set_ready(ctx: &GameLoopCtx, connection_id: ConnectionId, ready: bool) {
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
    if game_session.set_ready(conn_info.player_id, ready) {
        debug!(
            "Player {} ready on the grid: {}",
            conn_info.player_name, ready
        );
    }
}

/// A driver repaints their car in a session: kept for the session (the
/// roster carries it) and for the next one, as `SelectCar` would.
async fn handle_set_livery(ctx: &GameLoopCtx, connection_id: ConnectionId, livery: u8) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let mut state_write = ctx.state.write().await;
    // An event fixes each entrant's livery for all its sessions.
    if conn_info
        .in_session
        .is_some_and(|id| state_write.events.event_of_session(id).is_some())
    {
        drop(state_write);
        ctx.send_error(connection_id, 403, "The livery is fixed for this event")
            .await;
        return;
    }
    state_write
        .lobby
        .set_player_livery(conn_info.player_id, livery)
        .await;
    if let Some(game_session) = conn_info
        .in_session
        .and_then(|id| state_write.sessions.get_mut(&id))
    {
        game_session.set_livery(conn_info.player_id, livery);
    }
}

/// The host moves a race weekend on (`GameSession::advance_weekend`).
/// Anyone else gets `Error 403`; a session that is not a weekend, a phase
/// still running or a race already begun `Error 409`. The session hears the
/// new mode as `GameModeChanged` (`Countdown` into the race); the new
/// `WeekendState` follows with the next tick.
async fn handle_advance_weekend(ctx: &GameLoopCtx, connection_id: ConnectionId) {
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
    if game_session.session.host_player_id != conn_info.player_id {
        drop(state_write);
        ctx.send_error(
            connection_id,
            403,
            "Only the session host can advance the weekend",
        )
        .await;
        return;
    }
    if let Err(reason) = game_session.advance_weekend() {
        drop(state_write);
        ctx.send_error(connection_id, 409, reason).await;
        return;
    }
    let mode = game_session.session.game_mode;
    debug!(
        "Player {} advanced session {} to {:?}",
        conn_info.player_name, session_id, mode
    );
    let humans: Vec<PlayerId> = game_session
        .session
        .participants
        .keys()
        .filter(|id| !game_session.session.ai_player_ids.contains(id))
        .copied()
        .collect();
    let spectators = state_write.lobby.get_session_spectators(session_id).await;
    drop(state_write);

    let msg = ServerMessage::GameModeChanged { mode };
    for player_id in humans.into_iter().chain(spectators) {
        if let Some(conn_id) = ctx.player_connection(player_id).await {
            let _ = ctx.send(conn_id, msg.clone()).await;
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
    cold_tyres: bool,
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
            Some(game_session) => {
                game_session.hotlap_relocate(&conn_info.player_id, destination, cold_tyres)
            }
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

/// A stuck driver's car put back (`GameSession::recover_car`); an `Error`
/// says why when it cannot be.
/// A driver alone in a practice session rewinds it (`crate::rewind`): the
/// recording so far is written as it stands, the session restored, its
/// stream started again for whoever watches it, and everyone in it told
/// (`Rewound`) so they drop what they had of the run undone.
async fn handle_rewind(ctx: &GameLoopCtx, connection_id: ConnectionId, seconds: u16) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    let Some(session_id) = conn_info.in_session else {
        return;
    };
    let names: HashMap<PlayerId, String> = {
        let state = ctx.state.read().await;
        state
            .lobby
            .get_lobby_players()
            .await
            .into_iter()
            .map(|p| (p.id, p.name))
            .collect()
    };
    let outcome = {
        let mut state_write = ctx.state.write().await;
        let state = &mut *state_write;
        match state.sessions.get_mut(&session_id) {
            None => Err((404, "Session not found")),
            Some(game_session)
                if crate::rewind::sole_driver(game_session) != Some(conn_info.player_id) =>
            {
                Err((403, "Rewind is for a practice session you drive alone"))
            }
            Some(game_session) => match state.rewinds.get_mut(&session_id) {
                Some(rewind) if rewind.can_rewind() => {
                    // The run up to the rewind is a replay of its own.
                    let cut = super::tick::finish_recording(
                        &state.replay,
                        session_id,
                        game_session.session.race_start_tick,
                        game_session.session.game_mode,
                        super::tick::replay_participants(game_session, &names),
                    );
                    match rewind.rewind(game_session, seconds) {
                        Some(done) => {
                            if cut {
                                rewind.restart_recording();
                            }
                            state.streams.remove(&session_id);
                            for viewer in state.live.forget_session(&session_id) {
                                state.live.join(viewer, session_id);
                            }
                            let mut recipients: Vec<PlayerId> = game_session
                                .session
                                .participants
                                .keys()
                                .filter(|id| !game_session.session.ai_player_ids.contains(id))
                                .copied()
                                .collect();
                            recipients.extend(state.lobby.get_session_spectators(session_id).await);
                            Ok((done, recipients))
                        }
                        None => Err((409, "Nothing to rewind to yet")),
                    }
                }
                _ => Err((409, "Nothing to rewind to yet")),
            },
        }
    };
    match outcome {
        Ok((done, recipients)) => {
            debug!(
                "Player {} rewound session {} from tick {} to {}",
                conn_info.player_name, session_id, done.from_tick, done.tick
            );
            let msg = ServerMessage::Rewound(crate::network::RewoundData {
                tick: done.tick,
                from_tick: done.from_tick,
                hold_ms: crate::rewind::HOLD_MS,
            });
            for player_id in recipients {
                if let Some(conn_id) = ctx.player_connection(player_id).await {
                    let _ = ctx.send(conn_id, msg.clone()).await;
                }
            }
        }
        Err((code, reason)) => ctx.send_error(connection_id, code, reason).await,
    }
}

async fn handle_recover_car(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    destination: crate::recovery::RecoverDestination,
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
            Some(game_session) => game_session.recover_car(&conn_info.player_id, destination),
            None => Err("Session not found"),
        }
    };
    match result {
        Ok(()) => debug!(
            "Player {} recovered to {:?}",
            conn_info.player_name, destination
        ),
        Err(reason) => ctx.send_error(connection_id, 400, reason).await,
    }
}

/// The trace of the driver's record lap on this session's track in their
/// car, read from the record store off the loop, and an empty `GhostLap`
/// when there is none. Only a participant with a car gets one.
/// One chunk of a loaded track's physics bundle (`crate::track_bundle`),
/// named by its checksum: what a predicting client steps its car on. The
/// bundle is built when the track loads, so this is a slice of it; a
/// checksum or chunk the server does not have comes back as no chunks.
async fn handle_request_track_bundle(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    crc: u32,
    chunk: u32,
) {
    let bundle = ctx.state.read().await.track_content.bundle_by_crc(crc);
    let data = match bundle.as_deref().and_then(|b| Some((b, b.chunk(chunk)?))) {
        Some((bundle, bytes)) => TrackBundleChunkData {
            crc,
            chunk,
            chunks: bundle.chunk_count(),
            bytes: RecordBytes(bytes.to_vec()),
        },
        None => {
            debug!(
                "Connection {} asked for chunk {} of track bundle {:08X}, which is not here",
                connection_id, chunk, crc
            );
            TrackBundleChunkData {
                crc,
                chunk,
                chunks: 0,
                bytes: RecordBytes(Vec::new()),
            }
        }
    };
    let _ = ctx
        .send(connection_id, ServerMessage::TrackBundleChunk(data))
        .await;
}

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
    let (player_name, driver_id) = (conn_info.player_name.clone(), conn_info.driver_id);
    let data = tokio::task::spawn_blocking(move || {
        store
            .best(&player_name, driver_id, track_id, car_id)
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

/// The qualifying results stored for a track, newest first, read off the
/// loop.
/// How many laps a leaderboard lists.
const LEADERBOARD_LIMIT: usize = 100;

async fn handle_request_leaderboard(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    track_id: TrackConfigId,
    car: Option<String>,
) {
    if ctx.connection(connection_id).await.is_none() {
        return;
    }
    // An empty or unreadable car id is every car.
    let car_id = car
        .as_deref()
        .filter(|c| !c.is_empty())
        .and_then(|c| CarConfigId::parse_str(c).ok());
    let store = ctx.state.read().await.records.clone();
    let records =
        tokio::task::spawn_blocking(move || store.leaderboard(track_id, car_id, LEADERBOARD_LIMIT))
            .await
            .unwrap_or_default();
    let data = crate::network::LeaderboardData::from_records(track_id, car_id, records);
    let _ = ctx
        .send(connection_id, ServerMessage::Leaderboard(data))
        .await;
}

async fn handle_request_qualifying(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    track_id: TrackConfigId,
) {
    if ctx.connection(connection_id).await.is_none() {
        return;
    }
    let store = ctx.state.read().await.records.clone();
    let results = tokio::task::spawn_blocking(move || store.qualifying_for(track_id))
        .await
        .unwrap_or_default();
    let data = crate::network::QualifyingResultsData::from_results(track_id, results);
    let _ = ctx
        .send(connection_id, ServerMessage::QualifyingResults(data))
        .await;
}

/// Protocol-level disconnect (explicit `Disconnect` message from a client).
/// Stream teardown produces a `TransportEvent::Disconnected` instead; both
/// paths converge on the same lifecycle function.
async fn handle_disconnect(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    if let Some(conn_info) = ctx.connection(connection_id).await {
        // Leaving the server on purpose gives the seat up.
        lifecycle::handle_player_disconnect(
            ctx,
            connection_id,
            conn_info.player_id,
            conn_info.in_session,
            false,
        )
        .await;
        ctx.set_player_session(connection_id, None).await;
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
    drs: bool,
    headlights: Option<bool>,
    flash: bool,
    (ers_mode, ers_boost): (Option<u8>, bool),
    handbrake: bool,
    seq: Option<u32>,
    player_inputs: &mut HashMap<PlayerId, HeldInput>,
) {
    // Sanitize before the values reach physics: drop NaN/Inf, clamp to the
    // valid input ranges, ignore an implausible gear (the client's
    // prediction takes its own input the same way).
    let Some(input) = (PlayerInputData {
        throttle,
        brake,
        steering,
        gear,
        clutch,
        drs,
        headlights,
        flash,
        ers_mode,
        ers_boost,
        handbrake,
    })
    .sanitized() else {
        warn!(
            "Dropping PlayerInput with non-finite values from connection {}",
            connection_id
        );
        return;
    };
    if let Some(conn_info) = ctx.connection(connection_id).await {
        player_inputs.insert(conn_info.player_id, HeldInput { input, seq });
        ctx.metrics
            .input_messages_received
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
}

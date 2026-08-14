//! Outbound fan-out: per-session telemetry (pre-serialized once in the tick
//! phase, sent as cheap `Bytes` clones) and lobby-state messages. Lock rule:
//! recipients are resolved under a state READ lock which is released before
//! the transport READ lock is taken for sending.

use super::tick::{SessionRosterOut, SessionTelemetry};
use super::GameLoopCtx;
use crate::data::{ConnectionId, PlayerId, SessionId};
use crate::network::{
    CarConfigSummary, LobbyPlayer, LobbyStateData, MessagePriority, ServerMessage, SessionSummary,
    TrackConfigSummary,
};
use crate::transport::{TransportError, TransportLayer};
use bytes::Bytes;
use std::sync::atomic::Ordering;
use tracing::{debug, warn};

/// Resolve the live recipients of a session broadcast: players still in the
/// session (verified via the lobby) plus the session's spectators. Takes a
/// state READ lock which is released before any transport lock is taken.
async fn resolve_recipients(
    ctx: &GameLoopCtx,
    session_id: SessionId,
    player_recipients: &[PlayerId],
) -> (Vec<PlayerId>, Vec<PlayerId>) {
    let state_read = ctx.state.read().await;

    let mut players: Vec<PlayerId> = Vec::with_capacity(player_recipients.len());
    for player_id in player_recipients {
        if state_read.lobby.get_player_session(*player_id).await == Some(session_id) {
            players.push(*player_id);
        }
    }

    // Also collect spectators for this session (important for DemoLap mode)
    let spectators = state_read.lobby.get_session_spectators(session_id).await;

    (players, spectators)
}

/// Send one telemetry payload to a player: over UDP when the connection has
/// completed the UDP handshake, over TCP (droppable) otherwise.
async fn send_telemetry_to(transport: &TransportLayer, player_id: PlayerId, data: &Bytes) -> bool {
    let Some((conn_id, udp_addr)) = transport.get_player_route(player_id).await else {
        return false;
    };
    match udp_addr {
        Some(addr) => transport.send_udp_serialized(addr, data.clone()).is_ok(),
        None => transport
            .send_serialized(conn_id, data.clone(), MessagePriority::Droppable)
            .await
            .is_ok(),
    }
}

/// Fan out the tick phase's telemetry payloads to connected participants and
/// spectators. Each payload was serialized exactly once; every recipient gets
/// a `Bytes` clone, over UDP when handshaken (TCP fallback otherwise).
pub(crate) async fn broadcast_telemetry(
    ctx: &GameLoopCtx,
    sessions: Vec<SessionTelemetry>,
    tick_count: u64,
) {
    for entry in sessions {
        let (players, spectators) =
            resolve_recipients(ctx, entry.session_id, &entry.player_recipients).await;

        // Send under a transport READ lock (send paths only need &self).
        let transport_read = ctx.transport.read().await;

        let mut sent_players = 0usize;
        let mut sent_spectators = 0usize;
        for player_id in players {
            if send_telemetry_to(&transport_read, player_id, &entry.data).await {
                ctx.metrics
                    .telemetry_messages_sent
                    .fetch_add(1, Ordering::Relaxed);
                sent_players += 1;
            }
        }
        for player_id in spectators {
            if send_telemetry_to(&transport_read, player_id, &entry.data).await {
                ctx.metrics
                    .telemetry_messages_sent
                    .fetch_add(1, Ordering::Relaxed);
                sent_spectators += 1;
            }
        }

        if (sent_players > 0 || sent_spectators > 0) && tick_count.is_multiple_of(60) {
            debug!(
                "Broadcast telemetry for session {} to {} players + {} spectators (participants: {}, state: {:?})",
                entry.session_id,
                sent_players,
                sent_spectators,
                entry.participant_count,
                entry.session_state
            );
        }
    }
}

/// Deliver `SessionRoster` messages reliably over TCP to participants and
/// spectators of the affected sessions.
pub(crate) async fn broadcast_rosters(ctx: &GameLoopCtx, rosters: Vec<SessionRosterOut>) {
    for roster in rosters {
        let (players, spectators) =
            resolve_recipients(ctx, roster.session_id, &roster.player_recipients).await;

        let transport_read = ctx.transport.read().await;
        for player_id in players.into_iter().chain(spectators) {
            if let Some(conn_id) = transport_read.get_player_connection(player_id).await {
                if let Err(e) = transport_read.send_tcp(conn_id, roster.msg.clone()).await {
                    warn!(
                        "Failed to send session roster to player {}: {:?}",
                        player_id, e
                    );
                }
            }
        }
    }
}

/// Snapshot the parts of the lobby state that actually change (state read lock).
async fn lobby_participants(
    ctx: &GameLoopCtx,
    log_players: bool,
) -> (Vec<LobbyPlayer>, Vec<SessionSummary>) {
    let state_read = ctx.state.read().await;

    let players_in_lobby = state_read.lobby.get_lobby_players().await;
    if log_players {
        debug!(
            "send_lobby_state: players_in_lobby count = {}",
            players_in_lobby.len()
        );
        for player in &players_in_lobby {
            debug!(
                "  - Player: {} (ID: {}), SelectedCar: {:?}, InSession: {:?}",
                player.name, player.id, player.selected_car, player.in_session
            );
        }
    }
    let available_sessions = state_read.lobby.get_available_sessions().await;

    (players_in_lobby, available_sessions)
}

/// Build the current `LobbyState` message from server state (state read lock).
async fn build_lobby_state(ctx: &GameLoopCtx, log_players: bool) -> ServerMessage {
    let (players_in_lobby, available_sessions) = lobby_participants(ctx, log_players).await;
    build_lobby_state_with(ctx, players_in_lobby, available_sessions).await
}

/// Build a `LobbyState` message around an already-taken participant snapshot.
async fn build_lobby_state_with(
    ctx: &GameLoopCtx,
    players_in_lobby: Vec<LobbyPlayer>,
    available_sessions: Vec<SessionSummary>,
) -> ServerMessage {
    let state_read = ctx.state.read().await;

    // Get car and track configs
    let car_configs: Vec<CarConfigSummary> = state_read
        .car_configs
        .values()
        .map(|c| CarConfigSummary {
            id: c.id,
            name: c.name.clone(),
            model_path: format!("res://content/cars/{}/{}", c.id, c.model),
            mass_kg: c.mass_kg,
            max_engine_force_n: c.max_engine_power_w / 100.0, // Rough approximation
        })
        .collect();

    let track_configs: Vec<TrackConfigSummary> = state_read
        .track_configs
        .values()
        .map(|t| TrackConfigSummary {
            id: t.id,
            name: t.name.clone(),
            centerline: t
                .centerline
                .iter()
                .step_by(10)
                .map(|p| crate::network::TrackPoint { x: p.x, y: p.y })
                .collect(), // Send every 10th point
        })
        .collect();

    drop(state_read);

    ServerMessage::LobbyState(LobbyStateData {
        players_in_lobby,
        available_sessions,
        car_configs,
        track_configs,
    })
}

/// Send the current lobby state to a single connection.
pub(crate) async fn send_lobby_state(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
) -> Result<(), TransportError> {
    let lobby_state = build_lobby_state(ctx, true).await;
    ctx.transport
        .read()
        .await
        .send_tcp(connection_id, lobby_state)
        .await
}

/// Last broadcast `LobbyState`, kept by the game loop across ticks.
///
/// The message is dominated by the static car/track catalog (the decimated
/// centerlines of every track — hundreds of KB), while only the player and
/// session lists change. Re-encoding all of it every two seconds costs
/// milliseconds and overran the tick budget on an idle server, so the encoded
/// payload is reused until the participant snapshot actually differs.
#[derive(Default)]
pub(crate) struct LobbyStateCache {
    players: Vec<LobbyPlayer>,
    sessions: Vec<SessionSummary>,
    encoded: Option<Bytes>,
}

/// Broadcast lobby state to all connected clients: serialized once, fanned
/// out as `Bytes` clones (LobbyState is droppable, matching its priority).
pub(crate) async fn broadcast_lobby_state(
    ctx: &GameLoopCtx,
    cache: &mut LobbyStateCache,
) -> Result<(), rmp_serde::encode::Error> {
    // Nobody to send to: skip the whole build/encode.
    if ctx
        .transport
        .read()
        .await
        .get_connection_count_async()
        .await
        == 0
    {
        return Ok(());
    }

    let (players, sessions) = lobby_participants(ctx, false).await;

    let data = match &cache.encoded {
        Some(encoded) if cache.players == players && cache.sessions == sessions => encoded.clone(),
        _ => {
            let lobby_state = build_lobby_state_with(ctx, players.clone(), sessions.clone()).await;
            debug_assert_eq!(lobby_state.priority(), MessagePriority::Droppable);
            let data = Bytes::from(rmp_serde::to_vec_named(&lobby_state)?);
            cache.players = players;
            cache.sessions = sessions;
            cache.encoded = Some(data.clone());
            data
        }
    };

    // Broadcast to all connections
    ctx.transport
        .read()
        .await
        .broadcast_serialized(data, MessagePriority::Droppable)
        .await;
    Ok(())
}

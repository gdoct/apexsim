//! Outbound fan-out: per-session telemetry (pre-serialized once in the tick
//! phase, sent as cheap `Bytes` clones) and lobby-state messages. Lock rule:
//! recipients are resolved under a state READ lock which is released before
//! the transport READ lock is taken for sending.

use super::tick::SessionTelemetry;
use super::GameLoopCtx;
use crate::data::{ConnectionId, PlayerId};
use crate::network::{
    CarConfigSummary, LobbyStateData, MessagePriority, ServerMessage, TrackConfigSummary,
};
use crate::transport::TransportError;
use bytes::Bytes;
use std::sync::atomic::Ordering;
use tracing::debug;

/// Fan out the tick phase's telemetry payloads to connected participants and
/// spectators. Each payload was serialized exactly once; every recipient gets
/// a `Bytes` clone via the transport's serialized send path.
pub(crate) async fn broadcast_telemetry(
    ctx: &GameLoopCtx,
    sessions: Vec<SessionTelemetry>,
    tick_count: u64,
) {
    for entry in sessions {
        // Resolve recipients under a state READ lock (lobby queries only),
        // released before any transport lock is taken.
        let (players, spectators) = {
            let state_read = ctx.state.read().await;

            // Real players must still be in this session (check via lobby manager)
            let mut players: Vec<PlayerId> = Vec::with_capacity(entry.player_recipients.len());
            for player_id in &entry.player_recipients {
                if state_read.lobby.get_player_session(*player_id).await == Some(entry.session_id) {
                    players.push(*player_id);
                }
            }

            // Also collect spectators for this session (important for DemoLap mode)
            let spectators = state_read
                .lobby
                .get_session_spectators(entry.session_id)
                .await;

            (players, spectators)
        };

        // Send under a transport READ lock (send paths only need &self).
        let transport_read = ctx.transport.read().await;

        let mut sent_players = 0usize;
        let mut sent_spectators = 0usize;
        for player_id in players {
            if let Some(conn_id) = transport_read.get_player_connection(player_id).await {
                if transport_read
                    .send_serialized(conn_id, entry.data.clone(), MessagePriority::Droppable)
                    .await
                    .is_ok()
                {
                    ctx.metrics
                        .telemetry_messages_sent
                        .fetch_add(1, Ordering::Relaxed);
                    sent_players += 1;
                }
            }
        }
        for player_id in spectators {
            if let Some(conn_id) = transport_read.get_player_connection(player_id).await {
                if transport_read
                    .send_serialized(conn_id, entry.data.clone(), MessagePriority::Droppable)
                    .await
                    .is_ok()
                {
                    ctx.metrics
                        .telemetry_messages_sent
                        .fetch_add(1, Ordering::Relaxed);
                    sent_spectators += 1;
                }
            }
        }

        if (sent_players > 0 || sent_spectators > 0) && tick_count % 60 == 0 {
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

/// Build the current `LobbyState` message from server state (state read lock).
async fn build_lobby_state(ctx: &GameLoopCtx, log_players: bool) -> ServerMessage {
    let state_read = ctx.state.read().await;

    // Get lobby players and sessions
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

/// Broadcast lobby state to all connected clients: serialized once, fanned
/// out as `Bytes` clones (LobbyState is droppable, matching its priority).
pub(crate) async fn broadcast_lobby_state(
    ctx: &GameLoopCtx,
) -> Result<(), rmp_serde::encode::Error> {
    let lobby_state = build_lobby_state(ctx, false).await;
    debug_assert_eq!(lobby_state.priority(), MessagePriority::Droppable);
    let data = Bytes::from(rmp_serde::to_vec_named(&lobby_state)?);

    // Broadcast to all connections
    ctx.transport
        .read()
        .await
        .broadcast_serialized(data, MessagePriority::Droppable)
        .await;
    Ok(())
}

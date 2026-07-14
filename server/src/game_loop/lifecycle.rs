//! Unified player-disconnect handling. Both the protocol-level `Disconnect`
//! message / stream teardown and the heartbeat-timeout cleanup converge here,
//! so session/lobby removal logic exists exactly once.

use super::GameLoopCtx;
use crate::data::{PlayerId, SessionId};
use tracing::debug;

/// Remove a disconnected player from their game session and the lobby, and
/// tear down the session if it is now empty.
pub(crate) async fn handle_player_disconnect(
    ctx: &GameLoopCtx,
    player_id: PlayerId,
    session_id: Option<SessionId>,
) {
    debug!(
        "Handling disconnected player: {} (session: {:?})",
        player_id, session_id
    );

    let mut state_write = ctx.state.write().await;

    // Remove from game session if in one
    if let Some(session_id) = session_id {
        if let Some(game_session) = state_write.sessions.get_mut(&session_id) {
            game_session.remove_player(&player_id);
        }
    }

    // Remove from lobby and check if session is now empty
    let (_, empty_session) = state_write.lobby.remove_player(player_id).await;

    if let Some(session_id) = empty_session {
        debug!(
            "Session {} is empty after player disconnect, removing it",
            session_id
        );
        state_write.sessions.remove(&session_id);
        state_write.lobby.unregister_session(session_id).await;
    }
}

/// Evict connections whose heartbeat timed out (short transport read lock),
/// then run the disconnect lifecycle for each affected player.
pub(crate) async fn cleanup_stale_connections(ctx: &GameLoopCtx) {
    let disconnected_players = {
        let transport_read = ctx.transport.read().await;
        transport_read.cleanup_stale_connections().await
    };

    for (player_id, session_id) in disconnected_players {
        handle_player_disconnect(ctx, player_id, session_id).await;
    }
}

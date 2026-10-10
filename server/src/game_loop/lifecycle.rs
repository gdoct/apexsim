//! Unified player-disconnect handling. Both the protocol-level `Disconnect`
//! message / stream teardown and the heartbeat-timeout cleanup converge here,
//! so session/lobby removal logic exists exactly once.
//!
//! A lost connection holds the player's seat (`GameSession::hold_seat`): the
//! car stays in the session, driven by the server, until they reconnect with
//! their resume token and `JoinSession` it back. A deliberate leave
//! (`Disconnect`, a kick) gives the seat up. `tick` ends a session whose
//! humans are all away for `[server] reconnect_grace_seconds`.

use super::GameLoopCtx;
use crate::data::{ConnectionId, PlayerId, SessionId};
use crate::network::{RejoinData, ServerMessage};
use crate::server::ServerState;
use tracing::debug;

/// Handle the end of `connection_id`, `player_id`'s connection (watching
/// `session_id`, or driving in it). With `keep_seat` (the connection was
/// lost) their seat is held for them; otherwise they are removed from their
/// game session and the lobby, and the session goes if it is now empty.
/// A player already back on another connection (a resume that overtook the
/// old connection's timeout) is left to that connection, which holds the
/// seat itself when it authenticates (`hold_seat_for_rejoin`).
pub(crate) async fn handle_player_disconnect(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    player_id: PlayerId,
    session_id: Option<SessionId>,
    keep_seat: bool,
) {
    debug!(
        "Handling disconnected player: {} (session: {:?}, keep seat: {})",
        player_id, session_id, keep_seat
    );

    let reconnected = ctx
        .player_connection(player_id)
        .await
        .is_some_and(|current| current != connection_id);
    if reconnected {
        debug!("Player {} is already back on another connection", player_id);
        return;
    }

    let mut state_write = ctx.state.write().await;
    state_write.showcase.leave(&player_id);

    // The seat is the lobby's record, not the connection's session: a player
    // holding a seat may be watching a menu backdrop when they drop again.
    let seat = state_write.lobby.get_player_session(player_id).await;
    let held = keep_seat
        && seat.is_some_and(|sid| {
            state_write
                .sessions
                .get_mut(&sid)
                .is_some_and(|game_session| game_session.hold_seat(&player_id))
        });
    if held {
        debug!("Holding player {}'s seat in session {:?}", player_id, seat);
        state_write.lobby.hold_seat(player_id).await;
        return;
    }

    // Remove from game session if in one
    for sid in [session_id, seat].into_iter().flatten() {
        if let Some(game_session) = state_write.sessions.get_mut(&sid) {
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

/// A player who has just authenticated holds the seat their player id has
/// in a session (they resumed): it is held now if the old connection's end
/// has not been handled yet, and the rejoin offer for the client's menu is
/// returned.
pub(crate) async fn hold_seat_for_rejoin(
    state: &mut ServerState,
    player_id: PlayerId,
) -> Option<ServerMessage> {
    let session_id = state.lobby.get_player_session(player_id).await?;
    let game_session = state.sessions.get_mut(&session_id)?;
    if !game_session.hold_seat(&player_id) {
        return None;
    }
    Some(ServerMessage::RejoinAvailable(RejoinData {
        session_id,
        track_id: game_session.session.track_config_id,
        session_kind: game_session.session.session_kind,
    }))
}

/// Give up the seat `player_id` holds in `session_id` (they created or
/// joined another session, or left it from the menu): their car leaves the
/// session, and the session goes if nobody is left in it.
pub(crate) async fn give_up_seat(
    state: &mut ServerState,
    player_id: PlayerId,
    session_id: SessionId,
) {
    debug!("Player {} gives up their seat in {}", player_id, session_id);
    if let Some(game_session) = state.sessions.get_mut(&session_id) {
        game_session.remove_player(&player_id);
    }
    if let Some(empty) = state.lobby.leave_seat(player_id).await {
        state.sessions.remove(&empty);
        state.lobby.unregister_session(empty).await;
    }
}

/// Evict connections whose heartbeat timed out (short transport read lock),
/// then run the disconnect lifecycle for each affected player (who keeps
/// their seat: a timeout is a lost connection). Resume tokens nobody can use
/// any more are forgotten here too.
pub(crate) async fn cleanup_stale_connections(ctx: &GameLoopCtx) {
    let disconnected_players = {
        let transport_read = ctx.transport.read().await;
        transport_read.cleanup_stale_connections().await
    };

    for (connection_id, player_id, session_id) in disconnected_players {
        handle_player_disconnect(ctx, connection_id, player_id, session_id, true).await;
    }

    let seated = ctx.state.read().await.lobby.seated_players().await;
    ctx.transport
        .read()
        .await
        .prune_resume_tokens(|player_id| seated.contains(player_id))
        .await;
}

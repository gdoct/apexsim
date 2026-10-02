//! Showcase channels in the game loop (`crate::showcase`): the three client
//! messages, the per-tick advance, and the fan-out of what each channel
//! hands back. Frames go out as bare record bodies over UDP (TCP, droppable,
//! for a client with no UDP); everything else reliably over TCP, wrapped as
//! `SpectatorRecord`.
//!
//! Lock rule, as everywhere in the loop: the state lock is released before
//! the transport lock is taken.

use super::GameLoopCtx;
use crate::data::{ConnectionId, PlayerId};
use crate::network::{spectator_record_message, MessagePriority, ServerMessage, ShowcasesData};
use crate::showcase::{LoadRequest, LoadedStream};
use bytes::Bytes;
use std::sync::atomic::Ordering;
use tracing::{debug, warn};

pub(crate) async fn handle_list(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    let entries = ctx.state.read().await.showcase.summaries();
    let _ = ctx
        .send(
            connection_id,
            ServerMessage::Showcases(ShowcasesData { entries }),
        )
        .await;
}

pub(crate) async fn handle_spectate(
    ctx: &GameLoopCtx,
    connection_id: ConnectionId,
    id: Option<String>,
) {
    let Some(conn_info) = ctx.connection(connection_id).await else {
        return;
    };
    if conn_info.in_session.is_some() {
        ctx.send_error(
            connection_id,
            400,
            "Leave the session before watching a showcase",
        )
        .await;
        return;
    }
    let joined = ctx
        .state
        .write()
        .await
        .showcase
        .join(conn_info.player_id, id.as_deref());
    let Some((joined, load)) = joined else {
        ctx.send_error(connection_id, 404, "No such showcase").await;
        return;
    };
    debug!(
        "Player {} watches showcase {}",
        conn_info.player_name, joined.showcase_id
    );
    let _ = ctx
        .send(connection_id, ServerMessage::SpectatorJoined(joined))
        .await;
    if let Some(request) = load {
        spawn_load(ctx, request);
    }
}

pub(crate) async fn handle_leave(ctx: &GameLoopCtx, connection_id: ConnectionId) {
    if let Some(conn_info) = ctx.connection(connection_id).await {
        leave(ctx, conn_info.player_id).await;
    }
}

/// Take a player off whatever showcase they watch: asked for, implied by a
/// session create or join, or because they are gone.
pub(crate) async fn leave(ctx: &GameLoopCtx, player_id: PlayerId) {
    // Nearly always nobody: look before taking the write lock.
    if ctx
        .state
        .read()
        .await
        .showcase
        .watching(&player_id)
        .is_none()
    {
        return;
    }
    ctx.state.write().await.showcase.leave(&player_id);
}

/// Inflate a channel's file off the loop and hand it to the channel.
fn spawn_load(ctx: &GameLoopCtx, request: LoadRequest) {
    let state = ctx.state.clone();
    tokio::spawn(async move {
        let path = request.path.clone();
        let loaded = tokio::task::spawn_blocking(move || LoadedStream::load(&path))
            .await
            .unwrap_or_else(|e| Err(format!("the load panicked: {e}")));
        state.write().await.showcase.install(&request, loaded);
    });
}

/// Advance every watched channel one tick and send what comes due.
pub(crate) async fn advance(ctx: &GameLoopCtx) {
    if !ctx.state.read().await.showcase.has_viewers() {
        return;
    }
    let (outputs, loads) = ctx.state.write().await.showcase.advance(ctx.tick_rate);
    for request in loads {
        spawn_load(ctx, request);
    }

    let transport = ctx.transport.read().await;
    for out in outputs {
        for (player_id, bodies) in &out.catch_up {
            let Some(conn_id) = transport.get_player_connection(*player_id).await else {
                continue;
            };
            // One message, however many records: a viewer's whole preamble.
            let data = Bytes::from(spectator_record_message(bodies.iter().map(|b| &b[..])));
            if let Err(e) = transport
                .send_serialized(conn_id, data, MessagePriority::Critical)
                .await
            {
                warn!(
                    "Failed to send a showcase preamble to {}: {:?}",
                    player_id, e
                );
            }
        }
        if out.reliable.is_empty() && out.frames.is_empty() {
            continue;
        }
        let reliable = (!out.reliable.is_empty()).then(|| {
            Bytes::from(spectator_record_message(
                out.reliable.iter().map(|b| &b[..]),
            ))
        });
        // Wrapped for TCP only if somebody has no UDP.
        let mut frames_tcp: Option<Bytes> = None;
        for player_id in &out.viewers {
            let Some((conn_id, udp_addr)) = transport.get_player_route(*player_id).await else {
                continue;
            };
            if let Some(data) = &reliable {
                let _ = transport
                    .send_serialized(conn_id, data.clone(), MessagePriority::Critical)
                    .await;
            }
            if out.frames.is_empty() {
                continue;
            }
            match udp_addr {
                Some(addr) => {
                    for frame in &out.frames {
                        let _ = transport.send_udp_serialized(addr, frame.clone());
                    }
                }
                None => {
                    let data = frames_tcp.get_or_insert_with(|| {
                        Bytes::from(spectator_record_message(out.frames.iter().map(|b| &b[..])))
                    });
                    let _ = transport
                        .send_serialized(conn_id, data.clone(), MessagePriority::Droppable)
                        .await;
                }
            }
            ctx.metrics
                .showcase_frames_sent
                .fetch_add(out.frames.len() as u64, Ordering::Relaxed);
        }
    }
}

/// Publish the channels for `/showcase` and `/metrics`. Once a second.
pub(crate) async fn refresh_status(ctx: &GameLoopCtx) {
    let summaries = ctx.state.read().await.showcase.summaries();
    if summaries.is_empty() && ctx.metrics.showcases().is_empty() {
        return;
    }
    ctx.metrics.set_showcases(summaries);
}

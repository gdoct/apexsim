//! Loading a track's sidecars off the loop before a session is created on it.
//!
//! The first `CreateSession` on a track has to read its sidecars from disk
//! (`crate::track_content`): tens of milliseconds for a road mesh in a
//! release build, most of a second in a debug one, longer on a cold disk.
//! Done in the handler, that would stall every session already running.
//! Instead the request is held here while a blocking task loads the track,
//! and handed back to the dispatcher once it is in memory, where it finds
//! the track loaded and creates the session as it always did.
//!
//! Everything that connection sends meanwhile is held behind it and handed
//! back in order, so the handlers see a client's messages in the order it
//! sent them: a create arriving late must not, say, land after the
//! `LeaveSession` the client sent once it had given up on it.

use super::GameLoopCtx;
use crate::data::ConnectionId;
use crate::network::ClientMessage;
use crate::transport::TransportEvent;
use std::collections::HashMap;
use tokio::sync::mpsc;
use tracing::{debug, warn};

/// The most events held for one connection while its track loads. A load
/// takes a second at worst; a client sending inputs at 120 Hz stays far
/// below this, and past it the newest are dropped rather than the server
/// growing without bound.
const MAX_HELD_EVENTS: usize = 4096;

pub(crate) struct TrackLoads {
    held: HashMap<ConnectionId, Vec<TransportEvent>>,
    /// A connection whose track has finished loading.
    done_tx: mpsc::UnboundedSender<ConnectionId>,
    done_rx: mpsc::UnboundedReceiver<ConnectionId>,
}

impl TrackLoads {
    pub(crate) fn new() -> Self {
        let (done_tx, done_rx) = mpsc::unbounded_channel();
        Self {
            held: HashMap::new(),
            done_tx,
            done_rx,
        }
    }

    /// The events held for every connection whose track has loaded since the
    /// last call, each connection's in the order they arrived. They go ahead
    /// of anything drained this tick, which arrived after them.
    pub(crate) fn release_loaded(&mut self) -> Vec<TransportEvent> {
        let mut released = Vec::new();
        while let Ok(connection_id) = self.done_rx.try_recv() {
            released.extend(self.held.remove(&connection_id).unwrap_or_default());
        }
        released
    }

    /// The event back when the dispatcher can take it now, or `None` when it
    /// was held: its connection is waiting for a track, or it is a create
    /// on a track that has to be loaded first.
    pub(crate) async fn admit(
        &mut self,
        ctx: &GameLoopCtx,
        event: TransportEvent,
    ) -> Option<TransportEvent> {
        let connection_id = match &event {
            TransportEvent::Message(connection_id, _) => *connection_id,
            TransportEvent::Disconnected { connection_id, .. } => *connection_id,
        };
        if let Some(queue) = self.held.get_mut(&connection_id) {
            if queue.len() < MAX_HELD_EVENTS {
                queue.push(event);
            } else {
                warn!(
                    "Connection {} sent over {} messages while its track loaded; dropping one",
                    connection_id, MAX_HELD_EVENTS
                );
            }
            return None;
        }
        let TransportEvent::Message(
            _,
            ClientMessage::CreateSession {
                track_config_id, ..
            },
        ) = &event
        else {
            return Some(event);
        };

        let (content, catalog) = {
            let state = ctx.state.read().await;
            if !state.track_content.needs_loading(*track_config_id) {
                return Some(event);
            }
            let Some(catalog) = state.track_configs.get(track_config_id) else {
                return Some(event);
            };
            (state.track_content.clone(), catalog.clone())
        };
        debug!(
            "Loading {} for connection {} before creating its session",
            catalog.name, connection_id
        );
        self.held.insert(connection_id, vec![event]);
        let done = self.done_tx.clone();
        tokio::spawn(async move {
            // A load that panics is caught inside `complete`; this only
            // makes sure the connection is let go whatever happens.
            let _ = tokio::task::spawn_blocking(move || {
                content.complete(&catalog);
            })
            .await;
            let _ = done.send(connection_id);
        });
        None
    }
}

//! The authoritative game loop (240Hz by default), decomposed by phase:
//!
//! - [`dispatch`]: inbound `TransportEvent` handling (per-message handlers)
//! - [`tick`]: session ticking + replay recording
//! - [`broadcast`]: telemetry fan-out and lobby-state broadcasts
//! - [`lifecycle`]: unified player-disconnect handling
//!
//! Lock discipline: inbound events are drained without any lock (the TCP
//! receiver is taken out of the transport at startup). Each phase then takes
//! the state or transport lock briefly and on its own; a state write lock and
//! a transport write lock are never held at the same time, and the transport
//! is only ever read-locked from this loop.

mod broadcast;
mod dispatch;
mod lifecycle;
mod tick;

use crate::data::{ConnectionId, PlayerId, PlayerInputData, SessionId};
use crate::metrics::ServerMetrics;
use crate::network::ServerMessage;
use crate::server::ServerState;
use crate::transport::{ConnectionInfo, TransportError, TransportLayer};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;
use tokio::time::{interval, Duration};
use tracing::warn;

/// Shared handles every game-loop phase needs. All locks are taken briefly
/// inside the helpers/phases, never held across phases.
pub(crate) struct GameLoopCtx {
    pub(crate) state: Arc<RwLock<ServerState>>,
    pub(crate) transport: Arc<RwLock<TransportLayer>>,
    pub(crate) metrics: Arc<ServerMetrics>,
    pub(crate) tick_rate: u16,
    /// Telemetry is broadcast every N-th tick (1 = every tick). The sim and
    /// replay recording always run at the full tick rate.
    pub(crate) telemetry_divisor: u64,
}

impl GameLoopCtx {
    /// Snapshot the connection info for a connection (short transport read lock).
    pub(crate) async fn connection(&self, connection_id: ConnectionId) -> Option<ConnectionInfo> {
        self.transport
            .read()
            .await
            .get_connection(connection_id)
            .await
    }

    /// Look up the connection currently associated with a player.
    pub(crate) async fn player_connection(&self, player_id: PlayerId) -> Option<ConnectionId> {
        self.transport
            .read()
            .await
            .get_player_connection(player_id)
            .await
    }

    /// Send a message to a connection (short transport read lock).
    pub(crate) async fn send(
        &self,
        connection_id: ConnectionId,
        msg: ServerMessage,
    ) -> Result<(), TransportError> {
        self.transport
            .read()
            .await
            .send_tcp(connection_id, msg)
            .await
    }

    /// Send an `Error` message to a connection, ignoring delivery failures.
    pub(crate) async fn send_error(&self, connection_id: ConnectionId, code: u16, message: &str) {
        let _ = self
            .send(
                connection_id,
                ServerMessage::Error {
                    code,
                    message: message.to_string(),
                },
            )
            .await;
    }

    /// Record which session a connection's player is in.
    pub(crate) async fn set_player_session(
        &self,
        connection_id: ConnectionId,
        session_id: Option<SessionId>,
    ) {
        self.transport
            .read()
            .await
            .set_player_session(connection_id, session_id)
            .await
    }
}

/// Run the fixed-rate game loop until the runtime shuts down.
pub(crate) async fn run_game_loop(
    state: Arc<RwLock<ServerState>>,
    transport: Arc<RwLock<TransportLayer>>,
    tick_rate: u16,
    metrics: Arc<ServerMetrics>,
) {
    // Take the inbound event receiver (TCP + handshake-bound UDP) out of the
    // transport once, so draining messages requires no transport lock at all.
    let mut inbound_events = transport
        .write()
        .await
        .take_event_receiver()
        .expect("event receiver already taken: only one game loop may run per transport");

    let telemetry_divisor = state.read().await.config.network.telemetry_divisor.max(1) as u64;

    let ctx = GameLoopCtx {
        state,
        transport,
        metrics,
        tick_rate,
        telemetry_divisor,
    };

    let tick_duration = Duration::from_micros((1_000_000.0 / tick_rate as f64) as u64);
    let mut ticker = interval(tick_duration);
    // Skip missed ticks instead of bursting to catch up: the sim advances a
    // fixed dt per tick, so bursting only amplifies a stall.
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let mut tick_count = 0u64;
    let mut player_inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();

    loop {
        ticker.tick().await;
        let tick_start = std::time::Instant::now();
        tick_count += 1;

        // Drain inbound events fast (lock-free), then dispatch each one with
        // short per-handler locks.
        let mut drained = Vec::new();
        while let Ok(Some(event)) =
            tokio::time::timeout(Duration::from_micros(100), inbound_events.recv()).await
        {
            drained.push(event);
        }
        for event in drained {
            dispatch::handle_event(&ctx, event, &mut player_inputs).await;
        }

        // Cleanup stale connections every second and handle their players.
        if tick_count.is_multiple_of(tick_rate as u64) {
            lifecycle::cleanup_stale_connections(&ctx).await;
        }

        // Broadcast lobby state every 2 seconds.
        if tick_count.is_multiple_of(tick_rate as u64 * 2) {
            if let Err(e) = broadcast::broadcast_lobby_state(&ctx).await {
                warn!("Failed to broadcast lobby state: {:?}", e);
            }
        }

        // Advance all sessions; telemetry is computed and serialized once per
        // session here and fanned out below. Roster updates (car index →
        // player mapping for compact telemetry) go out reliably over TCP.
        let output = tick::tick_sessions(&ctx, &player_inputs, tick_count).await;
        broadcast::broadcast_rosters(&ctx, output.rosters).await;
        broadcast::broadcast_telemetry(&ctx, output.telemetry, tick_count).await;

        // Cleanup timed-out finished sessions and refresh gauges.
        tick::cleanup_finished_sessions(&ctx, tick_count).await;

        let elapsed = tick_start.elapsed();
        ctx.metrics.observe_tick(elapsed);
        if elapsed > tick_duration {
            warn!(
                "Tick {} overran budget: {:?} > {:?}",
                tick_count, elapsed, tick_duration
            );
        }
    }
}

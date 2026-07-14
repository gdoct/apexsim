//! Session ticking: advance every session by one fixed step (with a panic
//! boundary per session), drive replay recording, and prepare the telemetry
//! payloads for the broadcast phase. Runs entirely under one state write
//! lock; no transport lock is touched here.

use super::GameLoopCtx;
use crate::data::*;
use crate::network::ServerMessage;
use bytes::Bytes;
use std::collections::HashMap;
use tracing::{debug, error, warn};

/// Telemetry for one session, serialized exactly once per tick. The broadcast
/// phase fans out cheap `Bytes` clones to every recipient.
pub(crate) struct SessionTelemetry {
    pub session_id: SessionId,
    /// The `ServerMessage::Telemetry` pre-encoded with `rmp_serde::to_vec_named`.
    pub data: Bytes,
    /// Non-AI participants at tick time; broadcast still verifies lobby
    /// membership and connection liveness before sending.
    pub player_recipients: Vec<PlayerId>,
    pub participant_count: usize,
    pub session_state: SessionState,
}

/// Advance all sessions one tick and return the telemetry payloads to
/// broadcast. `get_telemetry()` is called at most once per session per tick;
/// the result feeds both replay recording and the serialized broadcast.
pub(crate) async fn tick_sessions(
    ctx: &GameLoopCtx,
    player_inputs: &HashMap<PlayerId, PlayerInputData>,
    tick_count: u64,
) -> Vec<SessionTelemetry> {
    let tick_rate = ctx.tick_rate;
    let mut state_write = ctx.state.write().await;

    // Collect replay operations to execute after iteration
    let mut replay_starts = Vec::new();
    let mut replay_frames = Vec::new();
    let mut replay_stops = Vec::new();

    // Collect sessions to remove (empty or finished)
    let mut sessions_to_remove = Vec::new();

    let mut telemetry_out = Vec::new();

    // Tick each session
    for (session_id, game_session) in state_write.sessions.iter_mut() {
        // Check if session has no real (non-AI) players left
        let real_player_count = game_session
            .session
            .participants
            .keys()
            .filter(|player_id| !game_session.session.ai_player_ids.contains(player_id))
            .count();

        // Keep session alive if it's in Demo Lap mode with AI drivers
        let is_demo_lap_with_ai = game_session.session.game_mode == GameMode::DemoLap
            && !game_session.session.ai_player_ids.is_empty();

        if real_player_count == 0 && !is_demo_lap_with_ai {
            debug!(
                "Session {} has no real players, marking for removal",
                session_id
            );
            sessions_to_remove.push(*session_id);
            continue;
        }

        // Build this session's input map: human inputs are looked up by key
        // (no clone of the global input map), AI inputs are generated.
        let mut session_inputs: HashMap<PlayerId, PlayerInputData> =
            HashMap::with_capacity(game_session.session.participants.len());
        for player_id in game_session.session.participants.keys() {
            let input = match player_inputs.get(player_id) {
                Some(input) => *input,
                // No human input, generate AI input
                None => game_session.generate_ai_input(player_id),
            };
            session_inputs.insert(*player_id, input);
        }

        let prev_state = game_session.session.state;
        // Panic boundary: one session's tick must never take down the
        // whole server. A panicking session is terminated instead.
        let tick_result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            game_session.tick(&session_inputs)
        }));
        if let Err(panic) = tick_result {
            let panic_msg = panic
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| panic.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "unknown panic".to_string());
            error!(
                "Session {} tick panicked: {} — terminating session",
                session_id, panic_msg
            );
            ctx.metrics
                .session_tick_panics
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            game_session.session.state = SessionState::Finished;
            sessions_to_remove.push(*session_id);
            continue;
        }
        let new_state = game_session.session.state;

        // Collect replay recording operations
        if prev_state != SessionState::Racing && new_state == SessionState::Racing {
            let participants: Vec<_> = game_session
                .session
                .participants
                .keys()
                .map(|pid| crate::replay::ReplayParticipant {
                    player_id: *pid,
                    player_name: format!("Player-{}", pid), // TODO: Get actual names
                    car_config_id: game_session
                        .session
                        .participants
                        .get(pid)
                        .map(|cs| cs.car_config_id)
                        .unwrap_or_else(uuid::Uuid::nil),
                    finish_position: None,
                })
                .collect();

            let track_config_id = game_session.session.track_config_id;
            let metadata = (*session_id, track_config_id, participants);
            replay_starts.push(metadata);
        }

        // Telemetry: computed ONCE per session per tick and reused for both
        // the replay frame (when racing) and the broadcast payload (when the
        // session is in an active state).
        let should_broadcast = matches!(
            new_state,
            SessionState::Countdown | SessionState::Racing | SessionState::Finished
        );
        if should_broadcast || new_state == SessionState::Racing {
            let telemetry_msg = game_session.get_telemetry();

            if should_broadcast {
                match rmp_serde::to_vec_named(&telemetry_msg) {
                    Ok(data) => {
                        let player_recipients: Vec<PlayerId> = game_session
                            .session
                            .participants
                            .keys()
                            .filter(|pid| !game_session.session.ai_player_ids.contains(pid))
                            .cloned()
                            .collect();
                        telemetry_out.push(SessionTelemetry {
                            session_id: *session_id,
                            data: Bytes::from(data),
                            player_recipients,
                            participant_count: game_session.session.participants.len(),
                            session_state: new_state,
                        });
                    }
                    Err(e) => {
                        error!(
                            "Failed to serialize telemetry for session {}: {}",
                            session_id, e
                        );
                    }
                }
            }

            // Collect telemetry frame if racing
            if new_state == SessionState::Racing {
                if let ServerMessage::Telemetry(tel) = telemetry_msg {
                    replay_frames.push((*session_id, game_session.session.current_tick, tel));
                }
            }
        }

        // Collect replay stops when session finishes
        if prev_state == SessionState::Racing && new_state == SessionState::Finished {
            replay_stops.push(*session_id);
        }

        // Log state changes
        if game_session.session.current_tick % tick_rate as u32 == 0 {
            match new_state {
                SessionState::Countdown => {
                    if let Some(countdown) = game_session.session.countdown_ticks_remaining {
                        let seconds = countdown / tick_rate;
                        debug!("Session {} countdown: {}s", session_id, seconds);
                    }
                }
                SessionState::Racing => {
                    let lap_info: Vec<String> = game_session
                        .session
                        .participants
                        .values()
                        .map(|s| format!("L{}", s.current_lap))
                        .collect();
                    debug!("Session {} racing - Laps: {:?}", session_id, lap_info);
                }
                SessionState::Finished => {
                    debug!("Session {} finished", session_id);
                }
                _ => {}
            }
        }
    }

    // Execute collected replay operations
    for (session_id, track_config_id, participants) in replay_starts {
        use crate::replay::ReplayMetadata;

        let track_name = state_write
            .track_configs
            .get(&track_config_id)
            .map(|t| t.name.clone())
            .unwrap_or_else(|| "Unknown Track".to_string());

        let metadata = ReplayMetadata {
            session_id,
            track_config_id,
            track_name,
            recorded_at: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs(),
            duration_ticks: 0,
            tick_rate,
            participants,
        };

        state_write.replay.start_recording(metadata).await;
        debug!("Started replay recording for session {}", session_id);
    }

    for (session_id, tick, telemetry) in replay_frames {
        state_write
            .replay
            .record_frame(session_id, tick, telemetry)
            .await;
    }

    for session_id in replay_stops {
        match state_write.replay.stop_recording(session_id).await {
            Ok(replay_path) => {
                debug!(
                    "Replay saved for session {} to {:?}",
                    session_id, replay_path
                );
            }
            Err(e) => {
                warn!("Failed to save replay for session {}: {}", session_id, e);
            }
        }
    }

    // Remove empty sessions from the game state and lobby
    for session_id in sessions_to_remove {
        state_write.sessions.remove(&session_id);
        state_write.lobby.unregister_session(session_id).await;
        debug!("Removed empty session {}", session_id);
    }

    // Periodic cleanup of orphaned lobby sessions (every 5 seconds)
    if tick_count % (tick_rate as u64 * 5) == 0 {
        let lobby_session_ids: Vec<SessionId> = state_write.lobby.get_all_session_ids().await;
        for session_id in lobby_session_ids {
            if !state_write.sessions.contains_key(&session_id) {
                debug!("Cleaning up orphaned lobby session {}", session_id);
                state_write.lobby.unregister_session(session_id).await;
            }
        }
    }

    telemetry_out
}

/// Remove finished sessions older than the configured timeout and refresh
/// the once-per-second gauges. Runs after the telemetry broadcast so a
/// finished session still gets its final telemetry.
pub(crate) async fn cleanup_finished_sessions(ctx: &GameLoopCtx, tick_count: u64) {
    let tick_rate = ctx.tick_rate;
    let mut state_write = ctx.state.write().await;

    // Cleanup finished sessions (older than timeout)
    let timeout_seconds = state_write.config.server.session_timeout_seconds as u64;
    state_write.sessions.retain(|id, session| {
        if session.session.state == SessionState::Finished {
            let age_ticks = tick_count.saturating_sub(session.session.current_tick as u64);
            let age_seconds = age_ticks / tick_rate as u64;

            if age_seconds > timeout_seconds {
                debug!("Removing finished session: {}", id);
                return false;
            }
        }
        true
    });

    // Refresh gauges once per second
    if tick_count % tick_rate as u64 == 0 {
        ctx.metrics.active_sessions.store(
            state_write.sessions.len() as u64,
            std::sync::atomic::Ordering::Relaxed,
        );
        ctx.metrics.connected_players.store(
            state_write.lobby.get_lobby_count().await as u64,
            std::sync::atomic::Ordering::Relaxed,
        );
    }
}

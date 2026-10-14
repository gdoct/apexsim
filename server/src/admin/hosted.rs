//! Sessions the server runs itself, from the dashboard or its schedule
//! (docs/server/operations.md "Hosted sessions").
//!
//! A hosted session is an ordinary multiplayer session whose host is the
//! server ([`SERVER_HOST_ID`], which no connection ever has): it is listed
//! in the lobby for players to join, stays when nobody is in it
//! (`GameSession::hosted`, `LobbySessionInfo::persistent`), and only the
//! dashboard starts, advances, restarts and ends it. Ending and starting
//! work on any session, so an operator can also stop a player's.
//!
//! Every operation takes the state lock briefly, as a game-loop handler
//! does, and sends after dropping it.

use crate::data::*;
use crate::game_loop::GameLoopCtx;
use crate::lobby::{LobbySessionInfo, SessionVisibility};
use crate::network::ServerMessage;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Mutex;
use tracing::info;

/// The host of every hosted session: the server, never a connection.
pub const SERVER_HOST_ID: PlayerId = PlayerId::from_u128(0x0000_0000_0000_4000_8000_5e7e_7000_0001);

/// Most AI cars and player slots a hosted session takes, each.
pub const MAX_HOSTED_CARS: u8 = 30;

/// What a hosted session is: everything a player's `CreateSession` names,
/// as the dashboard's form and the schedule file write it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct HostedSpec {
    /// What the lobby lists as the host ("Sunday Cup"); "Server" when empty.
    #[serde(default)]
    pub name: String,
    pub track_id: TrackConfigId,
    /// The class the AI field races in (`game_session::class_field`).
    pub car_id: CarConfigId,
    /// Seats for players, on top of the AI.
    #[serde(default = "default_player_slots")]
    pub player_slots: u8,
    #[serde(default)]
    pub ai_count: u8,
    /// The AI field's level; the mixed field when absent.
    #[serde(default)]
    pub ai_skill: Option<u8>,
    /// Race distance in laps (ignored for a timed race).
    #[serde(default = "default_laps")]
    pub lap_limit: u8,
    /// A timed race's length, s.
    #[serde(default)]
    pub race_seconds: Option<u32>,
    /// Practice and knockout qualifying before the race.
    #[serde(default)]
    pub weekend: Option<crate::weekend::WeekendFormat>,
    #[serde(default)]
    pub conditions: SessionConditions,
    #[serde(default)]
    pub damage: DamageLevel,
    #[serde(default)]
    pub allowed_assists: AllowedAssists,
}

fn default_player_slots() -> u8 {
    12
}

fn default_laps() -> u8 {
    10
}

impl HostedSpec {
    fn host_name(&self) -> String {
        let name = self.name.trim();
        if name.is_empty() {
            "Server".to_string()
        } else {
            name.chars().take(40).collect()
        }
    }
}

/// The spec of every hosted session still running, for a restart and for
/// the dashboard to tell its own sessions from the players'.
#[derive(Default)]
pub struct HostedSessions {
    specs: Mutex<HashMap<SessionId, HostedSpec>>,
}

impl HostedSessions {
    pub fn spec(&self, id: SessionId) -> Option<HostedSpec> {
        self.lock().get(&id).cloned()
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, HashMap<SessionId, HostedSpec>> {
        self.specs.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Forget the sessions that are gone (finished and timed out).
    pub fn retain(&self, live: impl Fn(&SessionId) -> bool) {
        self.lock().retain(|id, _| live(id));
    }
}

/// Create a hosted session in its lobby, listed for players to join.
pub(crate) async fn open(
    ctx: &GameLoopCtx,
    hosted: &HostedSessions,
    spec: HostedSpec,
) -> Result<SessionId, String> {
    // The track's sidecars are read off the lock, as `track_loads` does for
    // a player's create, so no session already running waits for the disk.
    let (content, catalog) = {
        let st = ctx.state.read().await;
        let Some(track) = st.track_configs.get(&spec.track_id) else {
            return Err("no such track".into());
        };
        if !st.car_configs.contains_key(&spec.car_id) {
            return Err("no such car".into());
        }
        if st.disabled_content.contains(&spec.track_id) {
            return Err("that track is switched off on this server".into());
        }
        if st.disabled_content.contains(&spec.car_id) {
            return Err("that car is switched off on this server".into());
        }
        (st.track_content.clone(), track.clone())
    };
    let track_name = catalog.name.clone();
    if content.needs_loading(spec.track_id) {
        tokio::task::spawn_blocking(move || {
            content.complete(&catalog);
        })
        .await
        .map_err(|_| "loading the track failed".to_string())?;
    }

    let ai_count = spec.ai_count.min(MAX_HOSTED_CARS);
    let player_slots = spec.player_slots.clamp(1, MAX_HOSTED_CARS);
    let conditions = spec.conditions.clamp();
    let mut st = ctx.state.write().await;
    let Some(session_id) = st.create_session(
        SERVER_HOST_ID,
        spec.car_id,
        spec.track_id,
        SessionKind::Multiplayer,
        ai_count + player_slots,
        ai_count,
        spec.lap_limit.max(1),
        spec.allowed_assists,
        conditions,
        spec.ai_skill,
        spec.race_seconds,
    ) else {
        return Err(
            if st.sessions.len() >= st.config.server.max_sessions as usize {
                "the server is at its session limit ([server] max_sessions)"
            } else {
                "the session could not be created"
            }
            .into(),
        );
    };
    let Some(gs) = st.sessions.get_mut(&session_id) else {
        return Err("the session could not be created".into());
    };
    gs.hosted = true;
    gs.set_damage(spec.damage);
    gs.set_weekend(spec.weekend.clone().map(|w| w.clamp()));
    let (conditions, lap_limit, race_seconds) = (
        gs.session.conditions,
        gs.session.lap_limit,
        gs.session.race_seconds,
    );
    let track_file = catalog_file(&st, spec.track_id);
    let info = LobbySessionInfo {
        session_id,
        host_player_id: SERVER_HOST_ID,
        host_name: spec.host_name(),
        track_name: track_name.clone(),
        track_file,
        track_config_id: spec.track_id,
        session_kind: SessionKind::Multiplayer,
        conditions,
        damage: spec.damage,
        lap_limit,
        race_seconds,
        max_players: ai_count + player_slots,
        current_player_count: 0,
        spectator_count: 0,
        state: SessionState::Lobby,
        visibility: SessionVisibility::Public,
        password_hash: None,
        created_at: std::time::Instant::now(),
        persistent: true,
    };
    st.lobby.register_session(info).await;
    drop(st);
    info!(
        "Admin opened hosted session {} ({} on {}, {} AI)",
        session_id,
        spec.host_name(),
        track_name,
        ai_count
    );
    hosted.lock().insert(session_id, spec);
    Ok(session_id)
}

fn catalog_file(st: &crate::server::ServerState, track: TrackConfigId) -> String {
    st.track_configs
        .get(&track)
        .and_then(|t| t.source_path.clone())
        .unwrap_or_default()
}

/// The humans driving in a session and those watching it.
async fn audience(st: &crate::server::ServerState, id: SessionId) -> Vec<PlayerId> {
    let mut out: Vec<PlayerId> = st
        .sessions
        .get(&id)
        .map(|gs| {
            gs.session
                .participants
                .keys()
                .filter(|p| !gs.session.ai_player_ids.contains(p))
                .copied()
                .collect()
        })
        .unwrap_or_default();
    out.extend(st.lobby.get_session_spectators(id).await);
    out
}

async fn send_to(ctx: &GameLoopCtx, players: &[PlayerId], msg: ServerMessage) {
    for player in players {
        if let Some(conn) = ctx.player_connection(*player).await {
            let _ = ctx.send(conn, msg.clone()).await;
        }
    }
}

/// Start a session in its lobby: a weekend into its first phase, anything
/// else into the race's countdown. The session's own host would do this
/// from the game.
pub(crate) async fn start(ctx: &GameLoopCtx, id: SessionId) -> Result<&'static str, String> {
    let mut st = ctx.state.write().await;
    let Some(gs) = st.sessions.get_mut(&id) else {
        return Err("no such session".into());
    };
    if gs.session.state != SessionState::Lobby {
        return Err("the session has already started".into());
    }
    let msg = if gs.is_weekend() {
        gs.advance_weekend().map_err(str::to_string)?;
        ServerMessage::GameModeChanged {
            mode: gs.session.game_mode,
        }
    } else {
        if !gs.start_countdown() {
            return Err("the session has already started".into());
        }
        ServerMessage::SessionStarting {
            countdown_seconds: crate::game_session::START_SESSION_COUNTDOWN_SECONDS as u8,
        }
    };
    let mode = gs.session.game_mode;
    let players = audience(&st, id).await;
    drop(st);
    send_to(ctx, &players, msg).await;
    info!("Admin started session {}", id);
    Ok(mode_name(mode))
}

/// Move a race weekend on to its next phase, as its host would
/// (`GameSession::advance_weekend`).
pub(crate) async fn advance(ctx: &GameLoopCtx, id: SessionId) -> Result<&'static str, String> {
    let mut st = ctx.state.write().await;
    let Some(gs) = st.sessions.get_mut(&id) else {
        return Err("no such session".into());
    };
    gs.advance_weekend().map_err(str::to_string)?;
    let mode = gs.session.game_mode;
    let players = audience(&st, id).await;
    drop(st);
    send_to(ctx, &players, ServerMessage::GameModeChanged { mode }).await;
    info!("Admin advanced session {} to {:?}", id, mode);
    Ok(mode_name(mode))
}

fn mode_name(mode: GameMode) -> &'static str {
    match mode {
        GameMode::Countdown | GameMode::Grid | GameMode::Race => "race",
        GameMode::Practice => "practice",
        GameMode::Qualification => "qualifying",
        _ => "session",
    }
}

/// Whether a hosted weekend is waiting between phases (the scheduler moves
/// it on).
pub(crate) async fn awaits_advance(ctx: &GameLoopCtx, id: SessionId) -> bool {
    ctx.state
        .read()
        .await
        .sessions
        .get(&id)
        .is_some_and(|gs| gs.awaits_weekend_advance())
}

/// End a session now: every human in it (or watching it) is told why and
/// sent back to the menu, and it is gone from the lobby.
pub(crate) async fn stop(
    ctx: &GameLoopCtx,
    hosted: &HostedSessions,
    id: SessionId,
    reason: &str,
) -> Result<(), String> {
    end(ctx, hosted, id, reason, true).await
}

/// Remove a session, telling everyone in it `reason`; `send_back` also sends
/// them `SessionLeft` (a restart seats them again instead).
async fn end(
    ctx: &GameLoopCtx,
    hosted: &HostedSessions,
    id: SessionId,
    reason: &str,
    send_back: bool,
) -> Result<(), String> {
    let mut st = ctx.state.write().await;
    if !st.sessions.contains_key(&id) {
        return Err("no such session".into());
    }
    let players = audience(&st, id).await;
    for player in &players {
        // The connection id is not read; any will do.
        st.lobby.leave_session(*player, ConnectionId::nil()).await;
    }
    st.sessions.remove(&id);
    st.lobby.unregister_session(id).await;
    drop(st);
    hosted.lock().remove(&id);
    for player in &players {
        if let Some(conn) = ctx.player_connection(*player).await {
            let _ = ctx
                .send(
                    conn,
                    ServerMessage::ServerNotice {
                        message: reason.to_string(),
                    },
                )
                .await;
            if send_back {
                let _ = ctx.send(conn, ServerMessage::SessionLeft).await;
            }
            ctx.set_player_session(conn, None).await;
        }
    }
    info!(
        "Admin ended session {} ({} players {}): {}",
        id,
        players.len(),
        if send_back { "sent back" } else { "moving on" },
        reason
    );
    Ok(())
}

/// End a hosted session and open it again with the same settings, in its
/// lobby. Its connected drivers are seated in the new one as a join would
/// seat them (their car and livery, the session's rules, the racing line),
/// and its spectators watch it; the client takes that `SessionJoined`
/// without a `SessionLeft` in between. A driver whose seat was being held
/// (disconnected) is not carried over.
pub(crate) async fn restart(
    ctx: &GameLoopCtx,
    hosted: &HostedSessions,
    id: SessionId,
) -> Result<SessionId, String> {
    let Some(spec) = hosted.spec(id) else {
        return Err("only a session the server hosts can be restarted".into());
    };
    let (drivers, spectators) = {
        let st = ctx.state.read().await;
        let drivers: Vec<PlayerId> = st
            .sessions
            .get(&id)
            .map(|gs| {
                gs.session
                    .participants
                    .keys()
                    .filter(|p| !gs.session.ai_player_ids.contains(p) && !gs.is_away(p))
                    .copied()
                    .collect()
            })
            .unwrap_or_default();
        (drivers, st.lobby.get_session_spectators(id).await)
    };
    end(ctx, hosted, id, "The server restarted this session", false).await?;
    let new_id = open(ctx, hosted, spec).await?;
    for player in drivers {
        if let Some(conn) = ctx.player_connection(player).await {
            crate::game_loop::dispatch::handle_join_session(ctx, conn, new_id).await;
        }
    }
    for player in spectators {
        if let Some(conn) = ctx.player_connection(player).await {
            crate::game_loop::dispatch::handle_join_as_spectator(ctx, conn, new_id).await;
        }
    }
    Ok(new_id)
}

/// Take an AI car out of a session.
pub(crate) async fn remove_ai(
    ctx: &GameLoopCtx,
    id: SessionId,
    ai: PlayerId,
) -> Result<String, String> {
    let mut st = ctx.state.write().await;
    let Some(gs) = st.sessions.get_mut(&id) else {
        return Err("no such session".into());
    };
    let name = gs
        .get_ai_profile(&ai)
        .map(|p| p.name.clone())
        .unwrap_or_default();
    if !gs.remove_ai(&ai) {
        return Err("that car is not one of the session's AI".into());
    }
    drop(st);
    info!("Admin removed AI driver {} from session {}", name, id);
    Ok(name)
}

/// Who a broadcast goes to.
pub enum Audience {
    Everyone,
    Session(SessionId),
}

/// Show every connected player (or a session's) a message from the
/// operator. Returns how many it was sent to.
pub(crate) async fn broadcast(ctx: &GameLoopCtx, to: Audience, message: &str) -> usize {
    let players: Vec<PlayerId> = match to {
        Audience::Everyone => ctx
            .transport
            .read()
            .await
            .connection_snapshots()
            .await
            .into_iter()
            .map(|c| c.player_id)
            .collect(),
        Audience::Session(id) => audience(&*ctx.state.read().await, id).await,
    };
    let mut sent = 0;
    for player in players {
        if let Some(conn) = ctx.player_connection(player).await {
            let msg = ServerMessage::ServerNotice {
                message: message.to_string(),
            };
            if ctx.send(conn, msg).await.is_ok() {
                sent += 1;
            }
        }
    }
    info!("Admin broadcast to {} players: {}", sent, message);
    sent
}

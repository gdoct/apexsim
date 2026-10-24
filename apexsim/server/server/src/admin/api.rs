//! The dashboard's JSON endpoints. Everything here has already passed the
//! access-token check in `admin::route`.
//!
//! The server state is read under its read lock and copied into a
//! `serde_json::Value` before the lock is released, so a slow browser never
//! holds the game loop up.

use super::bans::BanEntry;
use super::hosted::{self, HostedSpec};
use super::schedule::ScheduleEntry;
use super::{logbuf, AdminContext, Resp};
use crate::data::*;
use crate::game_session::GameSession;
use bytes::Bytes;
use http_body_util::Full;
use hyper::{header, Method, Response, StatusCode};
use serde_json::{json, Value};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};
use uuid::Uuid;

pub fn json(status: StatusCode, value: Value) -> Resp {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json")
        .header(header::CACHE_CONTROL, "no-store")
        .body(Full::new(Bytes::from(value.to_string())))
        .expect("static response construction cannot fail")
}

pub fn error(status: StatusCode, message: &str) -> Resp {
    json(status, json!({ "error": message }))
}

fn ok(value: Value) -> Resp {
    json(StatusCode::OK, value)
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// `a=1&b=x%20y` as pairs, percent-decoded.
fn query_pairs(query: &str) -> Vec<(String, String)> {
    query
        .split('&')
        .filter(|kv| !kv.is_empty())
        .map(|kv| {
            let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
            (percent_decode(k), percent_decode(v))
        })
        .collect()
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < bytes.len() => {
                let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).ok();
                match hex.and_then(|h| u8::from_str_radix(h, 16).ok()) {
                    Some(b) => {
                        out.push(b);
                        i += 2;
                    }
                    None => out.push(b'%'),
                }
            }
            b => out.push(b),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn param<'a>(pairs: &'a [(String, String)], key: &str) -> Option<&'a str> {
    pairs
        .iter()
        .find(|(k, _)| k == key)
        .map(|(_, v)| v.as_str())
}

pub async fn handle(
    ctx: &AdminContext,
    method: &Method,
    path: &str,
    query: &str,
    body: Bytes,
) -> Resp {
    let segments: Vec<&str> = path
        .trim_start_matches("/api/")
        .split('/')
        .filter(|s| !s.is_empty())
        .collect();
    let q = query_pairs(query);

    match (method, segments.as_slice()) {
        (&Method::GET, ["overview"]) => overview(ctx).await,
        (&Method::GET, ["sessions"]) => sessions(ctx).await,
        (&Method::GET, ["sessions", id]) => match id.parse::<Uuid>() {
            Ok(id) => session_detail(ctx, id).await,
            Err(_) => error(StatusCode::BAD_REQUEST, "bad session id"),
        },
        (&Method::POST, ["sessions"]) => {
            open_session(ctx, &body, param(&q, "start") == Some("1")).await
        }
        (&Method::POST, ["sessions", id, action @ ("start" | "advance" | "stop" | "restart")]) => {
            match id.parse::<Uuid>() {
                Ok(id) => session_action(ctx, id, action, &body).await,
                Err(_) => error(StatusCode::BAD_REQUEST, "bad session id"),
            }
        }
        (&Method::POST, ["sessions", id, "ai", ai, "remove"]) => {
            match (id.parse::<Uuid>(), ai.parse::<Uuid>()) {
                (Ok(id), Ok(ai)) => match hosted::remove_ai(&ctx.loop_ctx, id, ai).await {
                    Ok(name) => ok(json!({ "ok": true, "name": name })),
                    Err(why) => error(StatusCode::CONFLICT, &why),
                },
                _ => error(StatusCode::BAD_REQUEST, "bad id"),
            }
        }
        (&Method::POST, ["broadcast"]) => broadcast(ctx, &body).await,
        (&Method::GET, ["schedule"]) => schedule_list(ctx).await,
        (&Method::POST, ["schedule"]) => schedule_put(ctx, &body).await,
        (&Method::DELETE, ["schedule", id]) => match id.parse::<Uuid>() {
            Ok(id) => match ctx.schedule.remove(id) {
                Some(_) => ok(json!({ "ok": true })),
                None => error(StatusCode::NOT_FOUND, "no such entry"),
            },
            Err(_) => error(StatusCode::BAD_REQUEST, "bad id"),
        },
        (&Method::GET, ["events"]) => events_list(ctx).await,
        (&Method::GET, ["events", id]) => event_detail(ctx, id).await,
        (&Method::GET, ["events", id, file @ ("sheets.json" | "sheets.csv")]) => {
            event_sheets(ctx, id, file).await
        }
        (&Method::POST, ["events"]) => event_create(ctx, &body).await,
        (
            &Method::POST,
            ["events", id, action @ ("open" | "start" | "advance" | "close" | "entries" | "schedule")],
        ) => event_action(ctx, id, action, &body).await,
        (&Method::POST, ["events", id, "entries", driver, "remove"]) => {
            match driver.parse::<Uuid>() {
                Ok(driver) => event_remove_entry(ctx, id, driver).await,
                Err(_) => error(StatusCode::BAD_REQUEST, "bad driver id"),
            }
        }
        (&Method::DELETE, ["events", id]) => event_delete(ctx, id).await,
        (&Method::GET, ["championships"]) => championships_list(ctx).await,
        (&Method::POST, ["championships"]) => championship_create(ctx, &body).await,
        (&Method::POST, ["championships", id]) => championship_update(ctx, id, &body).await,
        (&Method::DELETE, ["championships", id]) => championship_delete(ctx, id).await,
        (&Method::POST, ["content", "toggle"]) => content_toggle(ctx, &body).await,
        (&Method::POST, ["content", "upload"]) => content_upload(ctx, &q, body).await,
        (&Method::GET, ["players"]) => players(ctx).await,
        (&Method::POST, ["players", id, action @ ("kick" | "ban")]) => match id.parse::<Uuid>() {
            Ok(id) => remove_player(ctx, id, *action == "ban", &body).await,
            Err(_) => error(StatusCode::BAD_REQUEST, "bad player id"),
        },
        (&Method::POST, ["players", id, action @ ("mute" | "unmute")]) => {
            match id.parse::<Uuid>() {
                Ok(id) => {
                    let changed = ctx
                        .state
                        .write()
                        .await
                        .chat
                        .set_muted(id, *action == "mute");
                    tracing::info!("Admin {} chat for player {}", action, id);
                    ok(json!({ "ok": true, "changed": changed }))
                }
                Err(_) => error(StatusCode::BAD_REQUEST, "bad player id"),
            }
        }
        (&Method::GET, ["bans"]) => ok(json!({ "bans": ctx.bans.entries() })),
        (&Method::POST, ["bans", "remove"]) => unban(ctx, &body),
        (&Method::GET, ["history"]) => history(ctx).await,
        (&Method::GET, ["incidents"]) => incidents(ctx).await,
        (&Method::GET, ["sessions", id, "incidents"]) => match id.parse::<Uuid>() {
            Ok(id) => session_incidents(ctx, id).await,
            Err(_) => error(StatusCode::BAD_REQUEST, "bad session id"),
        },
        (&Method::POST, ["sessions", id, "penalties", car, index, "clear"]) => {
            match (
                id.parse::<Uuid>(),
                car.parse::<Uuid>(),
                index.parse::<usize>(),
            ) {
                (Ok(id), Ok(car), Ok(index)) => clear_penalty(ctx, id, car, index).await,
                _ => error(StatusCode::BAD_REQUEST, "bad id"),
            }
        }
        (&Method::POST, ["sessions", id, "penalties", car, "give"]) => {
            match (id.parse::<Uuid>(), car.parse::<Uuid>()) {
                (Ok(id), Ok(car)) => give_penalty(ctx, id, car, &body).await,
                _ => error(StatusCode::BAD_REQUEST, "bad id"),
            }
        }
        (&Method::GET, ["replays", file]) => replay_download(ctx, file).await,
        (&Method::GET, ["logs"]) => logs(&q),
        (&Method::GET, ["logs", "download"]) => logs_download(&q),
        (&Method::GET, ["perf"]) => perf(ctx).await,
        (&Method::GET, ["config"]) => config_get(ctx),
        (&Method::PUT, ["config"]) => config_put(ctx, &body).await,
        (&Method::POST, ["server", "restart"]) => server_restart(ctx),
        (&Method::GET, ["content"]) => content(ctx).await,
        _ => error(StatusCode::NOT_FOUND, "no such endpoint"),
    }
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------

async fn overview(ctx: &AdminContext) -> Resp {
    let healthy = *ctx.health.is_healthy.read().await;
    let ready = *ctx.health.is_ready.read().await;
    let status = if !healthy {
        "stopping"
    } else if !ready {
        "starting"
    } else {
        "online"
    };
    let (sessions, humans_in_sessions, tick_rate, cfg) = {
        let st = ctx.state.read().await;
        let sessions = st.sessions.len();
        let humans = st
            .sessions
            .values()
            .map(|s| {
                s.session
                    .participants
                    .keys()
                    .filter(|p| !s.ai_profiles.contains_key(p))
                    .count()
            })
            .sum::<usize>();
        (
            sessions,
            humans,
            st.config.server.tick_rate_hz,
            st.config.clone(),
        )
    };
    let connected = ctx
        .transport
        .read()
        .await
        .connection_snapshots()
        .await
        .len();
    let samples = ctx.history.samples();
    let achieved = samples.last().map(|s| s.hz);
    // As the transport decides it: a certificate and key that are really there.
    let tls_game = Path::new(cfg.network.tls_cert_path.trim()).is_file()
        && Path::new(cfg.network.tls_key_path.trim()).is_file();
    ok(json!({
        "version": env!("APEXSIM_BUILD_VERSION"),
        "status": status,
        "uptime_s": ctx.started.elapsed().as_secs(),
        "players": connected,
        "players_in_sessions": humans_in_sessions,
        "sessions": sessions,
        "tick_target_hz": tick_rate,
        "tick_hz": achieved,
        "tcp_bind": cfg.network.tcp_bind,
        "udp_bind": cfg.network.udp_bind,
        "tls": tls_game,
        "http_port": ctx.http_addr.lock().unwrap_or_else(|e| e.into_inner()).map(|a| a.port()),
        "https_port": ctx.https_addr.lock().unwrap_or_else(|e| e.into_inner()).map(|a| a.port()),
        "config_editable": ctx.config_path.is_some(),
        "restart_mode": ctx.settings.restart,
    }))
}

/// RESTART SERVER: ring `AdminContext::restart` once this reply is out, and
/// the binary stops the process as `[admin] restart` says (a copy of
/// itself started first, or the supervisor's turn). `off` refuses.
fn server_restart(ctx: &AdminContext) -> Resp {
    let mode = ctx.settings.restart.clone();
    if mode == "off" {
        return error(
            StatusCode::CONFLICT,
            "restarting from the dashboard is switched off ([admin] restart = \"off\")",
        );
    }
    tracing::warn!("Admin asked for a server restart ({})", mode);
    let restart = std::sync::Arc::clone(&ctx.restart);
    tokio::spawn(async move {
        // Long enough for this reply to leave.
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;
        restart.notify_one();
    });
    ok(json!({ "ok": true, "mode": mode }))
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

fn mode_label(mode: GameMode) -> &'static str {
    match mode {
        GameMode::Lobby => "Lobby",
        GameMode::Sandbox => "Sandbox",
        GameMode::Countdown => "Countdown",
        GameMode::Grid => "Grid",
        GameMode::DemoLap => "Demo lap",
        GameMode::FreePractice => "Practice",
        GameMode::Replay => "Replay",
        GameMode::Qualification => "Qualifying",
        GameMode::Race => "Race",
        GameMode::Hotlap => "Hotlap",
        GameMode::Practice => "Timed practice",
    }
}

fn state_label(state: SessionState) -> &'static str {
    match state {
        SessionState::Lobby => "lobby",
        SessionState::Countdown => "countdown",
        SessionState::Racing => "racing",
        SessionState::Finished => "finished",
    }
}

fn session_elapsed_s(gs: &GameSession) -> f64 {
    let rate = gs.tick_rate_hz().max(1) as f64;
    let s = &gs.session;
    match s.race_start_tick {
        Some(start) => s.current_tick.saturating_sub(start) as f64 / rate,
        None => s.current_tick as f64 / rate,
    }
}

async fn sessions(ctx: &AdminContext) -> Resp {
    let st = ctx.state.read().await;
    let mut list: Vec<(f64, Value)> = st
        .sessions
        .values()
        .map(|gs| {
            let humans = gs
                .session
                .participants
                .keys()
                .filter(|p| !gs.ai_profiles.contains_key(p))
                .count();
            let elapsed = session_elapsed_s(gs);
            (
                elapsed,
                json!({
                    "id": gs.session.id,
                    "track": gs.track_config.name,
                    "mode": mode_label(gs.session.game_mode),
                    "state": state_label(gs.session.state),
                    "humans": humans,
                    "ai": gs.ai_profiles.len(),
                    "laps": gs.session.lap_limit,
                    "race_seconds": gs.session.race_seconds,
                    "qualifying_seconds": gs.session.qualifying_seconds,
                    "weekend": gs.session.weekend,
                    "elapsed_s": elapsed,
                    "hosted": gs.hosted,
                    "restartable": ctx.hosted.spec(gs.session.id).is_some(),
                    "awaits_advance": gs.awaits_weekend_advance(),
                }),
            )
        })
        .collect();
    // Sessions with people in them first, then by age.
    list.sort_by(|a, b| {
        let hum = |v: &Value| v["humans"].as_u64().unwrap_or(0);
        hum(&b.1).cmp(&hum(&a.1)).then(b.0.total_cmp(&a.0))
    });
    ok(json!({ "sessions": list.into_iter().map(|(_, v)| v).collect::<Vec<_>>() }))
}

/// Milliseconds as `m:ss.mmm`, or null.
fn lap_ms(ms: Option<u32>) -> Value {
    ms.filter(|m| *m > 0)
        .map(Value::from)
        .unwrap_or(Value::Null)
}

async fn session_detail(ctx: &AdminContext, id: SessionId) -> Resp {
    // A human's name is on their connection; the state's player table is
    // not filled for every route into a session.
    let names: std::collections::HashMap<PlayerId, String> = ctx
        .transport
        .read()
        .await
        .connection_snapshots()
        .await
        .into_iter()
        .map(|c| (c.player_id, c.name))
        .collect();
    let st = ctx.state.read().await;
    let Some(gs) = st.sessions.get(&id) else {
        return error(StatusCode::NOT_FOUND, "no such session");
    };
    let s = &gs.session;
    let lap_len = crate::laps::track_length_m(&gs.track_config).max(1.0);
    let in_timing_mode = matches!(
        s.game_mode,
        GameMode::Qualification | GameMode::Hotlap | GameMode::FreePractice | GameMode::Practice
    );

    // The fastest legal split of each sector among all cars, for the purple.
    let mut session_best = [None::<u32>; crate::laps::SECTOR_COUNT];
    for car in s.participants.values() {
        for (i, best) in car.laps.best_splits_ms.iter().enumerate() {
            if let Some(b) = best.filter(|b| *b > 0) {
                session_best[i] = Some(session_best[i].map_or(b, |m: u32| m.min(b)));
            }
        }
    }

    struct Row<'a> {
        id: PlayerId,
        car: &'a CarState,
        dist: f32,
        name: String,
        is_ai: bool,
    }
    let mut rows: Vec<Row> = s
        .participants
        .iter()
        .map(|(pid, car)| {
            let is_ai = gs.ai_profiles.contains_key(pid);
            let name = gs
                .ai_profiles
                .get(pid)
                .map(|p| p.name.clone())
                .or_else(|| names.get(pid).cloned())
                .or_else(|| st.players.get(pid).map(|p| p.name.clone()))
                .unwrap_or_else(|| "Driver".to_string());
            Row {
                id: *pid,
                car,
                dist: car.current_lap as f32 * lap_len + car.track_progress,
                name,
                is_ai,
            }
        })
        .collect();
    rows.sort_by(|a, b| {
        let key = |r: &Row| (r.car.in_garage, r.car.finish_position.unwrap_or(u8::MAX));
        key(a).cmp(&key(b)).then_with(|| {
            if in_timing_mode {
                let best = |r: &Row| {
                    r.car
                        .best_lap_time_ms
                        .filter(|b| *b > 0)
                        .unwrap_or(u32::MAX)
                };
                best(a).cmp(&best(b))
            } else {
                b.dist.total_cmp(&a.dist)
            }
        })
    });

    let leader_dist = rows.first().map(|r| r.dist).unwrap_or(0.0);
    let gap_of = |ahead: f32, r: &Row| -> Value {
        let behind = (ahead - r.dist).max(0.0);
        if behind >= lap_len {
            json!({ "laps": (behind / lap_len).floor() as u32 })
        } else {
            json!({ "s": behind / r.car.speed_mps.max(15.0) })
        }
    };

    let rows_json: Vec<Value> = rows
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let car = r.car;
            let flag = |split: u32, own_best: Option<u32>, sb: Option<u32>| -> &'static str {
                if split == 0 {
                    ""
                } else if sb == Some(split) {
                    "sb"
                } else if own_best == Some(split) {
                    "pb"
                } else {
                    ""
                }
            };
            let sectors: Vec<Value> = (0..crate::laps::SECTOR_COUNT)
                .map(|k| {
                    let split = car.laps.last_splits_ms[k];
                    json!({
                        "ms": (split > 0).then_some(split),
                        "flag": flag(split, car.laps.best_splits_ms[k], session_best[k]),
                    })
                })
                .collect();
            let (gap, interval) = if i == 0 || r.car.in_garage {
                (Value::Null, Value::Null)
            } else {
                (
                    gap_of(leader_dist, r),
                    gap_of(rows[i - 1].dist, r),
                )
            };
            json!({
                "pos": i + 1,
                "id": r.id,
                "num": car.grid_position,
                "name": r.name,
                "ai": r.is_ai,
                "car": st.car_configs.get(&car.car_config_id).map(|c| c.name.as_str()).unwrap_or("?"),
                "lap": car.current_lap,
                "gap": gap,
                "interval": interval,
                "sectors": sectors,
                "sector": car.laps.sector,
                "last_ms": lap_ms(car.last_lap_time_ms),
                "best_ms": lap_ms(car.best_lap_time_ms),
                "lap_ms": car.current_lap_time_ms,
                "lap_invalid": car.laps.invalid,
                "finished": car.finish_position.is_some(),
                "in_garage": car.in_garage,
                "x": car.pos_x,
                "y": car.pos_y,
                "yaw": car.yaw_rad,
                "speed_kph": car.speed_mps * 3.6,
                "gear": car.gear,
                "rpm": car.engine_rpm,
                "throttle": car.throttle_input,
                "brake": car.brake_input,
            })
        })
        .collect();

    // The outline: at most ~320 points of the centerline.
    let cl = &gs.track_config.centerline;
    let step = (cl.len() / 320).max(1);
    let outline: Vec<[f32; 2]> = cl.iter().step_by(step).map(|p| [p.x, p.y]).collect();

    let leader_lap = rows
        .iter()
        .filter(|r| !r.car.in_garage)
        .map(|r| r.car.current_lap)
        .max()
        .unwrap_or(0);
    let clock = gs.race_clock();
    ok(json!({
        "id": s.id,
        "track": gs.track_config.name,
        "track_length_m": lap_len,
        "mode": mode_label(s.game_mode),
        "state": state_label(s.state),
        "laps": s.lap_limit,
        "race_seconds": s.race_seconds,
        "qualifying_seconds": s.qualifying_seconds,
        "weekend": s.weekend,
        "pit_rule": s.pit_rule,
        "race_rules": s.race_rules,
        "time_left_s": clock.map(|c| c.left_ms as f64 / 1000.0),
        "leader_lap": leader_lap,
        "elapsed_s": session_elapsed_s(gs),
        "weather": s.conditions.weather.name(),
        "air_c": s.conditions.air_temperature_c(),
        "time_of_day_minutes": s.conditions.time_of_day_minutes,
        "hosted": gs.hosted,
        "restartable": ctx.hosted.spec(s.id).is_some(),
        "awaits_advance": gs.awaits_weekend_advance(),
        "outline": outline,
        "cars": rows_json,
    }))
}

// ---------------------------------------------------------------------------
// Players and bans
// ---------------------------------------------------------------------------

async fn players(ctx: &AdminContext) -> Resp {
    let conns = ctx.transport.read().await.connection_snapshots().await;
    let st = ctx.state.read().await;
    let mut rows: Vec<Value> = Vec::new();
    for c in &conns {
        let in_session = st
            .sessions
            .values()
            .find(|gs| gs.session.participants.contains_key(&c.player_id));
        let spectating = st.lobby.is_spectator(c.player_id).await;
        let role = match in_session {
            Some(gs) if gs.session.host_player_id == c.player_id => "HOST",
            Some(_) => "DRIVER",
            None if spectating => "SPECTATOR",
            None => "LOBBY",
        };
        let car = in_session
            .and_then(|gs| gs.session.participants.get(&c.player_id))
            .and_then(|car| st.car_configs.get(&car.car_config_id))
            .map(|c| c.name.clone())
            .or_else(|| {
                st.players
                    .get(&c.player_id)
                    .and_then(|p| p.selected_car_config_id)
                    .and_then(|id| st.car_configs.get(&id))
                    .map(|c| c.name.clone())
            });
        rows.push(json!({
            "id": c.player_id,
            "name": c.name,
            "role": role,
            "human": true,
            "car": car,
            "track": in_session.map(|gs| gs.track_config.name.clone()),
            "address": c.address.ip().to_string(),
            "online_s": c.connected_for.as_secs(),
            "last_seen_ms": c.since_heartbeat.as_millis() as u64,
            "ping_ms": c.ping_ms,
            "udp": c.udp_bound,
            "muted": st.chat.is_muted(&c.player_id),
            "driver_id": c.driver_id,
        }));
    }
    for gs in st.sessions.values() {
        for (pid, profile) in &gs.ai_profiles {
            let car = gs
                .session
                .participants
                .get(pid)
                .and_then(|car| st.car_configs.get(&car.car_config_id))
                .map(|c| c.name.clone());
            rows.push(json!({
                "id": pid,
                "name": profile.name,
                "role": "AI",
                "human": false,
                "car": car,
                "track": gs.track_config.name,
                "session": gs.session.id,
            }));
        }
    }
    ok(json!({ "players": rows, "bans": ctx.bans.entries() }))
}

async fn remove_player(ctx: &AdminContext, id: PlayerId, ban: bool, body: &Bytes) -> Resp {
    let reason = serde_json::from_slice::<Value>(body)
        .ok()
        .and_then(|v| v.get("reason").and_then(|r| r.as_str()).map(str::to_string))
        .map(|r| r.trim().chars().take(200).collect::<String>())
        .filter(|r| !r.is_empty())
        .unwrap_or_else(|| {
            if ban {
                "Banned by the server admin".to_string()
            } else {
                "Kicked by the server admin".to_string()
            }
        });
    let transport = ctx.transport.read().await;
    let Some(conn) = transport
        .connection_snapshots()
        .await
        .into_iter()
        .find(|c| c.player_id == id)
    else {
        return error(StatusCode::NOT_FOUND, "that player is not connected");
    };
    if ban {
        ctx.bans.add(BanEntry {
            name: conn.name.clone(),
            ip: conn.address.ip().to_string(),
            reason: reason.clone(),
            banned_at: unix_now(),
        });
    }
    let sent = transport.kick_player(id, &reason).await;
    tracing::info!(
        "Admin {} {} ({}): {}",
        if ban { "banned" } else { "kicked" },
        conn.name,
        conn.address.ip(),
        reason
    );
    ok(json!({ "ok": sent, "banned": ban }))
}

fn unban(ctx: &AdminContext, body: &Bytes) -> Resp {
    let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
    let name = v.get("name").and_then(|n| n.as_str()).unwrap_or("");
    let ip = v.get("ip").and_then(|n| n.as_str()).unwrap_or("");
    if ctx.bans.remove(name, ip) {
        tracing::info!("Admin lifted the ban on {} ({})", name, ip);
        ok(json!({ "ok": true }))
    } else {
        error(StatusCode::NOT_FOUND, "no such ban")
    }
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

async fn history(ctx: &AdminContext) -> Resp {
    let (dir, car_names) = {
        let st = ctx.state.read().await;
        let names: std::collections::HashMap<CarConfigId, String> = st
            .car_configs
            .iter()
            .map(|(id, c)| (*id, c.name.clone()))
            .collect();
        (st.replay.dir().to_path_buf(), names)
    };
    let rows =
        tokio::task::spawn_blocking(move || crate::stream_recorder::history(&dir, &car_names))
            .await
            .unwrap_or_default();
    ok(json!({ "sessions": rows }))
}

// ---------------------------------------------------------------------------
// Incidents
// ---------------------------------------------------------------------------

/// A report as the page takes it, with the replay of its race (`file`, the
/// recorder's `.apxs`, when it is still in the replay folder).
fn report_json(
    report: &crate::incident_report::IncidentReport,
    replay_file: Option<&str>,
) -> Value {
    let mut v = serde_json::to_value(report).unwrap_or(Value::Null);
    v["replay_file"] = json!(replay_file);
    v
}

/// Every finished race's incident list (`crate::incident_report`), newest
/// first, each matched to its replay: the same session's recording begun
/// last before the list was written.
async fn incidents(ctx: &AdminContext) -> Resp {
    let (results_dir, replay_dir) = {
        let st = ctx.state.read().await;
        (
            Path::new(&st.config.records.dir).join("results"),
            st.replay.dir().to_path_buf(),
        )
    };
    let (reports, replays) = tokio::task::spawn_blocking(move || {
        (
            crate::incident_report::list(&results_dir),
            crate::stream_recorder::history(&replay_dir, &Default::default()),
        )
    })
    .await
    .unwrap_or_default();
    let list: Vec<Value> = reports
        .iter()
        .map(|r| {
            let id = r.session_id.to_string();
            let file = replays
                .iter()
                .filter(|h| h["id"].as_str() == Some(id.as_str()))
                .filter(|h| h["recorded_at"].as_u64().unwrap_or(0) <= r.finished_at)
                .max_by_key(|h| h["recorded_at"].as_u64().unwrap_or(0))
                .and_then(|h| h["file"].as_str());
            report_json(r, file)
        })
        .collect();
    ok(json!({ "reports": list }))
}

/// A running session's incident list, and each car's penalties as they
/// stand (in the order given: the index a steward clears one by).
async fn session_incidents(ctx: &AdminContext, id: SessionId) -> Resp {
    let names: std::collections::HashMap<PlayerId, String> = ctx
        .transport
        .read()
        .await
        .connection_snapshots()
        .await
        .into_iter()
        .map(|c| (c.player_id, c.name))
        .collect();
    let st = ctx.state.read().await;
    let Some(gs) = st.sessions.get(&id) else {
        return error(StatusCode::NOT_FOUND, "no such session");
    };
    let report = crate::incident_report::IncidentReport::of(gs, &names, 0);
    let penalties: Vec<Value> = gs
        .penalties
        .iter()
        .flat_map(|(car, list)| {
            list.iter().enumerate().map(move |(index, p)| {
                json!({
                    "car": car,
                    "index": index,
                    "offence": crate::incidents::offence_name(p.offence),
                    "penalty": crate::incidents::penalty_name(p.kind),
                    "seconds": p.seconds,
                    "lap": p.lap,
                    "status": match p.status {
                        crate::race_control::PenaltyStatus::Owed => "owed",
                        crate::race_control::PenaltyStatus::Served => "served",
                        crate::race_control::PenaltyStatus::Added => "added",
                    },
                })
            })
        })
        .collect();
    let mut v = report_json(&report, None);
    v["live"] = json!(true);
    v["penalties"] = json!(penalties);
    ok(v)
}

/// A steward takes back a car's penalty (`GameSession::clear_penalty`); the
/// roster goes out again without it.
async fn clear_penalty(ctx: &AdminContext, id: SessionId, car: PlayerId, index: usize) -> Resp {
    let mut st = ctx.state.write().await;
    let Some(gs) = st.sessions.get_mut(&id) else {
        return error(StatusCode::NOT_FOUND, "no such session");
    };
    match gs.clear_penalty(&car, index) {
        Ok(p) => {
            drop(st);
            tracing::info!(
                "Admin cleared a {:?} penalty ({:?}) in session {}",
                p.kind,
                p.offence,
                id
            );
            ok(json!({ "ok": true }))
        }
        Err(why) => error(StatusCode::CONFLICT, why),
    }
}

/// A steward gives a car a penalty (`GameSession::give_penalty`): the body
/// names the `kind` ("time", "drive_through", "stop_go") and the `seconds`
/// of a time penalty or a stop-go's hold; the roster goes out again with it.
async fn give_penalty(ctx: &AdminContext, id: SessionId, car: PlayerId, body: &Bytes) -> Resp {
    use crate::race_control::PenaltyKind;
    let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
    let kind = match v.get("kind").and_then(|k| k.as_str()) {
        Some("time") => PenaltyKind::Time,
        Some("drive_through") => PenaltyKind::DriveThrough,
        Some("stop_go") => PenaltyKind::StopGo,
        _ => {
            return error(
                StatusCode::BAD_REQUEST,
                "kind: time, drive_through or stop_go",
            )
        }
    };
    let seconds = v
        .get("seconds")
        .and_then(|s| s.as_u64())
        .unwrap_or(5)
        .min(u16::MAX as u64) as u16;
    let mut st = ctx.state.write().await;
    let Some(gs) = st.sessions.get_mut(&id) else {
        return error(StatusCode::NOT_FOUND, "no such session");
    };
    match gs.give_penalty(&car, kind, seconds) {
        Ok(p) => {
            drop(st);
            tracing::info!(
                "Admin gave a {:?} penalty ({} s) in session {}",
                p.kind,
                p.seconds,
                id
            );
            ok(json!({ "ok": true }))
        }
        Err(why) => error(StatusCode::CONFLICT, why),
    }
}

/// One of the recorder's replays, to download and watch in the game (the
/// incident list's jump: `-ApexWatchReplay=<file>` and `apexsim.watch seek`).
async fn replay_download(ctx: &AdminContext, file: &str) -> Resp {
    if !crate::stream_recorder::is_served_stream(file) {
        return error(StatusCode::BAD_REQUEST, "not a replay");
    }
    let path = ctx.state.read().await.replay.dir().join(file);
    match tokio::fs::read(&path).await {
        Ok(bytes) => Response::builder()
            .status(StatusCode::OK)
            .header(header::CONTENT_TYPE, "application/octet-stream")
            .header(
                header::CONTENT_DISPOSITION,
                format!("attachment; filename=\"{file}\""),
            )
            .header(header::CACHE_CONTROL, "no-store")
            .body(Full::new(Bytes::from(bytes)))
            .expect("static response construction cannot fail"),
        Err(_) => error(StatusCode::NOT_FOUND, "no such replay"),
    }
}

// ---------------------------------------------------------------------------
// Logs
// ---------------------------------------------------------------------------

fn logs(q: &[(String, String)]) -> Resp {
    let after = param(q, "after").and_then(|a| a.parse().ok()).unwrap_or(0);
    let limit = param(q, "limit")
        .and_then(|a| a.parse().ok())
        .unwrap_or(1000usize)
        .min(5000);
    let lines = logbuf::read(
        after,
        param(q, "level").unwrap_or("ALL"),
        param(q, "q").unwrap_or(""),
        limit,
    );
    let last = lines.last().map(|l| l.seq).unwrap_or(after);
    ok(json!({ "lines": lines, "last_seq": last }))
}

fn logs_download(q: &[(String, String)]) -> Resp {
    let lines = logbuf::read(
        0,
        param(q, "level").unwrap_or("ALL"),
        param(q, "q").unwrap_or(""),
        usize::MAX,
    );
    let mut body = String::new();
    for l in lines {
        body.push_str(&format!(
            "{} {:5} {} {}\n",
            iso_time(l.ts_ms),
            l.level,
            l.source,
            l.message
        ));
    }
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
        .header(
            header::CONTENT_DISPOSITION,
            "attachment; filename=\"apexsim-server.log\"",
        )
        .header(header::CACHE_CONTROL, "no-store")
        .body(Full::new(Bytes::from(body)))
        .expect("static response construction cannot fail")
}

/// `2026-10-07T21:04:05.123Z` for a Unix time in ms.
fn iso_time(ms: u64) -> String {
    let secs = ms / 1000;
    let days = (secs / 86400) as i64;
    let rem = secs % 86400;
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        y,
        m,
        d,
        rem / 3600,
        rem % 3600 / 60,
        rem % 60,
        ms % 1000
    )
}

// ---------------------------------------------------------------------------
// Performance
// ---------------------------------------------------------------------------

async fn perf(ctx: &AdminContext) -> Resp {
    let tick_rate = ctx.state.read().await.config.server.tick_rate_hz;
    ok(json!({
        "target_hz": tick_rate,
        "budget_ms": 1000.0 / tick_rate.max(1) as f64,
        "cores": std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1),
        "overruns_total": ctx.history.overruns_total(),
        "samples": ctx.history.samples(),
    }))
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

fn config_get(ctx: &AdminContext) -> Resp {
    let Some(path) = &ctx.config_path else {
        return error(
            StatusCode::NOT_FOUND,
            "the server was started without a config file",
        );
    };
    match std::fs::read_to_string(path) {
        Ok(text) => ok(json!({
            "path": path.file_name().map(|n| n.to_string_lossy().into_owned()),
            "text": text,
            "has_backup": backup_path(path).is_file(),
            "restart_mode": ctx.settings.restart,
        })),
        Err(e) => error(
            StatusCode::INTERNAL_SERVER_ERROR,
            &format!("cannot read {}: {}", path.display(), e),
        ),
    }
}

fn backup_path(path: &Path) -> std::path::PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(".bak");
    path.with_file_name(name)
}

/// The top-level tables that differ between two config texts.
fn changed_sections(old: &str, new: &toml::Table) -> Vec<String> {
    let old: toml::Table = old.parse().unwrap_or_default();
    let mut keys: Vec<&String> = old.keys().chain(new.keys()).collect();
    keys.sort();
    keys.dedup();
    keys.into_iter()
        .filter(|k| old.get(*k) != new.get(*k))
        .cloned()
        .collect()
}

async fn config_put(ctx: &AdminContext, body: &Bytes) -> Resp {
    let Some(path) = &ctx.config_path else {
        return error(
            StatusCode::NOT_FOUND,
            "the server was started without a config file",
        );
    };
    let Some(text) = serde_json::from_slice::<Value>(body)
        .ok()
        .and_then(|v| v.get("text").and_then(|t| t.as_str()).map(str::to_string))
    else {
        return error(StatusCode::BAD_REQUEST, "expected {\"text\": \"...\"}");
    };

    // Refuse what the server would refuse to start with, before it is on disk.
    let table: toml::Table = match text.parse() {
        Ok(t) => t,
        Err(e) => return error(StatusCode::UNPROCESSABLE_ENTITY, &format!("TOML: {}", e)),
    };
    let parsed: crate::config::ServerConfig = match toml::from_str(&text) {
        Ok(c) => c,
        Err(e) => return error(StatusCode::UNPROCESSABLE_ENTITY, &format!("config: {}", e)),
    };
    if let Err(e) = parsed.validate() {
        return error(StatusCode::UNPROCESSABLE_ENTITY, &e);
    }

    let previous = std::fs::read_to_string(path).unwrap_or_default();
    let changed = changed_sections(&previous, &table);
    if !previous.is_empty() {
        if let Err(e) = std::fs::write(backup_path(path), &previous) {
            return error(
                StatusCode::INTERNAL_SERVER_ERROR,
                &format!("cannot write the backup: {}", e),
            );
        }
    }
    // Write beside and rename, so a crash never leaves half a config.
    let tmp = path.with_extension("toml.tmp");
    let written = std::fs::write(&tmp, &text).and_then(|_| std::fs::rename(&tmp, path));
    if let Err(e) = written {
        let _ = std::fs::remove_file(&tmp);
        return error(
            StatusCode::INTERNAL_SERVER_ERROR,
            &format!("cannot write {}: {}", path.display(), e),
        );
    }
    // What the save changed, key by key, against the file as it was; the
    // live ones go into force now, the rest wait for a restart.
    let old: Option<crate::config::ServerConfig> = toml::from_str(&previous).ok();
    let keys = old.map_or_else(Vec::new, |old| old.changed_keys(&parsed));
    let (mut applied, mut restart): (Vec<String>, Vec<String>) = keys
        .into_iter()
        .partition(|k| crate::config::LIVE_KEYS.contains(&k.as_str()));
    if !applied.is_empty() {
        let auth = {
            let mut st = ctx.state.write().await;
            st.config.take_live(&parsed);
            // An environment override still wins, as at startup.
            st.config.apply_env_overrides();
            st.config.auth.clone()
        };
        ctx.transport.read().await.set_auth(auth);
        logbuf::set_capacity(parsed.admin.log_buffer_lines);
        if applied.iter().any(|k| k == "logging.level") {
            match super::apply_log_level(&parsed.logging.level) {
                Some(Ok(())) => {}
                Some(Err(e)) => {
                    tracing::warn!("Log level {:?} not applied: {}", parsed.logging.level, e)
                }
                None => {
                    // RUST_LOG or --log-level sets the level for this run.
                    applied.retain(|k| k != "logging.level");
                    restart.push("logging.level".to_string());
                }
            }
        }
    }
    tracing::info!(
        "Admin saved {} (changed: {:?}; applied now: {:?}; at restart: {:?})",
        path.display(),
        changed,
        applied,
        restart
    );
    ok(json!({
        "ok": true,
        "changed": changed,
        "applied": applied,
        "restart_keys": restart,
        "restart_required": !restart.is_empty(),
    }))
}

// ---------------------------------------------------------------------------
// Hosted sessions, broadcast and schedule
// ---------------------------------------------------------------------------

async fn open_session(ctx: &AdminContext, body: &Bytes, start_now: bool) -> Resp {
    let spec: HostedSpec = match serde_json::from_slice(body) {
        Ok(s) => s,
        Err(e) => return error(StatusCode::BAD_REQUEST, &format!("session settings: {}", e)),
    };
    let id = match hosted::open(&ctx.loop_ctx, &ctx.hosted, spec).await {
        Ok(id) => id,
        Err(why) => return error(StatusCode::CONFLICT, &why),
    };
    let started = if start_now {
        match hosted::start(&ctx.loop_ctx, id).await {
            Ok(phase) => Some(phase),
            Err(why) => return error(StatusCode::CONFLICT, &why),
        }
    } else {
        None
    };
    ok(json!({ "ok": true, "id": id, "started": started }))
}

async fn session_action(ctx: &AdminContext, id: SessionId, action: &str, body: &Bytes) -> Resp {
    let lc = &ctx.loop_ctx;
    let result: Result<Value, String> = match action {
        "start" => hosted::start(lc, id)
            .await
            .map(|phase| json!({ "phase": phase })),
        "advance" => hosted::advance(lc, id)
            .await
            .map(|phase| json!({ "phase": phase })),
        "restart" => hosted::restart(lc, &ctx.hosted, id)
            .await
            .map(|new_id| json!({ "id": new_id })),
        _ => {
            let reason = serde_json::from_slice::<Value>(body)
                .ok()
                .and_then(|v| v.get("reason").and_then(|r| r.as_str()).map(str::to_string))
                .map(|r| r.trim().chars().take(200).collect::<String>())
                .filter(|r| !r.is_empty())
                .unwrap_or_else(|| "The server admin ended this session".to_string());
            hosted::stop(lc, &ctx.hosted, id, &reason)
                .await
                .map(|_| json!({}))
        }
    };
    match result {
        Ok(mut v) => {
            v["ok"] = json!(true);
            ok(v)
        }
        Err(why) => error(StatusCode::CONFLICT, &why),
    }
}

async fn broadcast(ctx: &AdminContext, body: &Bytes) -> Resp {
    let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
    let message: String = v
        .get("message")
        .and_then(|m| m.as_str())
        .unwrap_or("")
        .trim()
        .chars()
        .take(300)
        .collect();
    if message.is_empty() {
        return error(StatusCode::BAD_REQUEST, "the message is empty");
    }
    let to = match v.get("session").and_then(|s| s.as_str()) {
        Some(id) => match id.parse::<Uuid>() {
            Ok(id) => hosted::Audience::Session(id),
            Err(_) => return error(StatusCode::BAD_REQUEST, "bad session id"),
        },
        None => hosted::Audience::Everyone,
    };
    let sent = hosted::broadcast(&ctx.loop_ctx, to, &message).await;
    ok(json!({ "ok": true, "sent": sent }))
}

async fn schedule_list(ctx: &AdminContext) -> Resp {
    let (tracks, cars) = {
        let st = ctx.state.read().await;
        let tracks: std::collections::HashMap<Uuid, String> = st
            .track_configs
            .values()
            .map(|t| (t.id, t.name.clone()))
            .collect();
        let cars: std::collections::HashMap<Uuid, String> = st
            .car_configs
            .values()
            .map(|c| (c.id, c.name.clone()))
            .collect();
        (tracks, cars)
    };
    let now = super::schedule::unix_now();
    let rows: Vec<Value> = ctx
        .schedule
        .entries()
        .into_iter()
        .map(|e| {
            let status = if e.open_session.is_some() {
                "open"
            } else if e.running_session.is_some() {
                "running"
            } else if !e.enabled {
                "done"
            } else {
                "scheduled"
            };
            let mut v = serde_json::to_value(&e).unwrap_or(Value::Null);
            v["opens_at"] = json!(e.opens_at());
            v["status"] = json!(status);
            v["track"] = json!(tracks.get(&e.spec.track_id));
            v["car"] = json!(cars.get(&e.spec.car_id));
            v
        })
        .collect();
    ok(json!({ "entries": rows, "now": now }))
}

async fn schedule_put(ctx: &AdminContext, body: &Bytes) -> Resp {
    let entry: ScheduleEntry = match serde_json::from_slice(body) {
        Ok(e) => e,
        Err(e) => return error(StatusCode::BAD_REQUEST, &format!("schedule entry: {}", e)),
    };
    {
        let st = ctx.state.read().await;
        if !st.track_configs.contains_key(&entry.spec.track_id) {
            return error(StatusCode::BAD_REQUEST, "no such track");
        }
        if !st.car_configs.contains_key(&entry.spec.car_id) {
            return error(StatusCode::BAD_REQUEST, "no such car");
        }
    }
    let saved = ctx.schedule.upsert(entry);
    tracing::info!(
        "Admin scheduled {:?} at {} ({:?})",
        saved.spec.name,
        saved.start_at,
        saved.repeat
    );
    ok(json!({ "ok": true, "entry": saved }))
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// An event as the dashboard shows it: the stored event, the session open
/// for it now (if any) and that session's state.
async fn event_json(ctx: &AdminContext, event: &crate::events::Event) -> Value {
    let st = ctx.state.read().await;
    let session = st.events.session_of(&event.id);
    let live = session.and_then(|s| st.sessions.get(&s));
    let mut v = serde_json::to_value(event).unwrap_or(Value::Null);
    v["session"] = json!(session.filter(|_| live.is_some()));
    v["session_state"] = json!(live.map(|gs| state_label(gs.session.state)));
    v["session_mode"] = json!(live.map(|gs| mode_label(gs.session.game_mode)));
    v["awaits_advance"] = json!(live.is_some_and(|gs| gs.awaits_weekend_advance()));
    v["track"] = json!(st
        .track_configs
        .get(&event.spec.track_id)
        .map(|t| t.name.clone()));
    v["next_start"] = json!(event.next_start());
    // Each kept phase's replays (the session's recordings), for its links.
    if !event.results.is_empty() {
        v["replays"] = json!(crate::events::phase_replays(event, st.replay.dir()));
    }
    v["schedule"] = serde_json::to_value(&event.schedule).unwrap_or(Value::Null);
    v
}

async fn events_list(ctx: &AdminContext) -> Resp {
    let events = ctx.state.read().await.events.list();
    let mut out = Vec::with_capacity(events.len());
    for event in &events {
        out.push(event_json(ctx, event).await);
    }
    ok(json!({ "events": out }))
}

/// The weekend's sheets as one file: JSON or CSV, as they are kept beside
/// the event's file.
async fn event_sheets(ctx: &AdminContext, id: &str, file: &str) -> Resp {
    let Some(event) = ctx.state.read().await.events.get(id) else {
        return error(StatusCode::NOT_FOUND, "no such event");
    };
    let (body, kind) = if file.ends_with(".csv") {
        (crate::events::sheets_csv(&event), "text/csv; charset=utf-8")
    } else {
        (
            serde_json::to_string_pretty(&crate::events::sheets_json(&event)).unwrap_or_default(),
            "application/json",
        )
    };
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, kind)
        .header(
            header::CONTENT_DISPOSITION,
            format!("attachment; filename=\"{}.{}\"", event.id, file),
        )
        .header(header::CACHE_CONTROL, "no-store")
        .body(Full::new(Bytes::from(body)))
        .expect("static response construction cannot fail")
}

async fn event_detail(ctx: &AdminContext, id: &str) -> Resp {
    let Some(event) = ctx.state.read().await.events.get(id) else {
        return error(StatusCode::NOT_FOUND, "no such event");
    };
    ok(event_json(ctx, &event).await)
}

#[derive(serde::Deserialize)]
struct NewEvent {
    name: String,
    spec: HostedSpec,
    #[serde(default = "yes")]
    entries_open: bool,
    /// When its phases start (`events::EventSchedule`); none, the
    /// dashboard starts each.
    #[serde(default)]
    schedule: crate::events::EventSchedule,
}

fn yes() -> bool {
    true
}

async fn event_create(ctx: &AdminContext, body: &Bytes) -> Resp {
    let new: NewEvent = match serde_json::from_slice(body) {
        Ok(e) => e,
        Err(e) => return error(StatusCode::BAD_REQUEST, &format!("event: {}", e)),
    };
    if new.name.trim().is_empty() {
        return error(StatusCode::BAD_REQUEST, "the event needs a name");
    }
    let events = {
        let st = ctx.state.read().await;
        if !st.track_configs.contains_key(&new.spec.track_id) {
            return error(StatusCode::BAD_REQUEST, "no such track");
        }
        if !st.car_configs.contains_key(&new.spec.car_id) {
            return error(StatusCode::BAD_REQUEST, "no such car");
        }
        std::sync::Arc::clone(&st.events)
    };
    let mut event = events.create(&new.name, new.spec, new.entries_open, unix_now());
    if !new.schedule.is_empty() {
        let schedule = new.schedule.clamp();
        event = events
            .update(&event.id, |e| e.schedule = schedule)
            .unwrap_or(event);
    }
    tracing::info!("Admin created event {} ({})", event.id, event.name);
    ok(json!({ "ok": true, "event": event_json(ctx, &event).await }))
}

async fn event_action(ctx: &AdminContext, id: &str, action: &str, body: &Bytes) -> Resp {
    let events = std::sync::Arc::clone(&ctx.state.read().await.events);
    if events.get(id).is_none() {
        return error(StatusCode::NOT_FOUND, "no such event");
    }
    let lc = &ctx.loop_ctx;
    let session = events.session_of(id);
    let result: Result<Value, String> = match action {
        "open" => {
            events.update(id, |e| {
                e.opened = true;
                e.held = false;
                e.last_error = None;
            });
            crate::events::open_session(lc, &ctx.hosted, &events, id)
                .await
                .map(|s| json!({ "session": s }))
        }
        "start" | "advance" => match session {
            None => Err("the event has no session open".into()),
            Some(s) if action == "start" => {
                hosted::start(lc, s).await.map(|p| json!({ "phase": p }))
            }
            Some(s) => hosted::advance(lc, s).await.map(|p| json!({ "phase": p })),
        },
        "close" => {
            // Held: its schedule does not open it again until it is opened.
            events.update(id, |e| {
                e.opened = false;
                e.held = true;
            });
            match session {
                Some(s) => hosted::stop(lc, &ctx.hosted, s, "The event is closed for now")
                    .await
                    .map(|_| json!({})),
                None => Ok(json!({})),
            }
        }
        "schedule" => match serde_json::from_slice::<crate::events::EventSchedule>(body) {
            Ok(schedule) => {
                let schedule = schedule.clamp();
                events.update(id, |e| e.schedule = schedule.clone());
                Ok(json!({ "schedule": schedule }))
            }
            Err(e) => Err(format!("schedule: {e}")),
        },
        _ => {
            let open = serde_json::from_slice::<Value>(body)
                .ok()
                .and_then(|v| v.get("open").and_then(Value::as_bool));
            match open {
                Some(open) => {
                    events.update(id, |e| e.entries_open = open);
                    Ok(json!({ "entries_open": open }))
                }
                None => Err("send {\"open\": true} or false".into()),
            }
        }
    };
    match result {
        Ok(mut v) => {
            v["ok"] = json!(true);
            tracing::info!("Admin: event {} {}", id, action);
            ok(v)
        }
        Err(why) => error(StatusCode::CONFLICT, &why),
    }
}

async fn event_remove_entry(ctx: &AdminContext, id: &str, driver: Uuid) -> Resp {
    let events = std::sync::Arc::clone(&ctx.state.read().await.events);
    let mut removed = false;
    let found = events.update(id, |e| {
        let before = e.entries.len();
        e.entries.retain(|x| x.driver_id != driver);
        removed = e.entries.len() != before;
    });
    match (found, removed) {
        (None, _) => error(StatusCode::NOT_FOUND, "no such event"),
        (_, false) => error(StatusCode::NOT_FOUND, "no such entry"),
        _ => ok(json!({ "ok": true })),
    }
}

async fn event_delete(ctx: &AdminContext, id: &str) -> Resp {
    let events = std::sync::Arc::clone(&ctx.state.read().await.events);
    let session = events.session_of(id);
    if events.remove(id).is_none() {
        return error(StatusCode::NOT_FOUND, "no such event");
    }
    if let Some(s) = session {
        let _ = hosted::stop(&ctx.loop_ctx, &ctx.hosted, s, "The event was cancelled").await;
    }
    tracing::info!("Admin deleted event {}", id);
    ok(json!({ "ok": true }))
}

// ---------------------------------------------------------------------------
// Championships
// ---------------------------------------------------------------------------

/// Every championship with its rounds and standings
/// (`championships::standings`).
async fn championships_list(ctx: &AdminContext) -> Resp {
    let st = ctx.state.read().await;
    let events = st.events.list();
    let out: Vec<Value> = st
        .championships
        .list()
        .iter()
        .map(|c| {
            let (rounds, standings) = crate::championships::standings(c, &events);
            let mut v = serde_json::to_value(c).unwrap_or(Value::Null);
            v["rounds"] = json!(rounds);
            v["standings"] = json!(standings);
            v
        })
        .collect();
    ok(json!({ "championships": out }))
}

#[derive(serde::Deserialize)]
struct ChampionshipBody {
    #[serde(default)]
    name: String,
    #[serde(default)]
    events: Option<Vec<String>>,
    #[serde(default)]
    points: Option<Vec<u16>>,
    /// Points for a round's fastest lap, 0 to 5 (`Championship::fastest_lap_points`).
    #[serde(default)]
    fastest_lap_points: Option<u8>,
    /// The fastest lap scores at any place, not only a points finish.
    #[serde(default)]
    fastest_lap_any_place: Option<bool>,
}

impl ChampionshipBody {
    /// The fastest-lap rule this body asks for over `current`; none when
    /// it names neither field.
    fn fastest_lap(
        &self,
        current: crate::championships::FastestLapRule,
    ) -> Option<crate::championships::FastestLapRule> {
        if self.fastest_lap_points.is_none() && self.fastest_lap_any_place.is_none() {
            return None;
        }
        Some(crate::championships::FastestLapRule {
            points: self.fastest_lap_points.unwrap_or(current.points),
            any_place: self.fastest_lap_any_place.unwrap_or(current.any_place),
        })
    }
}

async fn championship_create(ctx: &AdminContext, body: &Bytes) -> Resp {
    let new: ChampionshipBody = match serde_json::from_slice(body) {
        Ok(c) => c,
        Err(e) => return error(StatusCode::BAD_REQUEST, &format!("championship: {}", e)),
    };
    if new.name.trim().is_empty() {
        return error(StatusCode::BAD_REQUEST, "the championship needs a name");
    }
    let store = std::sync::Arc::clone(&ctx.state.read().await.championships);
    let rule = new.fastest_lap(Default::default()).unwrap_or_default();
    let c = store.create(
        &new.name,
        new.events.unwrap_or_default(),
        new.points.unwrap_or_default(),
        rule,
        unix_now(),
    );
    tracing::info!("Admin created championship {} ({})", c.id, c.name);
    ok(json!({ "ok": true, "championship": c }))
}

async fn championship_update(ctx: &AdminContext, id: &str, body: &Bytes) -> Resp {
    let change: ChampionshipBody = match serde_json::from_slice(body) {
        Ok(c) => c,
        Err(e) => return error(StatusCode::BAD_REQUEST, &format!("championship: {}", e)),
    };
    let store = std::sync::Arc::clone(&ctx.state.read().await.championships);
    let rule = store.get(id).and_then(|c| {
        change.fastest_lap(crate::championships::FastestLapRule {
            points: c.fastest_lap_points,
            any_place: c.fastest_lap_any_place,
        })
    });
    match store.update(id, change.events, change.points, rule) {
        Some(c) => ok(json!({ "ok": true, "championship": c })),
        None => error(StatusCode::NOT_FOUND, "no such championship"),
    }
}

async fn championship_delete(ctx: &AdminContext, id: &str) -> Resp {
    let store = std::sync::Arc::clone(&ctx.state.read().await.championships);
    match store.remove(id) {
        Some(_) => {
            tracing::info!("Admin deleted championship {}", id);
            ok(json!({ "ok": true }))
        }
        None => error(StatusCode::NOT_FOUND, "no such championship"),
    }
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

async fn content_toggle(ctx: &AdminContext, body: &Bytes) -> Resp {
    let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
    let Some(id) = v
        .get("id")
        .and_then(|i| i.as_str())
        .and_then(|i| i.parse::<Uuid>().ok())
    else {
        return error(StatusCode::BAD_REQUEST, "expected an id and enabled");
    };
    let enabled = v.get("enabled").and_then(|e| e.as_bool()).unwrap_or(true);
    let mut st = ctx.state.write().await;
    let name = st
        .track_configs
        .get(&id)
        .map(|t| t.name.clone())
        .or_else(|| st.car_configs.get(&id).map(|c| c.name.clone()));
    let Some(name) = name else {
        return error(StatusCode::NOT_FOUND, "no such car or track");
    };
    if enabled {
        st.disabled_content.remove(&id);
    } else {
        st.disabled_content.insert(id);
    }
    let set = st.disabled_content.clone();
    drop(st);
    if let Err(e) = super::content::save_disabled(&ctx.settings.content_file, &set) {
        return error(
            StatusCode::INTERNAL_SERVER_ERROR,
            &format!("cannot write {}: {}", ctx.settings.content_file, e),
        );
    }
    tracing::info!(
        "Admin switched {} {}",
        name,
        if enabled { "on" } else { "off" }
    );
    ok(json!({ "ok": true, "enabled": enabled }))
}

async fn content_upload(ctx: &AdminContext, q: &[(String, String)], body: Bytes) -> Resp {
    let kind = param(q, "kind").unwrap_or("").to_string();
    let name = param(q, "name").unwrap_or("").to_string();
    if body.is_empty() {
        return error(StatusCode::BAD_REQUEST, "the file is empty");
    }
    let state = std::sync::Arc::clone(&ctx.state);
    // Parsing and writing are disk work: off the runtime, the state lock
    // taken inside only to check and add (never across a write).
    let result = tokio::task::spawn_blocking(move || match kind.as_str() {
        "car" => super::content::upload_car(&state, &body),
        "track" => super::content::upload_track_file(&state, &name, &body),
        _ => Err("kind is car or track".to_string()),
    })
    .await
    .unwrap_or_else(|_| Err("the upload failed".to_string()));
    match result {
        Ok(mut v) => {
            v["ok"] = json!(true);
            ok(v)
        }
        Err(why) => error(StatusCode::UNPROCESSABLE_ENTITY, &why),
    }
}

async fn content(ctx: &AdminContext) -> Resp {
    let st = ctx.state.read().await;
    let source_of = |path: &str| {
        if path.replace('\\', "/").contains("/custom/") {
            "CUSTOM"
        } else {
            "BUILT-IN"
        }
    };
    let mut tracks: Vec<Value> = st
        .track_configs
        .values()
        .map(|t| {
            let src = t.source_path.clone().unwrap_or_default();
            json!({
                "id": t.id,
                "name": t.name,
                "file": src,
                "source": source_of(&src),
                "length_m": crate::laps::track_length_m(t),
                "crc": format!("{:08x}", t.content_crc),
                "enabled": !st.disabled_content.contains(&t.id),
            })
        })
        .collect();
    tracks.sort_by(|a, b| a["name"].as_str().cmp(&b["name"].as_str()));
    let mut cars: Vec<Value> = st
        .car_configs
        .values()
        .map(|c| {
            json!({
                "id": c.id,
                "name": c.name,
                "class": c.class,
                "file": c.model,
                "source": source_of(&c.model),
                "crc": format!("{:08x}", c.content_crc),
                "enabled": !st.disabled_content.contains(&c.id),
            })
        })
        .collect();
    cars.sort_by(|a, b| a["name"].as_str().cmp(&b["name"].as_str()));
    let weather: Vec<Value> = Weather::ALL
        .iter()
        .map(|w| json!({ "id": *w as u8, "name": w.name() }))
        .collect();
    ok(json!({ "tracks": tracks, "cars": cars, "weather": weather }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn percent_decoding_handles_plus_escapes_and_stray_percent() {
        assert_eq!(percent_decode("a%20b+c"), "a b c");
        assert_eq!(percent_decode("100%"), "100%");
        assert_eq!(percent_decode("%zz"), "%zz");
        assert_eq!(percent_decode("%E2%9C%93"), "✓");
    }

    #[test]
    fn iso_time_formats_known_instants() {
        assert_eq!(iso_time(0), "1970-01-01T00:00:00.000Z");
        // 2026-10-07T12:34:56.789Z
        assert_eq!(iso_time(1_791_376_496_789), "2026-10-07T12:34:56.789Z");
        // A leap day.
        assert_eq!(iso_time(951_782_400_000), "2000-02-29T00:00:00.000Z");
    }

    #[test]
    fn changed_sections_lists_only_the_tables_that_differ() {
        let new: toml::Table = "[server]\ntick_rate_hz = 240\n[logging]\nlevel = \"info\"\n"
            .parse()
            .unwrap();
        let old = "[server]\ntick_rate_hz = 420\n[logging]\nlevel = \"info\"\n";
        assert_eq!(changed_sections(old, &new), vec!["server".to_string()]);
    }
}

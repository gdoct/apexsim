//! The dashboard's JSON endpoints. Everything here has already passed the
//! access-token check in `admin::route`.
//!
//! The server state is read under its read lock and copied into a
//! `serde_json::Value` before the lock is released, so a slow browser never
//! holds the game loop up.

use super::bans::BanEntry;
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
        (&Method::GET, ["sessions"]) => sessions(ctx, param(&q, "demo") == Some("1")).await,
        (&Method::GET, ["sessions", id]) => match id.parse::<Uuid>() {
            Ok(id) => session_detail(ctx, id).await,
            Err(_) => error(StatusCode::BAD_REQUEST, "bad session id"),
        },
        (&Method::GET, ["players"]) => players(ctx).await,
        (&Method::POST, ["players", id, action @ ("kick" | "ban")]) => match id.parse::<Uuid>() {
            Ok(id) => remove_player(ctx, id, *action == "ban", &body).await,
            Err(_) => error(StatusCode::BAD_REQUEST, "bad player id"),
        },
        (&Method::GET, ["bans"]) => ok(json!({ "bans": ctx.bans.entries() })),
        (&Method::POST, ["bans", "remove"]) => unban(ctx, &body),
        (&Method::GET, ["history"]) => history(ctx).await,
        (&Method::GET, ["logs"]) => logs(&q),
        (&Method::GET, ["logs", "download"]) => logs_download(&q),
        (&Method::GET, ["perf"]) => perf(ctx).await,
        (&Method::GET, ["config"]) => config_get(ctx),
        (&Method::PUT, ["config"]) => config_put(ctx, &body),
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
        let sessions = st
            .sessions
            .values()
            .filter(|s| !s.session.session_kind.is_watch_only())
            .count();
        let humans = st
            .sessions
            .values()
            .filter(|s| !s.session.session_kind.is_watch_only())
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
    }))
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

fn mode_label(mode: GameMode) -> &'static str {
    match mode {
        GameMode::Lobby => "Lobby",
        GameMode::Sandbox => "Sandbox",
        GameMode::Countdown => "Countdown",
        GameMode::DemoLap => "Demo lap",
        GameMode::FreePractice => "Practice",
        GameMode::Replay => "Replay",
        GameMode::Qualification => "Qualifying",
        GameMode::Race => "Race",
        GameMode::Hotlap => "Hotlap",
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

async fn sessions(ctx: &AdminContext, with_demo: bool) -> Resp {
    let st = ctx.state.read().await;
    let mut list: Vec<(f64, Value)> = st
        .sessions
        .values()
        .filter(|s| with_demo || !s.session.session_kind.is_watch_only())
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
                    "elapsed_s": elapsed,
                    "demo": gs.session.session_kind.is_watch_only(),
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
        GameMode::Qualification | GameMode::Hotlap | GameMode::FreePractice
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
        let key = |r: &Row| {
            (
                r.car.in_garage,
                r.car.finish_position.map_or(u8::MAX, |p| p),
            )
        };
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
        "time_left_s": clock.map(|c| c.left_ms as f64 / 1000.0),
        "leader_lap": leader_lap,
        "elapsed_s": session_elapsed_s(gs),
        "weather": s.conditions.weather.name(),
        "air_c": s.conditions.air_temperature_c(),
        "time_of_day_minutes": s.conditions.time_of_day_minutes,
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
            "udp": c.udp_bound,
        }));
    }
    for gs in st
        .sessions
        .values()
        .filter(|gs| !gs.session.session_kind.is_watch_only())
    {
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
    let rows = tokio::task::spawn_blocking(move || read_history(&dir, &car_names))
        .await
        .unwrap_or_default();
    ok(json!({ "sessions": rows }))
}

fn read_history(
    dir: &Path,
    car_names: &std::collections::HashMap<CarConfigId, String>,
) -> Vec<Value> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut files: Vec<(SystemTime, std::path::PathBuf)> = entries
        .flatten()
        .filter(|e| {
            let n = e.file_name();
            let n = n.to_string_lossy();
            n.starts_with("replay_") && n.ends_with(".bin")
        })
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    files.sort_by_key(|f| std::cmp::Reverse(f.0));
    files.truncate(200);

    let mut out = Vec::new();
    for (_, path) in files {
        let Ok(header) = crate::replay::read_replay_header(&path) else {
            continue;
        };
        let m = header.metadata;
        let mut results: Vec<_> = m.participants.iter().collect();
        results.sort_by_key(|p| p.finish_position.unwrap_or(u8::MAX));
        let winner = results
            .iter()
            .find(|p| p.finish_position == Some(1))
            .map(|p| p.player_name.clone());
        out.push(json!({
            "id": m.session_id,
            "recorded_at": m.recorded_at,
            "track": m.track_name,
            "kind": if m.race_start_tick.is_some() { "Race" } else { "Session" },
            "duration_s": m.duration_ticks as f64 / m.tick_rate.max(1) as f64,
            "cars": m.participants.len(),
            "humans": m.participants.iter().filter(|p| !p.is_ai).count(),
            "winner": winner,
            "results": results.iter().map(|p| json!({
                "pos": p.finish_position,
                "name": p.player_name,
                "ai": p.is_ai,
                "car": car_names.get(&p.car_config_id),
            })).collect::<Vec<_>>(),
        }));
    }
    out.sort_by_key(|v| std::cmp::Reverse(v["recorded_at"].as_u64().unwrap_or(0)));
    out
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

fn config_put(ctx: &AdminContext, body: &Bytes) -> Resp {
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
    tracing::info!("Admin saved {} (changed: {:?})", path.display(), changed);
    ok(json!({ "ok": true, "changed": changed, "restart_required": !changed.is_empty() }))
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

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
            })
        })
        .collect();
    cars.sort_by(|a, b| a["name"].as_str().cmp(&b["name"].as_str()));
    ok(json!({ "tracks": tracks, "cars": cars }))
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

//! Session ticking: advance every session by one fixed step (with a panic
//! boundary per session), drive replay recording, and prepare the telemetry
//! payloads for the broadcast phase. Runs entirely under one state write
//! lock; no transport lock is touched here.

use super::{GameLoopCtx, HeldInput};
use crate::data::*;
use crate::network::ServerMessage;
use crate::session_wire::SessionWire;
use bytes::Bytes;
use std::collections::HashMap;
use tracing::{debug, error, info, warn};

/// Telemetry for one session, serialized exactly once per tick. The broadcast
/// phase fans out cheap `Bytes` clones to every recipient.
pub(crate) struct SessionTelemetry {
    pub session_id: SessionId,
    /// The `ServerMessage::TelemetryCompact` pre-encoded with the positional
    /// `rmp_serde::to_vec` encoding (protocol v2).
    pub data: Bytes,
    /// Non-AI participants at tick time; broadcast still verifies lobby
    /// membership and connection liveness before sending.
    pub player_recipients: Vec<PlayerId>,
    pub participant_count: usize,
    pub session_state: SessionState,
    /// Each human driver's own `DriverFeedback`, pre-encoded (positional).
    /// Unlike telemetry these go to one recipient each.
    pub driver_feedback: Vec<(PlayerId, Bytes)>,
}

/// One car's timing-line crossing, ready to broadcast, with the record
/// submission it earned (a legal personal best set by a human driver).
pub(crate) struct LapTimingOut {
    pub session_id: SessionId,
    pub player_recipients: Vec<PlayerId>,
    pub msg: ServerMessage,
    /// Present only for a lap a human driver may have set a record with.
    pub record: Option<RecordSubmission>,
    /// The session's qualifying classification, when this lap changed it.
    pub qualifying: Option<crate::records::QualifyingResult>,
}

/// A lap offered to the record store, once the state lock is released.
pub(crate) struct RecordSubmission {
    pub player_id: PlayerId,
    pub player_name: String,
    pub track_id: TrackConfigId,
    pub car_config_id: CarConfigId,
    pub lap_time_ms: u32,
    pub splits_ms: [u32; crate::laps::SECTOR_COUNT],
    pub ghost: Option<crate::records::GhostLap>,
}

/// A `SessionRoster` message to deliver reliably over TCP.
pub(crate) struct SessionRosterOut {
    pub session_id: SessionId,
    pub player_recipients: Vec<PlayerId>,
    pub msg: ServerMessage,
}

/// Everything the broadcast phase needs from one tick of all sessions.
pub(crate) struct TickOutput {
    pub telemetry: Vec<SessionTelemetry>,
    pub rosters: Vec<SessionRosterOut>,
    pub lap_timing: Vec<LapTimingOut>,
    /// Round-robin slices of each racing session's road state, reliable
    /// over TCP to its drivers and spectators (`SessionRosterOut` is reused
    /// as the generic reliable per-session delivery).
    pub road: Vec<SessionRosterOut>,
    /// The records of the sessions watched as streams this tick, for their
    /// viewers (`showcase::fan_out`).
    pub streams: Vec<crate::showcase::ChannelOutput>,
}

/// Advance all sessions one tick and return the payloads to broadcast.
/// Telemetry is only serialized every `telemetry_divisor` ticks (60 Hz
/// snapshots of a 420 Hz sim by default); the replay records on those ticks.
pub(crate) async fn tick_sessions(
    ctx: &GameLoopCtx,
    player_inputs: &HashMap<PlayerId, HeldInput>,
    tick_count: u64,
) -> TickOutput {
    let tick_rate = ctx.tick_rate;
    let telemetry_tick = tick_count.is_multiple_of(ctx.telemetry_divisor);
    let mut state_write = ctx.state.write().await;
    // Split the borrow so `sessions` can be iterated mutably while the
    // lobby (a disjoint field) is queried for player names.
    let state = &mut *state_write;

    // Player names for rosters, replay metadata and lap records, fetched
    // from the lobby the first time a session needs them this tick. On
    // demand and *after* each session has ticked: a lap event is raised
    // inside the tick and drained right after it, so a fetch decided before
    // the tick never saw one, and every record went to the store under an
    // empty name — which it refuses.
    let mut player_names: Option<HashMap<PlayerId, String>> = None;
    let empty_names: HashMap<PlayerId, String> = HashMap::new();
    let lobby = &state.lobby;
    let reconnect_grace_seconds = state.config.server.reconnect_grace_seconds as u64;

    // Replays finished this tick, written out once the loop is done.
    let mut replay_stops = Vec::new();
    // Races finished this tick: their classification (`crate::results`).
    let mut race_results: Vec<crate::results::RaceResult> = Vec::new();
    let results_dir = state
        .config
        .records
        .results
        .then(|| std::path::Path::new(&state.config.records.dir).join("results"));

    // Collect sessions to remove (empty or finished)
    let mut sessions_to_remove = Vec::new();

    let mut telemetry_out = Vec::new();
    let mut rosters_out = Vec::new();
    let mut lap_timing_out: Vec<LapTimingOut> = Vec::new();
    let mut road_out: Vec<SessionRosterOut> = Vec::new();
    let mut stream_out: Vec<crate::showcase::ChannelOutput> = Vec::new();
    // Every session sends the next slice of its road on the same beat.
    let road_send_ticks =
        ((crate::road_state::ROAD_SEND_SECONDS * tick_rate as f32).round() as u64).max(1);
    let road_send = tick_count.is_multiple_of(road_send_ticks);
    // Sessions whose state moved since the lobby last heard (a tick, or a
    // handler between ticks), so the browser lists what each is doing.
    let mut state_changes: Vec<(SessionId, SessionState, bool, SessionConditions)> = Vec::new();

    // Tick each session
    for (session_id, game_session) in state.sessions.iter_mut() {
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

        // A hosted session is the server's: it runs (or waits in its lobby)
        // with nobody in it, until the dashboard ends it.
        let is_hosted = game_session.hosted;

        if real_player_count == 0 && !is_demo_lap_with_ai && !is_hosted {
            debug!(
                "Session {} has no real players, marking for removal",
                session_id
            );
            sessions_to_remove.push(*session_id);
            continue;
        }

        // Every human here has lost their connection: their seats are held
        // for the grace period, then the session ends.
        let orphaned = !is_hosted && !game_session.has_connected_driver();
        if finished_session_expired(
            &mut game_session.orphaned_since_loop_tick,
            orphaned,
            tick_count,
            tick_rate as u64,
            reconnect_grace_seconds,
        ) {
            info!(
                "Session {} ended: no driver came back within {} s",
                session_id, reconnect_grace_seconds
            );
            sessions_to_remove.push(*session_id);
            continue;
        }

        // Build this session's input map: human inputs are looked up by key
        // (no clone of the global input map), AI inputs are generated.
        let mut session_inputs: HashMap<PlayerId, PlayerInputData> =
            HashMap::with_capacity(game_session.session.participants.len());
        for player_id in game_session.session.participants.keys() {
            // The debug stand-in (`crate::debug_hooks`, off by default)
            // drives a human's car in place of its input.
            // An away driver's car is the server's until they rejoin.
            let server_input = game_session.stand_in_input(player_id).or_else(|| {
                game_session
                    .is_away(player_id)
                    .then(|| game_session.away_input(player_id))
            });
            let input = match server_input {
                Some(input) => {
                    // A client that numbers its inputs still gets its car
                    // while the server drives it (the stand-in): no number
                    // is applied, which tells it so.
                    if player_inputs
                        .get(player_id)
                        .is_some_and(|h| h.seq.is_some())
                    {
                        game_session.own_car_feeds.entry(*player_id).or_default();
                    }
                    input
                }
                None => match player_inputs.get(player_id) {
                    Some(held) => {
                        // The driver's own input drives this tick: its
                        // number goes back with the car's snapshot. The
                        // sim never sees it.
                        if let Some(seq) = held.seq {
                            game_session
                                .own_car_feeds
                                .entry(*player_id)
                                .or_default()
                                .applied_seq = Some(seq);
                        }
                        held.input
                    }
                    // No human input, generate AI input
                    None => game_session.generate_ai_input(player_id),
                },
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
        if let Some(listed) = game_session.take_state_change() {
            state_changes.push((
                *session_id,
                listed,
                game_session.awaits_weekend_advance(),
                game_session.session.conditions,
            ));
        }

        // A weekend's phase begins between ticks (`AdvanceWeekend`), already
        // racing by the time the tick sees it: each phase is its own replay.
        let phase_started = game_session.take_phase_started();
        let replay_starting = (prev_state != SessionState::Racing
            && new_state == SessionState::Racing)
            || (phase_started && new_state == SessionState::Racing);
        let replay_stopping =
            prev_state == SessionState::Racing && new_state == SessionState::Finished;
        let stream_wanted =
            state.live.has_viewers(session_id) && !state.streams.contains_key(session_id);
        if player_names.is_none()
            && (replay_starting
                || replay_stopping
                || stream_wanted
                || game_session.roster_is_dirty()
                || game_session.has_lap_events())
        {
            player_names = Some(
                lobby
                    .get_lobby_players()
                    .await
                    .into_iter()
                    .map(|p| (p.id, p.name))
                    .collect(),
            );
        }
        let player_names = player_names.as_ref().unwrap_or(&empty_names);

        // Start a replay recording. A demo is a menu backdrop, and every
        // client idling in the menu would otherwise write a replay.
        if replay_starting {
            let participants = replay_participants(game_session, player_names);
            // A session already watched has its stream; the recording joins it.
            let stream = state.streams.entry(*session_id).or_insert_with(|| {
                crate::session_stream::SessionStream::new(
                    game_session,
                    REPLAY_FRAME_HZ,
                    player_names,
                )
            });
            let track = &game_session.track_config;
            let metadata = crate::replay::ReplayMetadata {
                session_id: *session_id,
                track_config_id: track.id,
                track_name: track.name.clone(),
                recorded_at: std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |d| d.as_secs()),
                duration_ticks: 0,
                tick_rate,
                participants,
                conditions: game_session.session.conditions,
                race_start_tick: None,
                track_stem: track
                    .source_path
                    .as_deref()
                    .and_then(|p| std::path::Path::new(p).file_stem())
                    .map(|s| s.to_string_lossy().into_owned()),
                track_length_m: game_session.track_length_m(),
            };
            state.replay.start(game_session, stream, metadata);
            debug!("Started replay recording for session {}", session_id);
        }

        // Roster: membership changed → announce the car-index mapping used
        // by compact telemetry (reliable, TCP).
        let should_broadcast = matches!(
            new_state,
            SessionState::Countdown | SessionState::Racing | SessionState::Finished
        );
        // A held seat's driver is sent nothing until `JoinSession` resumes
        // it: a resumed connection sits in the menu with the rejoin banner.
        let player_recipients: Vec<PlayerId> = game_session
            .session
            .participants
            .keys()
            .filter(|pid| {
                !game_session.session.ai_player_ids.contains(pid) && !game_session.is_away(pid)
            })
            .cloned()
            .collect();
        // Timing lines crossed this tick. Reliable.
        let lap_events = game_session.take_lap_events();
        // A weekend day under its own sky: every human's line at once.
        let line_refresh: Vec<PlayerId> = if game_session.take_day_lines_due() {
            player_recipients.clone()
        } else {
            lap_events
                .iter()
                .filter(|e| {
                    e.event.lap_time_ms.is_some() && player_recipients.contains(&e.player_id)
                })
                .map(|e| e.player_id)
                .collect()
        };
        let pit_starts = game_session.take_pit_events();
        // The spectator stream of a session being recorded: this tick's
        // events, frame, slow state and road.
        if stream_wanted {
            state.streams.insert(
                *session_id,
                crate::session_stream::SessionStream::new(
                    game_session,
                    REPLAY_FRAME_HZ,
                    player_names,
                ),
            );
        }
        if let Some(stream) = state.streams.get_mut(session_id) {
            let roster_changed = game_session.roster_is_dirty();
            let out = stream.tick(
                game_session,
                &lap_events,
                &pit_starts,
                player_names,
                roster_changed,
            );
            state.replay.record(session_id, &out.records);
            if state.live.has_viewers(session_id) {
                let mut channel = crate::showcase::ChannelOutput {
                    viewers: state.live.viewers(session_id),
                    ..Default::default()
                };
                for record in out.records {
                    let body = Bytes::from(record.body);
                    if record.kind == crate::spectator::RECORD_FRAME {
                        channel.frames.push(body);
                    } else {
                        channel.reliable.push(body);
                    }
                }
                let newcomers = state.live.take_newcomers(session_id);
                if !newcomers.is_empty() {
                    let catch_up: Vec<Bytes> = stream
                        .catch_up(game_session)
                        .into_iter()
                        .map(Bytes::from)
                        .collect();
                    for player in newcomers {
                        channel.catch_up.push((player, catch_up.clone()));
                    }
                }
                stream_out.push(channel);
            } else if !state.replay.is_recording(session_id) {
                // Nobody watches and nothing records: the stream stops.
                state.streams.remove(session_id);
            }
        }
        if !lap_events.is_empty() {
            let track_id = game_session.track_config.id;
            let events = lap_events;
            for out in events {
                let Some(car_index) = game_session.car_index_of(&out.player_id) else {
                    continue;
                };
                let is_ai = game_session.session.ai_player_ids.contains(&out.player_id);
                // A lap the server drove for an away driver is not theirs.
                let recordable = !is_ai && !game_session.is_away(&out.player_id);
                let mut flags = 0u8;
                if out.event.personal_best_lap {
                    flags |= crate::network::LapTimingData::FLAG_PERSONAL_BEST_LAP;
                }
                if out.session_best_lap {
                    flags |= crate::network::LapTimingData::FLAG_SESSION_BEST_LAP;
                }
                if out.event.personal_best_sector {
                    flags |= crate::network::LapTimingData::FLAG_PERSONAL_BEST_SECTOR;
                }
                if out.session_best_sector {
                    flags |= crate::network::LapTimingData::FLAG_SESSION_BEST_SECTOR;
                }
                let lap_time_ms = out.event.lap_time_ms.unwrap_or(0);
                let qualifying = match out.event.lap_time_ms {
                    Some(ms) if out.event.valid => {
                        game_session.note_qualifying_lap(out.player_id, ms)
                    }
                    _ => None,
                };
                let record = match (out.event.lap_time_ms, recordable) {
                    (Some(lap_time_ms), true) if out.event.valid => game_session
                        .session
                        .participants
                        .get(&out.player_id)
                        .map(|state| RecordSubmission {
                            player_id: out.player_id,
                            player_name: player_names
                                .get(&out.player_id)
                                .cloned()
                                .unwrap_or_default(),
                            track_id,
                            car_config_id: state.car_config_id,
                            lap_time_ms,
                            splits_ms: out.event.splits_ms,
                            ghost: out.ghost,
                        }),
                    _ => None,
                };
                lap_timing_out.push(LapTimingOut {
                    session_id: *session_id,
                    player_recipients: player_recipients.clone(),
                    msg: ServerMessage::LapTiming(crate::network::LapTimingData {
                        car_index,
                        lap: out.event.lap,
                        sector: out.event.sector,
                        sector_time_ms: out.event.sector_time_ms,
                        lap_time_ms,
                        is_lap_end: out.event.lap_time_ms.is_some(),
                        valid: out.event.valid,
                        flags,
                    }),
                    record,
                    qualifying,
                });
            }
        }

        // A human's racing line aid, planned again at the end of each lap
        // for their car as it is now (setup, fuel, tyres, the road), when
        // that has moved since the line they have.
        for player_id in &line_refresh {
            if let Some(profile) = game_session.refreshed_racing_line(player_id) {
                lap_timing_out.push(LapTimingOut {
                    session_id: *session_id,
                    player_recipients: vec![*player_id],
                    msg: ServerMessage::RacingLine(crate::network::RacingLineData::from_profile(
                        *session_id,
                        &profile,
                    )),
                    record: None,
                    qualifying: None,
                });
            }
        }

        // Pit stops the crews started this tick: what each one does, for
        // the pit-stop panel. Reliable, like the timing.
        for (player_id, service) in pit_starts {
            let Some(car_index) = game_session.car_index_of(&player_id) else {
                continue;
            };
            lap_timing_out.push(LapTimingOut {
                session_id: *session_id,
                player_recipients: player_recipients.clone(),
                msg: ServerMessage::PitService(crate::session_wire::pit_service_data(
                    car_index, &service,
                )),
                record: None,
                qualifying: None,
            });
        }

        // Each driver's own box, once it is dealt: what a driver who drives
        // the lane aims for, and the camera's place at the stop.
        for (player_id, pit_box) in game_session.take_pit_box_news() {
            lap_timing_out.push(LapTimingOut {
                session_id: *session_id,
                player_recipients: vec![player_id],
                msg: ServerMessage::PitBox((&pit_box).into()),
                record: None,
                qualifying: None,
            });
        }

        if game_session.take_roster_dirty() {
            let roster = game_session.build_roster(player_names);
            rosters_out.push(SessionRosterOut {
                session_id: *session_id,
                player_recipients: player_recipients.clone(),
                msg: ServerMessage::SessionRoster(roster),
            });
        }

        // A race weekend: a round that finished this tick is ranked now that
        // its last laps are classified, and the weekend's state goes to the
        // whole session whenever it changed (after the roster, whose car
        // indices it uses).
        game_session.settle_weekend();
        if let Some(weekend) = game_session.take_weekend_state() {
            rosters_out.push(SessionRosterOut {
                session_id: *session_id,
                player_recipients: player_recipients.clone(),
                msg: ServerMessage::WeekendState((&weekend).into()),
            });
        }
        if let Some(shootout) = game_session.take_shootout_state() {
            rosters_out.push(SessionRosterOut {
                session_id: *session_id,
                player_recipients: player_recipients.clone(),
                msg: ServerMessage::ShootoutState((&shootout).into()),
            });
        }

        // Road state: the next slice round the lap, while the session is
        // live. Round-robin by the global tick, so the whole lap refreshes
        // over several sends and there is no per-session cursor to keep.
        if road_send && should_broadcast {
            if let Some(n) = game_session
                .track_config
                .road_state
                .as_ref()
                .map(|r| r.cell_count())
                .filter(|n| *n > 0)
            {
                let send_index = (tick_count / road_send_ticks) as usize;
                let first = (send_index * crate::road_state::ROAD_CELLS_PER_SEND) % n;
                if let Some(msg) = super::dispatch::road_state_message(
                    game_session,
                    *session_id,
                    first,
                    crate::road_state::ROAD_CELLS_PER_SEND,
                ) {
                    road_out.push(SessionRosterOut {
                        session_id: *session_id,
                        player_recipients: player_recipients.clone(),
                        msg,
                    });
                }
            }
        }

        // Telemetry: the broadcast payload uses the compact positional
        // encoding and is only produced on divisor ticks, as is the replay
        // frame (full telemetry), captured further down.
        // Driver feedback is drained on every telemetry tick, broadcasting
        // or not, so a message never carries more than one interval (a car
        // that drove while nothing was sent would otherwise open with stale
        // samples).
        let mut driver_feedback = Vec::new();
        if telemetry_tick {
            let server_tick = game_session.session.current_tick;
            let own_state_period_messages =
                crate::own_car::state_period_messages(tick_rate, ctx.telemetry_divisor);
            // A driver who left takes their feed with them.
            let participants = &game_session.session.participants;
            game_session
                .own_car_feeds
                .retain(|pid, _| participants.contains_key(pid));
            // An away driver's feedback is drained too, so a resumed seat
            // does not open with what the server's driving left behind.
            let humans: Vec<PlayerId> = game_session
                .session
                .participants
                .keys()
                .filter(|pid| !game_session.session.ai_player_ids.contains(pid))
                .cloned()
                .collect();
            for player_id in &humans {
                let away = game_session.is_away(player_id);
                let Some(car) = game_session.session.participants.get_mut(player_id) else {
                    continue;
                };
                let mut feedback = car.feedback.take(server_tick);
                if !should_broadcast || away {
                    continue;
                }
                // The pace race control holds the car to, for the dash.
                feedback.speed_cap_mps = car.speed_cap_mps;
                // A driver whose client numbers its inputs predicts its own
                // car: its feedback carries the car's snapshot and, in
                // turn, a slice of the full state (`crate::own_car`).
                if let Some(feed) = game_session.own_car_feeds.get_mut(player_id) {
                    let (own_car, own_state) =
                        feed.next(car, server_tick, own_state_period_messages);
                    feedback.own_car = Some(Box::new(own_car));
                    feedback.own_state = own_state;
                }
                match rmp_serde::to_vec(&ServerMessage::DriverFeedback(feedback)) {
                    Ok(data) => driver_feedback.push((*player_id, Bytes::from(data))),
                    Err(e) => error!("Failed to serialize driver feedback: {}", e),
                }
            }
        }

        if should_broadcast && telemetry_tick {
            let compact = game_session.get_compact_telemetry();
            match rmp_serde::to_vec(&ServerMessage::TelemetryCompact(compact)) {
                Ok(data) => {
                    telemetry_out.push(SessionTelemetry {
                        session_id: *session_id,
                        data: Bytes::from(data),
                        player_recipients,
                        participant_count: game_session.session.participants.len(),
                        session_state: new_state,
                        driver_feedback,
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

        // A race that finished this tick leaves its classification.
        if results_dir.is_some()
            && game_session.session.game_mode == GameMode::Race
            && prev_state != SessionState::Finished
            && new_state == SessionState::Finished
        {
            let track = &game_session.track_config;
            race_results.push(crate::results::RaceResult {
                session_id: session_id.to_string(),
                track_name: track.name.clone(),
                track_stem: track
                    .source_path
                    .as_deref()
                    .and_then(|p| std::path::Path::new(p).file_stem())
                    .map(|s| s.to_string_lossy().into_owned()),
                finished_at: std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |d| d.as_secs()),
                lap_limit: game_session
                    .session
                    .race_seconds
                    .is_none()
                    .then_some(game_session.session.lap_limit as u16),
                race_seconds: game_session.session.race_seconds,
                cars: game_session.classification(player_names),
            });
        }

        // The replay is complete when the session finishes.
        if replay_stopping {
            replay_stops.push((
                *session_id,
                game_session.session.race_start_tick,
                game_session.session.game_mode,
                replay_participants(game_session, player_names),
            ));
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

    if let Some(dir) = results_dir {
        let records = &state.config.records;
        for result in race_results {
            if !records.webhook_url.is_empty() {
                let url = records.webhook_url.clone();
                let body = crate::webhook::body(&result, records.webhook_format);
                let session_id = result.session_id.clone();
                tokio::spawn(async move {
                    match crate::webhook::post(&url, body).await {
                        Ok(status) if (200..300).contains(&status) => {
                            info!("Race result for session {} posted ({})", session_id, status)
                        }
                        Ok(status) => warn!(
                            "The results webhook answered {} for session {}",
                            status, session_id
                        ),
                        Err(e) => warn!("Failed to post the race result of {}: {}", session_id, e),
                    }
                });
            }
            let dir = dir.clone();
            tokio::task::spawn_blocking(move || match result.write(&dir) {
                Ok(path) => info!(
                    "Race result for session {} written to {:?}",
                    result.session_id, path
                ),
                Err(e) => warn!(
                    "Failed to write the race result of {}: {}",
                    result.session_id, e
                ),
            });
        }
    }

    for (session_id, race_start_tick, game_mode, results) in replay_stops {
        state.streams.remove(&session_id);
        let Some(recording) = state.replay.take(&session_id) else {
            warn!("No active recording for session {}", session_id);
            continue;
        };
        let dir = state.replay.dir().to_path_buf();
        // Written off the loop: a long race is a long file to assemble.
        tokio::task::spawn_blocking(move || {
            match recording.finish(&dir, results, race_start_tick, game_mode) {
                Ok(path) => debug!("Replay saved for session {} to {:?}", session_id, path),
                Err(e) => warn!("Failed to save replay for session {}: {}", session_id, e),
            }
        });
    }

    for (session_id, session_state, weekend_waiting, conditions) in state_changes {
        state
            .lobby
            .set_session_state(session_id, session_state, weekend_waiting, conditions)
            .await;
    }

    // Remove empty sessions from the game state and lobby
    for session_id in sessions_to_remove {
        state.sessions.remove(&session_id);
        state.lobby.unregister_session(session_id).await;
        debug!("Removed empty session {}", session_id);
    }

    // Periodic cleanup of orphaned lobby sessions (every 5 seconds)
    if tick_count.is_multiple_of(tick_rate as u64 * 5) {
        let lobby_session_ids: Vec<SessionId> = state.lobby.get_all_session_ids().await;
        for session_id in lobby_session_ids {
            if !state.sessions.contains_key(&session_id) {
                debug!("Cleaning up orphaned lobby session {}", session_id);
                state.lobby.unregister_session(session_id).await;
            }
        }
        // A session removed mid-race never finishes its recording.
        let sessions = &state.sessions;
        state.replay.discard_except(|id| sessions.contains_key(id));
        state.streams.retain(|id, _| sessions.contains_key(id));
        for gone in state.live.watched_sessions() {
            if !sessions.contains_key(&gone) {
                state.live.forget_session(&gone);
            }
        }
    }

    TickOutput {
        telemetry: telemetry_out,
        rosters: rosters_out,
        lap_timing: lap_timing_out,
        road: road_out,
        streams: stream_out,
    }
}

/// Remove finished sessions older than the configured timeout and refresh
/// the once-per-second gauges. Runs after the telemetry broadcast so a
/// finished session still gets its final telemetry.
/// Whether a session finished more than `timeout_seconds` ago, remembering
/// in `finished_since` the loop tick it was first seen finished (and
/// forgetting it when it is running again: "drive again"). The age used to
/// be the loop's tick less the session's own, which is how long the server
/// had been up before the session was created: on a server up for more than
/// the timeout a race was removed the tick it finished, before most clients
/// had a frame saying so (a watched backdrop race froze at the flag).
pub(crate) fn finished_session_expired(
    finished_since: &mut Option<u64>,
    finished: bool,
    tick_count: u64,
    tick_rate: u64,
    timeout_seconds: u64,
) -> bool {
    if !finished {
        *finished_since = None;
        return false;
    }
    let since = *finished_since.get_or_insert(tick_count);
    tick_count.saturating_sub(since) / tick_rate.max(1) > timeout_seconds
}

pub(crate) async fn cleanup_finished_sessions(ctx: &GameLoopCtx, tick_count: u64) {
    let tick_rate = ctx.tick_rate;
    let mut state_write = ctx.state.write().await;

    // Cleanup finished sessions (older than timeout)
    let timeout_seconds = state_write.config.server.session_timeout_seconds as u64;
    state_write.sessions.retain(|id, session| {
        // A weekend waiting for its host between phases stays while anyone
        // is there to advance it; the reconnect grace ends it when all are
        // away.
        // A hosted weekend waits for the dashboard (or its schedule) instead.
        let waiting =
            session.awaits_weekend_advance() && (session.hosted || session.has_connected_driver());
        let expired = finished_session_expired(
            &mut session.finished_since_loop_tick,
            session.session.state == SessionState::Finished && !waiting,
            tick_count,
            tick_rate as u64,
            timeout_seconds,
        );
        if expired {
            debug!("Removing finished session: {}", id);
        }
        !expired
    });

    // Refresh gauges once per second
    if tick_count.is_multiple_of(tick_rate as u64) {
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

/// Frames a second a server replay is recorded at: the showcases' rate.
const REPLAY_FRAME_HZ: u16 = 30;

/// A session's cars as a replay lists them, with the places they have been
/// classified in so far.
fn replay_participants(
    session: &crate::game_session::GameSession,
    names: &HashMap<PlayerId, String>,
) -> Vec<crate::replay::ReplayParticipant> {
    session
        .session
        .participants
        .iter()
        .map(|(pid, car)| crate::replay::ReplayParticipant {
            player_id: *pid,
            player_name: session
                .get_ai_profile(pid)
                .map(|p| p.name.clone())
                .or_else(|| names.get(pid).cloned())
                .unwrap_or_else(|| format!("Player-{}", &pid.to_string()[..8])),
            car_config_id: car.car_config_id,
            finish_position: car.finish_position,
            is_ai: session.is_ai_player(pid),
            livery: 0,
            best_lap_ms: car.best_lap_time_ms.filter(|ms| *ms > 0),
            laps: session.laps_completed(pid),
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::finished_session_expired;

    #[test]
    fn a_finished_session_is_kept_for_the_timeout_from_when_it_finished() {
        let rate = 420;
        let mut since = None;
        // Running: never expired, whatever the server's uptime.
        assert!(!finished_session_expired(
            &mut since, false, 1_000_000, rate, 300
        ));
        assert_eq!(since, None);
        // Finishes on a server up for an hour: kept for the timeout.
        let finish = 3_600 * rate;
        assert!(!finished_session_expired(
            &mut since, true, finish, rate, 300
        ));
        assert!(!finished_session_expired(
            &mut since,
            true,
            finish + 300 * rate,
            rate,
            300
        ));
        assert!(finished_session_expired(
            &mut since,
            true,
            finish + 301 * rate,
            rate,
            300
        ));
        // Raced again before then: the clock starts over at the next finish.
        let mut again = Some(finish);
        assert!(!finished_session_expired(
            &mut again,
            false,
            finish + 10 * rate,
            rate,
            300
        ));
        assert!(!finished_session_expired(
            &mut again,
            true,
            finish + 400 * rate,
            rate,
            300
        ));
        assert_eq!(again, Some(finish + 400 * rate));
    }
}

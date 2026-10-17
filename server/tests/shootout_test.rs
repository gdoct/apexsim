//! A one-lap qualifying shoot-out end to end: driven through `GameSession`
//! at the Norisring, the cars run one at a time in the reverse of the start
//! order, each an outlap from the pit exit and one flying lap while the rest
//! wait in their garages, the AI entries included, and the classification
//! (and the race's grid) is the flying laps. Then over the wire: the format
//! echoed on join, `ShootoutState` to the session, and going out of turn
//! refused.

mod common;

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::Duration;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::network::{
    ClientMessage, LobbyStateData, ServerMessage, SessionJoinedData, ShootoutStateData,
};
use apexsim_server::track_loader::TrackLoader;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};
use uuid::Uuid;

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

#[test]
fn a_shootout_runs_each_car_alone_and_classifies_its_flying_lap() {
    let track = TrackLoader::load_from_file("../content/tracks/default/Norisring/Norisring.yaml")
        .expect("failed to load the Norisring");
    let car = CarConfig::default();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let ai_profiles: Vec<AiDriverProfile> = (0..2)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("Shoot-out AI {}", i + 1), 95);
            p.id = Uuid::from_u128(3000 + i as u128);
            p
        })
        .collect();
    let ai_ids: Vec<PlayerId> = ai_profiles.iter().map(|p| p.id).collect();
    let host = Uuid::from_u128(1);
    let session = RaceSession::new(host, track.id, SessionKind::Multiplayer, 8, 2, 3);
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
    gs.spawn_ai_drivers();
    gs.add_player(host, car_id).expect("seat");
    gs.set_driver_name(host, "Host");
    gs.set_qualifying_format(QualifyingFormat::Shootout);
    // Start order: AI 2, the host, AI 1; the shoot-out runs it backwards.
    gs.set_grid_order(vec!["@ai:2".into(), "@host".into(), "@ai:1".into()]);
    let expected = vec![ai_ids[0], host, ai_ids[1]];

    gs.set_game_mode(GameMode::Qualification);
    assert!(gs.is_shootout_round());
    assert_eq!(gs.race_clock(), None, "a shoot-out has no clock");
    let state = gs.shootout_state().expect("a shoot-out runs");
    let indices: Vec<u8> = expected
        .iter()
        .map(|id| gs.car_index_of(id).unwrap())
        .collect();
    assert_eq!(state.order, indices);
    assert_eq!((state.running, state.done), (1, 0));
    assert_eq!(
        gs.hotlap_relocate(&host, HotlapDestination::Track, false),
        Err("A shoot-out: each car is sent out in its turn")
    );

    let mut runs: Vec<PlayerId> = Vec::new();
    let mut timed_laps: HashMap<PlayerId, Vec<(u32, bool)>> = HashMap::new();
    let mut ticks = 0usize;
    let limit = HZ * 3 * 400;
    while gs.session.state != SessionState::Finished {
        ticks += 1;
        assert!(ticks < limit, "the shoot-out never ended");
        let out: Vec<PlayerId> = gs
            .session
            .participants
            .values()
            .filter(|s| !s.in_garage)
            .map(|s| s.player_id)
            .collect();
        assert!(out.len() <= 1, "one car out at a time: {out:?}");
        if let Some(id) = out.first() {
            if runs.last() != Some(id) {
                runs.push(*id);
            }
        }
        let inputs: HashMap<PlayerId, PlayerInputData> = out
            .iter()
            .map(|id| {
                let input = if ai_ids.contains(id) {
                    gs.generate_ai_input(id)
                } else {
                    gs.cooldown_input(id)
                };
                (*id, input)
            })
            .collect();
        gs.tick(&inputs);
        for lap in gs.take_lap_events() {
            if let Some(ms) = lap.event.lap_time_ms {
                timed_laps
                    .entry(lap.player_id)
                    .or_default()
                    .push((ms, lap.event.valid));
                if lap.event.valid {
                    gs.note_qualifying_lap(lap.player_id, ms);
                }
            }
        }
    }

    println!("shoot-out laps: {timed_laps:?}");
    assert_eq!(runs, expected, "each car ran once, in order");
    for id in &expected {
        let laps = &timed_laps[id];
        assert_eq!(laps.len(), 1, "one flying lap each: {laps:?}");
        assert!(gs.session.participants[id].in_garage, "every car is in");
        let (ms, _) = laps[0];
        assert!(
            (30_000..200_000).contains(&ms),
            "a flying lap of the Norisring: {ms} ms"
        );
    }
    let state = gs.shootout_state().unwrap();
    assert_eq!((state.running, state.done), (0, 3));

    // The classification is the valid flying laps, fastest first.
    let mut valid: Vec<(u32, PlayerId)> = expected
        .iter()
        .filter_map(|id| {
            let (ms, ok) = timed_laps[id][0];
            ok.then_some((ms, *id))
        })
        .collect();
    valid.sort();
    let classified: Vec<PlayerId> = valid.iter().map(|(_, id)| *id).collect();
    assert_eq!(gs.qualifying_order(), classified);

    // The race grids from it.
    gs.start_countdown_mode(1, GameMode::Race);
    for (i, id) in classified.iter().enumerate() {
        assert_eq!(gs.session.participants[id].grid_position, (i + 1) as u8);
    }
}

type TestResult<T> = Result<T, Box<dyn std::error::Error>>;

struct Client {
    tcp: TcpStream,
}

impl Client {
    async fn connect(name: &str, addr: SocketAddr) -> TestResult<(Self, LobbyStateData)> {
        let mut client = Client {
            tcp: TcpStream::connect(addr).await?,
        };
        client
            .send(&ClientMessage::Authenticate {
                token: format!("test_token_{name}"),
                player_name: name.to_string(),
                protocol_version: apexsim_server::network::PROTOCOL_VERSION,
                resume_token: None,
            })
            .await?;
        match client.recv().await? {
            ServerMessage::AuthSuccess(_) => {}
            other => return Err(format!("expected AuthSuccess, got {other:?}").into()),
        }
        match client.recv().await? {
            ServerMessage::LobbyState(lobby) => Ok((client, lobby)),
            other => Err(format!("expected LobbyState, got {other:?}").into()),
        }
    }

    async fn send(&mut self, msg: &ClientMessage) -> TestResult<()> {
        let data = rmp_serde::to_vec_named(msg)?;
        self.tcp
            .write_all(&(data.len() as u32).to_be_bytes())
            .await?;
        self.tcp.write_all(&data).await?;
        self.tcp.flush().await?;
        Ok(())
    }

    async fn recv(&mut self) -> TestResult<ServerMessage> {
        let mut len = [0u8; 4];
        self.tcp.read_exact(&mut len).await?;
        let mut buf = vec![0u8; u32::from_be_bytes(len) as usize];
        self.tcp.read_exact(&mut buf).await?;
        Ok(rmp_serde::from_slice(&buf)?)
    }

    async fn wait_joined(&mut self) -> TestResult<SessionJoinedData> {
        for _ in 0..200 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::SessionJoined(data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no SessionJoined".into())
    }

    async fn wait_shootout(&mut self) -> TestResult<ShootoutStateData> {
        for _ in 0..2000 {
            match timeout(Duration::from_secs(5), self.recv()).await?? {
                ServerMessage::ShootoutState(data) => return Ok(data),
                ServerMessage::Error { message, .. } => return Err(message.into()),
                _ => continue,
            }
        }
        Err("no ShootoutState".into())
    }

    async fn wait_error(&mut self) -> TestResult<String> {
        for _ in 0..2000 {
            if let ServerMessage::Error { message, .. } =
                timeout(Duration::from_secs(5), self.recv()).await??
            {
                return Ok(message);
            }
        }
        Err("no Error".into())
    }
}

#[tokio::test]
async fn a_shootout_is_echoed_and_its_state_sent_over_the_wire() {
    let result = timeout(Duration::from_secs(60), async {
        let server = common::start_test_server().await;
        let (mut host, lobby) = Client::connect("Host", server.tcp_addr).await?;
        let car_id = lobby.car_configs.first().ok_or("no cars")?.id;
        let track_id = lobby.track_configs.first().ok_or("no tracks")?.id;
        host.send(&ClientMessage::SelectCar {
            car_config_id: car_id,
            livery: 0,
        })
        .await?;
        sleep(Duration::from_millis(50)).await;
        host.send(&ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 6,
            ai_count: 2,
            lap_limit: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            // The host starts last, so runs first.
            grid_order: vec!["@ai:1".into(), "@ai:2".into(), "@host".into()],
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Shootout,
            qualifying_lap_allowance: 99,
            race_rules: Default::default(),
        })
        .await?;
        let joined = host.wait_joined().await?;
        assert_eq!(joined.qualifying_format, QualifyingFormat::Shootout);
        assert_eq!(
            joined.qualifying_lap_allowance, MAX_QUALIFYING_LAP_ALLOWANCE,
            "clamped"
        );

        host.send(&ClientMessage::SetGameMode {
            mode: GameMode::Qualification,
        })
        .await?;
        let state = host.wait_shootout().await?;
        assert_eq!(state.order.len(), 3, "the host and two AI");
        assert_eq!((state.running, state.done), (1, 0));
        {
            let state_read = server.state.read().await;
            let session = state_read
                .sessions
                .get(&joined.session_id)
                .ok_or("no session")?;
            let host_id = session.session.host_player_id;
            assert_eq!(
                session.car_index_of(&host_id),
                Some(state.order[0]),
                "{state:?}"
            );
            assert!(!session.session.participants[&host_id].in_garage);
        }
        let ai_index = state.order[1];
        // Asking to go out is refused in a shoot-out, even on one's turn.
        host.send(&ClientMessage::HotlapRelocate {
            destination: HotlapDestination::Track,
            cold_tyres: false,
        })
        .await?;
        let message = host.wait_error().await?;
        assert!(message.contains("shoot-out"), "{message}");

        // The host gives up their run: the next car is called, and everyone
        // hears it.
        host.send(&ClientMessage::HotlapRelocate {
            destination: HotlapDestination::Garage,
            cold_tyres: false,
        })
        .await?;
        let next = host.wait_shootout().await?;
        assert_eq!((next.running, next.done), (2, 1));
        assert_eq!(next.order[1], ai_index);
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;
    result.expect("timed out").expect("shoot-out over the wire");
}

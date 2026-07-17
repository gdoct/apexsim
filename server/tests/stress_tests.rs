mod common;

use std::io::Write;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::Mutex;
use tokio::time::{sleep, timeout, Instant};

use apexsim_server::data::*;
use apexsim_server::network::{ClientMessage, ServerMessage};

/// Results from a single tick rate test
#[derive(Debug)]
struct TickRateTestResult {
    target_hz: u16,
    actual_hz: f64,
    accuracy_percent: f64,
    tick_count: u32,
    duration_secs: f64,
    avg_tick_interval_us: f64,
    jitter_us: f64,
    min_interval_us: f64,
    max_interval_us: f64,
    missed_ticks: u32,
    passed: bool,
}

/// Tick rate stress test - tests server at multiple tick rates to find performance limits
/// Run: cargo test --test stress_tests test_tick_rate_stress -- --ignored --nocapture
#[tokio::test]
#[ignore = "long-running stress test; run with --ignored"]
async fn test_tick_rate_stress() {
    println!("╔══════════════════════════════════════════════════════════════════════════════╗");
    println!("║              TICK RATE STRESS TEST - PERFORMANCE BENCHMARK                   ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════╣");
    println!("║  Testing server tick rates: 120Hz, 240Hz, 480Hz, 960Hz                      ║");
    println!("║  Each test runs for 10 seconds measuring actual tick rate and timing jitter  ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════╝");
    println!();

    // Config validation caps tick_rate_hz at 1000 (tokio's ~1ms timer
    // granularity makes higher rates meaningless), so the sweep stays
    // inside the supported envelope.
    let tick_rates = [120u16, 240, 480, 960];
    let test_duration_secs = 10.0;
    let mut results: Vec<TickRateTestResult> = Vec::new();

    for (i, &target_hz) in tick_rates.iter().enumerate() {
        println!(
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        );
        println!(
            "  Testing {}Hz (Test {} of {})",
            target_hz,
            i + 1,
            tick_rates.len()
        );
        println!(
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        );

        // Start in-process server with this tick rate on ephemeral ports
        print!("  Starting server... ");
        std::io::stdout().flush().unwrap();
        let server = common::start_test_server_with_tick_rate(target_hz).await;
        let tcp_addr = server.tcp_addr;
        println!("OK");

        // Connect client and run test
        print!("  Connecting client... ");
        std::io::stdout().flush().unwrap();

        let result = run_tick_rate_test(tcp_addr, target_hz, test_duration_secs).await;

        match result {
            Ok(test_result) => {
                println!("  Results:");
                println!("    Target:    {:>6} Hz", test_result.target_hz);
                println!(
                    "    Actual:    {:>6.1} Hz ({:.1}% of target)",
                    test_result.actual_hz, test_result.accuracy_percent
                );
                println!(
                    "    Ticks:     {:>6} over {:.1}s",
                    test_result.tick_count, test_result.duration_secs
                );
                println!(
                    "    Avg interval: {:>8.1} µs (target: {:.1} µs)",
                    test_result.avg_tick_interval_us,
                    1_000_000.0 / target_hz as f64
                );
                println!(
                    "    Jitter:    {:>8.1} µs (std dev from target)",
                    test_result.jitter_us
                );
                println!(
                    "    Min/Max:   {:>8.1} / {:.1} µs",
                    test_result.min_interval_us, test_result.max_interval_us
                );
                println!(
                    "    Missed:    {:>6} packets ({:.1}% loss)",
                    test_result.missed_ticks,
                    if test_result.tick_count > 0 {
                        100.0 * test_result.missed_ticks as f64 / test_result.tick_count as f64
                    } else {
                        0.0
                    }
                );

                let status = if test_result.passed {
                    "✅ PASS"
                } else {
                    "⚠️  DEGRADED"
                };
                println!("    Status:    {}", status);

                results.push(test_result);
            }
            Err(e) => {
                println!("FAILED: {}", e);
                results.push(TickRateTestResult {
                    target_hz,
                    actual_hz: 0.0,
                    accuracy_percent: 0.0,
                    tick_count: 0,
                    duration_secs: test_duration_secs,
                    avg_tick_interval_us: 0.0,
                    jitter_us: 0.0,
                    min_interval_us: 0.0,
                    max_interval_us: 0.0,
                    missed_ticks: 0,
                    passed: false,
                });
            }
        }

        // Stop server
        print!("  Stopping server... ");
        std::io::stdout().flush().unwrap();
        server.shutdown().await;
        println!("OK");
        println!();
    }

    // Print summary
    println!("╔══════════════════════════════════════════════════════════════════════════════════════════╗");
    println!("║                              PERFORMANCE SUMMARY                                         ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════════════════╣");
    println!("║  Target Hz │ Actual Hz │ Ratio  │ Jitter µs │ Pkt Loss │ Status                          ║");
    println!("╠────────────┼───────────┼────────┼───────────┼──────────┼─────────────────────────────────╣");

    let mut max_sustainable_hz = 0u16;
    let mut first_ratio: Option<f64> = None;

    for result in &results {
        let status_icon = if result.passed {
            "✅"
        } else if result.actual_hz > 0.0 {
            "⚠️ "
        } else {
            "❌"
        };
        let pkt_loss = if result.tick_count > 0 {
            100.0 * result.missed_ticks as f64 / result.tick_count as f64
        } else {
            0.0
        };
        let ratio = result.actual_hz / result.target_hz as f64;

        if first_ratio.is_none() {
            first_ratio = Some(ratio);
        }

        println!("║  {:>8} │ {:>9.1} │ {:>5.1}% │ {:>9.1} │ {:>7.1}% │ {}                              ║",
            result.target_hz,
            result.actual_hz,
            ratio * 100.0,
            result.jitter_us,
            pkt_loss,
            status_icon
        );

        if result.passed && result.target_hz > max_sustainable_hz {
            max_sustainable_hz = result.target_hz;
        }
    }

    // Check ratio consistency - all ratios should be similar
    let ratios: Vec<f64> = results
        .iter()
        .filter(|r| r.actual_hz > 0.0)
        .map(|r| r.actual_hz / r.target_hz as f64)
        .collect();
    let avg_ratio = ratios.iter().sum::<f64>() / ratios.len() as f64;
    let ratio_variance =
        ratios.iter().map(|r| (r - avg_ratio).powi(2)).sum::<f64>() / ratios.len() as f64;
    let ratio_std_dev = ratio_variance.sqrt();

    println!("╠══════════════════════════════════════════════════════════════════════════════════════════╣");
    println!("║  Maximum sustainable tick rate: {:>5} Hz                                                  ║", max_sustainable_hz);
    println!("║  Average tick rate ratio: {:.1}% (std dev: {:.1}%)                                        ║", avg_ratio * 100.0, ratio_std_dev * 100.0);
    println!("║  Note: Ratio < 100% is normal due to TCP batching reducing telemetry receive rate        ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════════════════╝");

    // Test passes if at least 240Hz is sustainable
    assert!(
        max_sustainable_hz >= 240,
        "Server should sustain at least 240Hz, but max sustainable was {}Hz",
        max_sustainable_hz
    );

    println!(
        "\n✅ TEST PASSED: Server sustains up to {}Hz tick rate",
        max_sustainable_hz
    );
}

async fn run_tick_rate_test(
    tcp_addr: SocketAddr,
    target_hz: u16,
    duration_secs: f64,
) -> Result<TickRateTestResult, Box<dyn std::error::Error + Send + Sync>> {
    // Connect to server
    let tcp_stream = timeout(Duration::from_secs(5), TcpStream::connect(tcp_addr)).await??;

    let mut client = TestClientMinimal {
        tcp_stream,
        heartbeat_tick: 0,
    };
    println!("OK");

    // Authenticate
    print!("  Authenticating... ");
    std::io::stdout().flush().unwrap();
    let (player_id, car_id, track_id) = client
        .authenticate(&format!("StressTest_{}", target_hz))
        .await?;
    println!("OK (player: {})", player_id);

    // Select car and create session
    print!("  Creating session... ");
    std::io::stdout().flush().unwrap();
    client.select_car(car_id).await?;
    let session_id = client.create_session(track_id).await?;
    println!("OK (session: {})", session_id);

    // Start session
    print!("  Starting race... ");
    std::io::stdout().flush().unwrap();
    client.start_session().await?;

    // Wait for countdown, sending heartbeats to keep connection alive
    let countdown_start = Instant::now();
    while countdown_start.elapsed() < Duration::from_secs(6) {
        client.send_heartbeat().await?;
        sleep(Duration::from_secs(1)).await;
    }
    println!("OK");

    // Drain the countdown-era telemetry backlog: nothing read the socket
    // while waiting out the countdown, so seconds of queued telemetry are
    // sitting in the kernel buffer. Measuring across that backlog inflates
    // the apparent tick rate (old ticks read in fast-forward). Reading is
    // orders of magnitude faster than production, so a fixed 2s window
    // clears the backlog and leaves us at the live edge of the stream.
    let drain_deadline = Instant::now() + Duration::from_secs(2);
    let mut drain_heartbeat = Instant::now();
    while Instant::now() < drain_deadline {
        if drain_heartbeat.elapsed() > Duration::from_secs(1) {
            let _ = client.send_heartbeat().await;
            drain_heartbeat = Instant::now();
        }
        match timeout(Duration::from_millis(50), client.receive_message()).await {
            Ok(Ok(_)) => continue,
            Ok(Err(_)) => break,
            Err(_) => continue,
        }
    }

    // Collect tick timing data
    print!("  Collecting telemetry for {:.0}s... ", duration_secs);
    std::io::stdout().flush().unwrap();

    let collection_start = Instant::now();
    let mut last_heartbeat = Instant::now();
    let mut first_tick_num: Option<u32> = None;
    let mut last_tick_num: u32 = 0;
    let mut packets_received: u32 = 0;
    let mut prev_server_tick: Option<u32> = None;
    let mut tick_gaps: Vec<u32> = Vec::new(); // Gaps between consecutive server ticks we received

    while collection_start.elapsed().as_secs_f64() < duration_secs {
        // Send heartbeats every 2 seconds
        if last_heartbeat.elapsed() > Duration::from_secs(2) {
            let _ = client.send_heartbeat().await;
            last_heartbeat = Instant::now();
        }

        match timeout(Duration::from_millis(100), client.receive_message()).await {
            Ok(Ok(ServerMessage::TelemetryCompact(telemetry))) => {
                packets_received += 1;

                if first_tick_num.is_none() {
                    first_tick_num = Some(telemetry.server_tick);
                }

                // Track gaps between consecutive server ticks we receive
                if let Some(prev) = prev_server_tick {
                    let gap = telemetry.server_tick.saturating_sub(prev);
                    if gap > 0 {
                        tick_gaps.push(gap);
                    }
                }
                prev_server_tick = Some(telemetry.server_tick);
                last_tick_num = telemetry.server_tick;
            }
            Ok(Ok(_)) => continue,
            Ok(Err(_)) => break,
            Err(_) => continue, // Timeout, try again
        }

        // Send input to keep connection alive (occasionally)
        if packets_received.is_multiple_of(20) {
            let _ = client.send_input(0.5, 0.0, 0.0, last_tick_num).await;
        }
    }

    let collection_end = Instant::now();
    let actual_duration = collection_end
        .duration_since(collection_start)
        .as_secs_f64();

    println!("OK ({} packets)", packets_received);

    // Calculate statistics based on SERVER tick numbers
    let first_tick = first_tick_num.ok_or("No ticks received")?;
    let total_server_ticks = last_tick_num.saturating_sub(first_tick);

    if total_server_ticks < 100 {
        return Err(format!(
            "Not enough server ticks elapsed: {} ticks",
            total_server_ticks
        )
        .into());
    }

    // Calculate actual tick rate: server ticks elapsed / wall-clock collection time
    let actual_hz = total_server_ticks as f64 / actual_duration;
    let accuracy_percent = (actual_hz / target_hz as f64) * 100.0;

    // Expected vs actual interval
    let expected_interval_us = 1_000_000.0 / target_hz as f64;
    let actual_interval_us = 1_000_000.0 / actual_hz;

    // Calculate jitter based on gaps in tick numbers we received
    // A gap of 1 is perfect (consecutive ticks), gap > 1 means we missed some
    let avg_gap = if tick_gaps.is_empty() {
        1.0
    } else {
        tick_gaps.iter().map(|&g| g as f64).sum::<f64>() / tick_gaps.len() as f64
    };

    // Jitter: standard deviation of gaps from ideal (1.0)
    let jitter_in_ticks = if tick_gaps.is_empty() {
        0.0
    } else {
        let variance = tick_gaps
            .iter()
            .map(|&g| (g as f64 - avg_gap).powi(2))
            .sum::<f64>()
            / tick_gaps.len() as f64;
        variance.sqrt()
    };
    let jitter_us = jitter_in_ticks * expected_interval_us;

    // Calculate packet loss relative to the telemetry cadence actually in
    // use: the server broadcasts every `telemetry_divisor`-th tick, so the
    // most common (modal) tick gap between received packets IS the cadence.
    // Expecting one packet per tick would count the divisor as "loss".
    let modal_gap = {
        let mut counts: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
        for &g in &tick_gaps {
            *counts.entry(g).or_default() += 1;
        }
        counts
            .into_iter()
            .max_by_key(|(_, c)| *c)
            .map(|(g, _)| g)
            .unwrap_or(1)
            .max(1)
    };
    let expected_packets = total_server_ticks / modal_gap;
    let missed_packets = expected_packets.saturating_sub(packets_received);
    let packet_loss_percent = if expected_packets > 0 {
        100.0 * missed_packets as f64 / expected_packets as f64
    } else {
        0.0
    };

    // Min/max observed gap (in terms of equivalent microseconds)
    let min_gap = tick_gaps.iter().min().copied().unwrap_or(1) as f64 * actual_interval_us;
    let max_gap = tick_gaps.iter().max().copied().unwrap_or(1) as f64 * actual_interval_us;

    // Pass criteria:
    // - Tick rate should scale linearly with config (allow for some overhead)
    // - Ratio to target should be consistent across all tests
    // - < 5% packet loss
    // - Low jitter (tick gaps should be consistent)
    // For this stress test, we focus on relative consistency rather than absolute accuracy
    // since network batching affects receive rate
    let ratio = actual_hz / target_hz as f64;
    let passed = (0.45..=1.10).contains(&ratio)        // But shouldn't exceed target significantly
        && packet_loss_percent < 5.0
        && jitter_in_ticks < 2.0; // Gaps should be consistent (mostly 1s)

    Ok(TickRateTestResult {
        target_hz,
        actual_hz,
        accuracy_percent,
        tick_count: total_server_ticks,
        duration_secs: actual_duration,
        avg_tick_interval_us: actual_interval_us,
        jitter_us,
        min_interval_us: min_gap,
        max_interval_us: max_gap,
        missed_ticks: missed_packets,
        passed,
    })
}

/// Minimal test client for stress testing (less overhead than full TestClient)
struct TestClientMinimal {
    tcp_stream: TcpStream,
    heartbeat_tick: u32,
}

impl TestClientMinimal {
    async fn authenticate(
        &mut self,
        name: &str,
    ) -> Result<(PlayerId, CarConfigId, TrackConfigId), Box<dyn std::error::Error + Send + Sync>>
    {
        let msg = ClientMessage::Authenticate {
            token: format!("test_token_{}", name),
            player_name: name.to_string(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
        };
        self.send_message(&msg).await?;
        sleep(Duration::from_millis(50)).await;

        let response = self.receive_message().await?;
        let player_id = match response {
            ServerMessage::AuthSuccess(data) => data.player_id,
            ServerMessage::AuthFailure { reason } => {
                return Err(format!("Auth failed: {}", reason).into())
            }
            _ => return Err("Unexpected response".into()),
        };

        // Get lobby state
        let lobby = self.receive_message().await?;
        let (car_id, track_id) = match lobby {
            ServerMessage::LobbyState(lobby) => {
                let car_id = lobby.car_configs.first().ok_or("No cars")?.id;
                let track_id = lobby.track_configs.first().ok_or("No tracks")?.id;
                (car_id, track_id)
            }
            _ => return Err("Expected lobby state".into()),
        };

        Ok((player_id, car_id, track_id))
    }

    async fn select_car(
        &mut self,
        car_id: CarConfigId,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let msg = ClientMessage::SelectCar {
            car_config_id: car_id,
        };
        self.send_message(&msg).await
    }

    async fn create_session(
        &mut self,
        track_id: TrackConfigId,
    ) -> Result<SessionId, Box<dyn std::error::Error + Send + Sync>> {
        let msg = ClientMessage::CreateSession {
            track_config_id: track_id,
            max_players: 4,
            session_kind: SessionKind::Practice,
            ai_count: 0,
            lap_limit: 3,
        };
        self.send_message(&msg).await?;

        let response = self.receive_message().await?;
        match response {
            ServerMessage::SessionJoined(data) => Ok(data.session_id),
            ServerMessage::Error { message, .. } => {
                Err(format!("Create failed: {}", message).into())
            }
            _ => Err("Unexpected response".into()),
        }
    }

    async fn start_session(&mut self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let msg = ClientMessage::StartSession;
        self.send_message(&msg).await
    }

    async fn send_input(
        &mut self,
        throttle: f32,
        brake: f32,
        steering: f32,
        tick: u32,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let msg = ClientMessage::PlayerInput {
            server_tick_ack: tick,
            throttle,
            brake,
            steering,
            gear: None,
            clutch: None,
        };
        self.send_message(&msg).await
    }

    async fn send_heartbeat(&mut self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.heartbeat_tick += 1;
        let msg = ClientMessage::Heartbeat {
            client_tick: self.heartbeat_tick,
        };
        self.send_message(&msg).await
    }

    async fn send_message(
        &mut self,
        msg: &ClientMessage,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let data = rmp_serde::to_vec_named(msg)?;
        let len = (data.len() as u32).to_be_bytes();
        self.tcp_stream.write_all(&len).await?;
        self.tcp_stream.write_all(&data).await?;
        self.tcp_stream.flush().await?;
        Ok(())
    }

    async fn receive_message(
        &mut self,
    ) -> Result<ServerMessage, Box<dyn std::error::Error + Send + Sync>> {
        let mut len_buf = [0u8; 4];
        self.tcp_stream.read_exact(&mut len_buf).await?;
        let len = u32::from_be_bytes(len_buf);

        let mut buf = vec![0u8; len as usize];
        self.tcp_stream.read_exact(&mut buf).await?;
        let msg: ServerMessage = rmp_serde::from_slice(&buf)?;
        Ok(msg)
    }

    async fn join_session(
        &mut self,
        session_id: SessionId,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        // Small delay to let any pending broadcasts arrive
        sleep(Duration::from_millis(10)).await;

        let msg = ClientMessage::JoinSession { session_id };
        self.send_message(&msg).await?;

        // Read responses until we get SessionJoined (skip LobbyState updates)
        for _ in 0..10 {
            let response = self.receive_message().await?;
            match response {
                ServerMessage::SessionJoined(_) => return Ok(()),
                ServerMessage::LobbyState(_) => continue,
                ServerMessage::SessionRoster(_) => continue, // Skip lobby updates
                ServerMessage::Error { message, .. } => {
                    return Err(format!("Join failed: {}", message).into())
                }
                _ => continue,
            }
        }
        Err("Join timed out waiting for SessionJoined".into())
    }
}

/// Results from a multi-client load test
#[derive(Debug)]
struct MultiClientTestResult {
    target_hz: u16,
    client_count: usize,
    actual_hz: f64,
    ratio_percent: f64,
    total_inputs_sent: u64,
    total_telemetry_received: u64,
    avg_telemetry_per_client: f64,
    clients_with_telemetry: usize,
    passed: bool,
}

/// Multi-client load test - tests server with 16 concurrent clients sending random inputs
/// Run: cargo test --test stress_tests test_multi_client_load -- --ignored --nocapture
#[tokio::test]
#[ignore = "long-running stress test; run with --ignored"]
async fn test_multi_client_load() {
    println!("╔══════════════════════════════════════════════════════════════════════════════════════════╗");
    println!("║              MULTI-CLIENT LOAD TEST - 16 CLIENTS WITH RANDOM INPUT                       ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════════════════╣");
    println!("║  Testing with 16 concurrent clients at: 120Hz, 240Hz, 360Hz, 480Hz                       ║");
    println!("║  Each client sends random throttle inputs during a 10 second race                        ║");
    println!("║  (16 is max supported by default track grid positions)                                   ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════════════════╝");
    println!();

    let tick_rates = [120u16, 240, 360, 480];
    let client_count = 16;
    let test_duration_secs = 10.0;
    let mut results: Vec<MultiClientTestResult> = Vec::new();

    for (i, &target_hz) in tick_rates.iter().enumerate() {
        println!("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        println!(
            "  Testing {}Hz with {} clients (Test {} of {})",
            target_hz,
            client_count,
            i + 1,
            tick_rates.len()
        );
        println!("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

        // Start in-process server with this tick rate on ephemeral ports
        print!("  Starting server at {}Hz... ", target_hz);
        std::io::stdout().flush().unwrap();
        let server = common::start_test_server_with_tick_rate(target_hz).await;
        let tcp_addr = server.tcp_addr;
        println!("OK");

        // Run multi-client test
        let result =
            run_multi_client_test(tcp_addr, target_hz, client_count, test_duration_secs).await;

        match result {
            Ok(test_result) => {
                println!("  Results:");
                println!("    Clients connected: {:>4}", test_result.client_count);
                println!("    Target tick rate:  {:>4} Hz", test_result.target_hz);
                println!(
                    "    Actual tick rate:  {:>6.1} Hz ({:.1}% of target)",
                    test_result.actual_hz, test_result.ratio_percent
                );
                println!(
                    "    Total inputs sent: {:>6}",
                    test_result.total_inputs_sent
                );
                println!(
                    "    Telemetry received:{:>6} total ({:.1} avg/client)",
                    test_result.total_telemetry_received, test_result.avg_telemetry_per_client
                );
                println!(
                    "    Clients w/ telemetry: {}/{}",
                    test_result.clients_with_telemetry, test_result.client_count
                );

                let status = if test_result.passed {
                    "✅ PASS"
                } else {
                    "⚠️  DEGRADED"
                };
                println!("    Status:            {}", status);

                results.push(test_result);
            }
            Err(e) => {
                println!("  FAILED: {}", e);
                results.push(MultiClientTestResult {
                    target_hz,
                    client_count,
                    actual_hz: 0.0,
                    ratio_percent: 0.0,
                    total_inputs_sent: 0,
                    total_telemetry_received: 0,
                    avg_telemetry_per_client: 0.0,
                    clients_with_telemetry: 0,
                    passed: false,
                });
            }
        }

        // Stop server
        print!("  Stopping server... ");
        std::io::stdout().flush().unwrap();
        server.shutdown().await;
        println!("OK");
        println!();
    }

    // Print summary
    println!("╔══════════════════════════════════════════════════════════════════════════════════════════════════════╗");
    println!("║                                    MULTI-CLIENT LOAD TEST SUMMARY                                    ║");
    println!("╠══════════════════════════════════════════════════════════════════════════════════════════════════════╣");
    println!("║  Target │ Actual Hz │ Ratio  │ Inputs Sent │ Telemetry │ Clients OK │ Status                         ║");
    println!("╠─────────┼───────────┼────────┼─────────────┼───────────┼────────────┼────────────────────────────────╣");

    let mut max_sustainable_hz = 0u16;

    for result in &results {
        let status_icon = if result.passed {
            "✅"
        } else if result.actual_hz > 0.0 {
            "⚠️ "
        } else {
            "❌"
        };
        println!("║  {:>5} │ {:>9.1} │ {:>5.1}% │ {:>11} │ {:>9} │ {:>5}/{:<4} │ {}                             ║",
            result.target_hz,
            result.actual_hz,
            result.ratio_percent,
            result.total_inputs_sent,
            result.total_telemetry_received,
            result.clients_with_telemetry,
            result.client_count,
            status_icon
        );

        if result.passed && result.target_hz > max_sustainable_hz {
            max_sustainable_hz = result.target_hz;
        }
    }

    println!("╠══════════════════════════════════════════════════════════════════════════════════════════════════════╣");
    println!("║  Maximum sustainable tick rate with {} clients: {:>5} Hz                                             ║", client_count, max_sustainable_hz);
    println!("║  (Sustainable = ≥40% tick ratio with all clients receiving telemetry)                                ║");
    println!("╚══════════════════════════════════════════════════════════════════════════════════════════════════════╝");

    // Test passes if at least 120Hz is sustainable with 20 clients
    assert!(
        max_sustainable_hz >= 120,
        "Server should sustain at least 120Hz with {} clients, but max sustainable was {}Hz",
        client_count,
        max_sustainable_hz
    );

    println!(
        "\n✅ TEST PASSED: Server sustains up to {}Hz with {} concurrent clients",
        max_sustainable_hz, client_count
    );
}

async fn run_multi_client_test(
    tcp_addr: SocketAddr,
    target_hz: u16,
    client_count: usize,
    duration_secs: f64,
) -> Result<MultiClientTestResult, Box<dyn std::error::Error + Send + Sync>> {
    print!("  Connecting {} clients... ", client_count);
    std::io::stdout().flush().unwrap();

    // Connect all clients
    let mut clients: Vec<TestClientMinimal> = Vec::new();
    let mut car_id = None;
    let mut track_id = None;

    for i in 0..client_count {
        // First client may need retries while server is starting
        let tcp_stream = if i == 0 {
            let mut attempts = 0;
            loop {
                match timeout(Duration::from_secs(2), TcpStream::connect(tcp_addr)).await {
                    Ok(Ok(s)) => break s,
                    Ok(Err(_)) | Err(_) => {
                        attempts += 1;
                        if attempts >= 5 {
                            return Err("Client 0 connect failed after 5 attempts".into());
                        }
                        sleep(Duration::from_millis(500)).await;
                    }
                }
            }
        } else {
            match timeout(Duration::from_secs(5), TcpStream::connect(tcp_addr)).await {
                Ok(Ok(s)) => s,
                Ok(Err(e)) => return Err(format!("Client {} connect failed: {}", i, e).into()),
                Err(_) => return Err(format!("Client {} connect timeout", i).into()),
            }
        };

        let mut client = TestClientMinimal {
            tcp_stream,
            heartbeat_tick: 0,
        };

        // Authenticate
        let (_player_id, c_id, t_id) = client.authenticate(&format!("LoadTest_{}", i)).await?;

        if car_id.is_none() {
            car_id = Some(c_id);
            track_id = Some(t_id);
        }

        // Select car
        client.select_car(c_id).await?;

        clients.push(client);

        // Small delay between connections to avoid overwhelming
        if i % 5 == 4 {
            sleep(Duration::from_millis(50)).await;
        }
    }
    println!("OK ({} connected)", clients.len());

    let _car_id = car_id.ok_or("No car ID")?;
    let track_id = track_id.ok_or("No track ID")?;

    // Give server time to process all auth/select messages
    sleep(Duration::from_millis(200)).await;

    // First client creates session with max_players = 16 (max grid positions on default track)
    print!("  Creating session... ");
    std::io::stdout().flush().unwrap();

    // Create session with enough slots for all clients
    let create_msg = ClientMessage::CreateSession {
        track_config_id: track_id,
        max_players: 16,
        session_kind: SessionKind::Practice,
        ai_count: 0,
        lap_limit: 3,
    };
    clients[0].send_message(&create_msg).await?;

    // Read responses until we get SessionJoined (skip LobbyState updates)
    let mut session_id = None;
    for _ in 0..5 {
        let response = clients[0].receive_message().await?;
        match response {
            ServerMessage::SessionJoined(data) => {
                session_id = Some(data.session_id);
                break;
            }
            ServerMessage::LobbyState(_) => continue,
            ServerMessage::SessionRoster(_) => continue, // Skip lobby updates
            ServerMessage::Error { message, .. } => {
                return Err(format!("Create failed: {}", message).into())
            }
            other => return Err(format!("Unexpected response: {:?}", other).into()),
        }
    }
    let session_id = session_id.ok_or("No session created after retries")?;
    println!("OK (session: {})", session_id);

    // Give server time to process session creation
    sleep(Duration::from_millis(100)).await;

    // Other clients join the session
    print!("  Joining {} clients to session... ", client_count - 1);
    std::io::stdout().flush().unwrap();

    for (i, client) in clients.iter_mut().enumerate().skip(1) {
        match client.join_session(session_id).await {
            Ok(()) => {}
            Err(e) => {
                println!("\n    Client {} failed to join: {}", i, e);
                return Err(e);
            }
        }

        if i % 5 == 0 {
            sleep(Duration::from_millis(20)).await;
        }
    }
    println!("OK");

    // Start session
    print!("  Starting race... ");
    std::io::stdout().flush().unwrap();
    clients[0].start_session().await?;

    // Wait for countdown, sending heartbeats for all clients
    let countdown_start = Instant::now();
    while countdown_start.elapsed() < Duration::from_secs(6) {
        for client in &mut clients {
            let _ = client.send_heartbeat().await;
        }
        sleep(Duration::from_secs(1)).await;
    }
    println!("OK");

    // Split clients into separate tasks for concurrent operation
    print!("  Racing for {:.0}s with random inputs... ", duration_secs);
    std::io::stdout().flush().unwrap();

    // Use Arc<Mutex> to share state between tasks
    let inputs_sent = Arc::new(std::sync::atomic::AtomicU64::new(0));
    let telemetry_received: Arc<Mutex<Vec<u64>>> = Arc::new(Mutex::new(vec![0; client_count]));
    let first_tick: Arc<Mutex<Option<u32>>> = Arc::new(Mutex::new(None));
    let last_tick: Arc<Mutex<u32>> = Arc::new(Mutex::new(0));

    let collection_start = Instant::now();

    // Spawn tasks for each client
    let mut handles = Vec::new();

    for (client_idx, client) in clients.into_iter().enumerate() {
        let inputs_sent = Arc::clone(&inputs_sent);
        let telemetry_received = Arc::clone(&telemetry_received);
        let first_tick = Arc::clone(&first_tick);
        let last_tick = Arc::clone(&last_tick);
        let start = collection_start;
        // Create a seed from current time + client index for deterministic but varied RNG
        let seed = (std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos() as u64)
            .wrapping_add(client_idx as u64);

        let handle = tokio::spawn(async move {
            let mut client = client;
            // Use a simple PRNG seeded with time + client index
            let mut rng_state = seed;
            let mut local_telemetry_count = 0u64;
            let mut local_last_tick = 0u32;
            let mut last_heartbeat = Instant::now();

            // Simple xorshift64 PRNG
            let mut next_random = || -> f32 {
                rng_state ^= rng_state << 13;
                rng_state ^= rng_state >> 7;
                rng_state ^= rng_state << 17;
                (rng_state as f32) / (u64::MAX as f32)
            };

            while start.elapsed().as_secs_f64() < duration_secs {
                // Send heartbeat every 2 seconds
                if last_heartbeat.elapsed() > Duration::from_secs(2) {
                    let _ = client.send_heartbeat().await;
                    last_heartbeat = Instant::now();
                }

                // Send random input
                let throttle: f32 = next_random();
                let brake: f32 = if next_random() < 0.1 {
                    next_random() * 0.5
                } else {
                    0.0
                };
                let steering: f32 = (next_random() - 0.5) * 0.6; // -0.3 to 0.3

                if client
                    .send_input(throttle, brake, steering, local_last_tick)
                    .await
                    .is_ok()
                {
                    inputs_sent.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }

                // Try to receive telemetry (non-blocking)
                if let Ok(Ok(ServerMessage::TelemetryCompact(tel))) =
                    timeout(Duration::from_millis(5), client.receive_message()).await
                {
                    local_telemetry_count += 1;
                    local_last_tick = tel.server_tick;

                    // Update shared first/last tick
                    {
                        let mut ft = first_tick.lock().await;
                        if ft.is_none() {
                            *ft = Some(tel.server_tick);
                        }
                    }
                    {
                        let mut lt = last_tick.lock().await;
                        if tel.server_tick > *lt {
                            *lt = tel.server_tick;
                        }
                    }
                }

                // Small yield to allow other tasks
                tokio::task::yield_now().await;
            }

            // Update telemetry count for this client
            {
                let mut tr = telemetry_received.lock().await;
                tr[client_idx] = local_telemetry_count;
            }
        });

        handles.push(handle);
    }

    // Wait for all client tasks to complete
    for handle in handles {
        let _ = handle.await;
    }

    println!("OK");

    // Calculate results
    let total_inputs = inputs_sent.load(std::sync::atomic::Ordering::Relaxed);
    let telemetry_counts = telemetry_received.lock().await;
    let total_telemetry: u64 = telemetry_counts.iter().sum();
    let clients_with_telemetry = telemetry_counts.iter().filter(|&&c| c > 0).count();
    let avg_telemetry = total_telemetry as f64 / client_count as f64;

    let first_tick_val = first_tick.lock().await.unwrap_or(0);
    let last_tick_val = *last_tick.lock().await;
    let total_server_ticks = last_tick_val.saturating_sub(first_tick_val);

    let actual_duration = collection_start.elapsed().as_secs_f64();
    let actual_hz = if actual_duration > 0.0 {
        total_server_ticks as f64 / actual_duration
    } else {
        0.0
    };
    let ratio_percent = (actual_hz / target_hz as f64) * 100.0;

    // Pass criteria: ≥40% ratio (accounting for TCP batching) and all clients got some telemetry
    let passed = ratio_percent >= 40.0 && clients_with_telemetry == client_count;

    Ok(MultiClientTestResult {
        target_hz,
        client_count,
        actual_hz,
        ratio_percent,
        total_inputs_sent: total_inputs,
        total_telemetry_received: total_telemetry,
        avg_telemetry_per_client: avg_telemetry,
        clients_with_telemetry,
        passed,
    })
}

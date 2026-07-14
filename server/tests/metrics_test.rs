//! Health and Prometheus metrics endpoint tests against an in-process server.

mod common;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

async fn http_get(addr: std::net::SocketAddr, path: &str) -> String {
    let mut stream = TcpStream::connect(addr).await.expect("connect failed");
    let request = format!(
        "GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        path
    );
    stream.write_all(request.as_bytes()).await.unwrap();
    let mut response = Vec::new();
    stream.read_to_end(&mut response).await.unwrap();
    String::from_utf8_lossy(&response).to_string()
}

#[tokio::test]
async fn test_health_and_ready_endpoints() {
    let server = common::start_test_server().await;

    let health = http_get(server.health_addr, "/health").await;
    assert!(health.starts_with("HTTP/1.1 200"), "got: {}", health);

    let ready = http_get(server.health_addr, "/ready").await;
    assert!(ready.starts_with("HTTP/1.1 200"), "got: {}", ready);

    let missing = http_get(server.health_addr, "/nope").await;
    assert!(missing.starts_with("HTTP/1.1 404"), "got: {}", missing);
}

#[tokio::test]
async fn test_metrics_endpoint_exposes_prometheus_format() {
    let server = common::start_test_server().await;

    // Let the game loop run a few ticks so the histogram has data
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;

    let response = http_get(server.health_addr, "/metrics").await;
    assert!(response.starts_with("HTTP/1.1 200"), "got: {}", response);
    for metric in [
        "apexsim_connected_players",
        "apexsim_active_sessions",
        "apexsim_tick_duration_us_count",
        "apexsim_tcp_messages_dropped",
    ] {
        assert!(
            response.contains(metric),
            "missing {} in:\n{}",
            metric,
            response
        );
    }

    // The loop has been running, so ticks must have been observed
    let count_line = response
        .lines()
        .find(|l| l.starts_with("apexsim_tick_duration_us_count"))
        .expect("tick count metric missing");
    let count: u64 = count_line
        .split_whitespace()
        .nth(1)
        .and_then(|v| v.parse().ok())
        .expect("tick count not a number");
    assert!(count > 0, "game loop should have observed ticks");
}

#[tokio::test]
async fn test_health_flips_unavailable_after_shutdown() {
    let server = common::start_test_server().await;
    server.shutdown().await;
    let health = http_get(server.health_addr, "/health").await;
    assert!(health.starts_with("HTTP/1.1 503"), "got: {}", health);
}

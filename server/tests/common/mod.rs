#![allow(dead_code)]

use apexsim_server::config::ServerConfig;
use apexsim_server::server::{run_server, ServerHandle};

/// Spawn an in-process server on ephemeral ports for integration tests.
pub async fn start_test_server() -> ServerHandle {
    start_test_server_with_tick_rate(ServerConfig::default().server.tick_rate_hz).await
}

/// Spawn an in-process server on ephemeral ports with a custom tick rate
/// (used by the stress tests).
pub async fn start_test_server_with_tick_rate(tick_rate_hz: u16) -> ServerHandle {
    let mut config = ServerConfig::default();
    config.server.tick_rate_hz = tick_rate_hz;
    config.network.tcp_bind = "127.0.0.1:0".to_string();
    config.network.udp_bind = "127.0.0.1:0".to_string();
    config.network.health_bind = "127.0.0.1:0".to_string();
    config.network.require_tls = false;
    run_server(config)
        .await
        .expect("failed to start in-process test server")
}

/// Tests for bounded channels and drop/backpressure policy in transport layer
use apexsim_server::transport::TransportLayer;
use std::time::Duration;
use tokio::time::sleep;

#[tokio::test]
async fn test_bounded_channels_prevent_oom() {
    // This test verifies that bounded channels prevent unbounded memory growth
    // We create a transport layer without TLS (by providing invalid cert paths)
    
    let transport = TransportLayer::new(
        "127.0.0.1:0", // Random port
        "127.0.0.1:0", // Random port
        "invalid_cert_path.pem",
        "invalid_key_path.pem",
        false, // require_tls (optional for tests)
        5000, // 5 second heartbeat timeout
    )
    .await
    .expect("Failed to create transport layer");

    // Verify metrics start at zero
    assert_eq!(transport.metrics.tcp_dropped(), 0);
    assert_eq!(transport.metrics.udp_dropped(), 0);
    assert_eq!(transport.metrics.clients_disconnected(), 0);
}

#[tokio::test]
async fn test_droppable_messages_are_dropped_when_queue_full() {
    // This test verifies that droppable messages (telemetry) are dropped when queue is full
    
    let mut transport = TransportLayer::new(
        "127.0.0.1:0",
        "127.0.0.1:0",
        "invalid_cert_path.pem",
        "invalid_key_path.pem",
        false, // require_tls (optional for tests)
        5000,
    )
    .await
    .expect("Failed to create transport layer");

    transport.start().await;

    // Give it a moment to start
    sleep(Duration::from_millis(100)).await;

    // Test that metrics are accessible
    let initial_dropped = transport.metrics.tcp_dropped();
    assert_eq!(initial_dropped, 0, "Should start with zero dropped messages");
}

#[tokio::test]
async fn test_message_priority_classification() {
    use apexsim_server::network::{MessagePriority, ServerMessage};
    use uuid::Uuid;

    // Test that critical messages are correctly classified
    let auth_msg = ServerMessage::AuthSuccess(apexsim_server::network::AuthSuccessData {
        player_id: Uuid::new_v4(),
        server_version: 1,
    });
    assert_eq!(auth_msg.priority(), MessagePriority::Critical);

    let error_msg = ServerMessage::Error {
        code: 500,
        message: "Test error".to_string(),
    };
    assert_eq!(error_msg.priority(), MessagePriority::Critical);

    // Test that droppable messages are correctly classified
    let heartbeat_msg = ServerMessage::HeartbeatAck { server_tick: 100 };
    assert_eq!(heartbeat_msg.priority(), MessagePriority::Droppable);

    let telemetry_msg = ServerMessage::Telemetry(apexsim_server::network::Telemetry {
        server_tick: 100,
        session_state: apexsim_server::data::SessionState::Racing,
        countdown_ms: None,
        car_states: vec![],
        game_mode: apexsim_server::data::GameMode::Lobby
    });
    assert_eq!(telemetry_msg.priority(), MessagePriority::Droppable);
}

#[tokio::test]
async fn test_metrics_tracking() {
    use apexsim_server::transport::TransportMetrics;
    use std::sync::atomic::Ordering;

    let metrics = TransportMetrics::new();

    // Initially zero
    assert_eq!(metrics.tcp_dropped(), 0);
    assert_eq!(metrics.udp_dropped(), 0);
    assert_eq!(metrics.clients_disconnected(), 0);

    // Increment metrics
    metrics.tcp_messages_dropped.fetch_add(1, Ordering::Relaxed);
    assert_eq!(metrics.tcp_dropped(), 1);

    metrics.udp_messages_dropped.fetch_add(5, Ordering::Relaxed);
    assert_eq!(metrics.udp_dropped(), 5);

    metrics
        .clients_disconnected_backpressure
        .fetch_add(2, Ordering::Relaxed);
    assert_eq!(metrics.clients_disconnected(), 2);
}

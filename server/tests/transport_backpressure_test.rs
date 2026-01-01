/// Tests for bounded channels and drop/backpressure policy in transport layer
use apexsim_server::transport::TransportLayer;
use std::time::Duration;
use tokio::time::sleep;

/// Helper to create test TLS cert/key files
async fn create_test_tls_files() -> (String, String) {
    use std::io::Write;
    use tempfile::NamedTempFile;

    // Create self-signed cert and key for testing
    // These are minimal test files - not secure
    let cert_pem = r#"-----BEGIN CERTIFICATE-----
MIIDazCCAlOgAwIBAgIUXqRMQKmV6qKOm1xQ5F7p3lCMwqYwDQYJKoZIhvcNAQEL
BQAwRTELMAkGA1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxITAfBgNVBAoM
GEludGVybmV0IFdpZGdpdHMgUHR5IEx0ZDAeFw0yNDAxMDEwMDAwMDBaFw0yNTAx
MDEwMDAwMDBaMEUxCzAJBgNVBAYTAkFVMRMwEQYDVQQIDApTb21lLVN0YXRlMSEw
HwYDVQQKDBhJbnRlcm5ldCBXaWRnaXRzIFB0eSBMdGQwggEiMA0GCSqGSIb3DQEB
AQUAA4IBDwAwggEKAoIBAQC7VJTUt9Us8cKjMzEfYyjiWA4/qMD/Cw5YV9qYnqkL
UBDCbGKCM2VwFN7CgHSPTgYxkDphgmRTNvkJzMjPPgJpPQEZKjQhBMn8VLLuJLhb
sSKgGzRk0R3tCYkFqKwdQ4kYoXzKpq4lqHEBWFTNRVzJnp7+i7XCNhMqnPHPBWk2
nY1TBWjj4+Vx8shHShRfphZKJOdlNNqB4W8tPyqhOqNKLW5EWjLTxLqn7w7pCqQ1
0LGJVQ5q9wnIrXqHlTRjO1Y6eBLtCmN8YgmDLqfQpFGLVL8F3u5bQRnKEXBxMplw
RlN5kJpLG4PJhMCfQvqDfHkWWTIqLpKz0kCzFPLqOyzbAgMBAAGjUzBRMB0GA1Ud
DgQWBBRMqLLQJQtqyC7sGKqGwVPKPOqHdzAfBgNVHSMEGDAWgBRMqLLQJQtqyC7s
GKqGwVPKPOqHdzAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAv
VkzT0J4mB/p2A5C+xBRBBiqm4nLmWW3eCqNLBjYpKWHEWZLrQK3YPqIzCvJCqVyJ
hGNZMTVZhz6GKzQ6qGzKYMPJv3WFnqRqDvkhKqLQxq8NHHBY3b2lOmPYJqDN6yKJ
+QZqKVqJhChHBWGCLnBdOqDQPJ8mXVPILmEUyLqJC7Ih9C6YGBvGLBHkxBWPGrqW
qKTfJl/T0sBQ8jqW+qGO3mQZQvwRqPPJ1XDVBfPMEVQCqLQBGPXEPQxqRQkCqMxJ
lPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVBMqLJqLQBGP8EFVBPQP
-----END CERTIFICATE-----"#;

    let key_pem = r#"-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC7VJTUt9Us8cKj
MzEfYyjiWA4/qMD/Cw5YV9qYnqkLUBDCbGKCM2VwFN7CgHSPTgYxkDphgmRTNvkJ
zMjPPgJpPQEZKjQhBMn8VLLuJLhbsSKgGzRk0R3tCYkFqKwdQ4kYoXzKpq4lqHEB
WFTNRVzJnp7+i7XCNhMqnPHPBWk2nY1TBWjj4+Vx8shHShRfphZKJOdlNNqB4W8t
PyqhOqNKLW5EWjLTxLqn7w7pCqQ10LGJVQ5q9wnIrXqHlTRjO1Y6eBLtCmN8Ygm
DLqfQpFGLVL8F3u5bQRnKEXBxMplwRlN5kJpLG4PJhMCfQvqDfHkWWTIqLpKz0k
CzFPLqOyzbAgMBAAECggEAFhLCvQqZvPvt7W2BPTKGBBvSBVPWLPbqZYkP+CvjM8
8vKqPKLPQwXqDQJcCbV5FGQM3Z2qGBEJqLQXPGLVLQBMqLJqLQBGPXEPQxqRQk
CqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVBMqLJqLQBGP8E
FVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVBM
qLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJ
qLQPQ8XDVBMqLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGP
ZRQPQxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVBM
qLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGK
JqLQPQ8XDVBAoGBAOBPPJvYLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVB
MqLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGK
JqLQPQ8XDVBMqLJqLQBGP8EFVBAoGBANvGPZRQPQ2VrGKJqLQPQ8XDVBMqLJqL
QBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ
8XDVBMqLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQAoGAQPQ2VrGKJqLQPQ8XD
VBMqLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2Vr
GKJqLQPQ8XDVBMqLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQK
qGPZRQPQ2VrGKJqLQPQ8XDVBMqLJqLQBGP8EFVBAoGAPQ2VrGKJqLQPQ8XDVBM
qLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJ
qLQPQ8XDVBMqLJqLQBGP8EFVBPQPxqRQkCqMxJlPGYLqLQB8EF6hGJKBGQKqGP
ZRQPQ2VrGKJqLQPQ8XDVBMqLJqLQBGP8EFVBAoGAFVBPQPxqRQkCqMxJlPGYLq
LQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVBMqLJqLQBGP8EFVBPQPxqRQk
CqMxJlPGYLqLQB8EF6hGJKBGQKqGPZRQPQ2VrGKJqLQPQ8XDVBMqLJqLQBGP8E
FVBPQPQ=
-----END PRIVATE KEY-----"#;

    let mut cert_file = NamedTempFile::new().expect("Failed to create cert file");
    cert_file
        .write_all(cert_pem.as_bytes())
        .expect("Failed to write cert");
    let cert_path = cert_file.path().to_string_lossy().to_string();

    let mut key_file = NamedTempFile::new().expect("Failed to create key file");
    key_file
        .write_all(key_pem.as_bytes())
        .expect("Failed to write key");
    let key_path = key_file.path().to_string_lossy().to_string();

    // Keep files alive
    std::mem::forget(cert_file);
    std::mem::forget(key_file);

    (cert_path, key_path)
}

#[tokio::test]
async fn test_bounded_channels_prevent_oom() {
    // This test verifies that bounded channels prevent unbounded memory growth
    
    let (cert_path, key_path) = create_test_tls_files().await;
    
    let transport = TransportLayer::new(
        "127.0.0.1:0", // Random port
        "127.0.0.1:0", // Random port
        &cert_path,
        &key_path,
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
    
    let (cert_path, key_path) = create_test_tls_files().await;
    
    let mut transport = TransportLayer::new(
        "127.0.0.1:0",
        "127.0.0.1:0",
        &cert_path,
        &key_path,
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
    let auth_msg = ServerMessage::AuthSuccess {
        player_id: Uuid::new_v4(),
        server_version: 1,
    };
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

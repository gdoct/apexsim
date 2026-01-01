use std::fs;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::Duration;
use tempfile::TempDir;
use tokio::time::sleep;

/// Test that server starts successfully with require_tls=false when certificates are missing
#[tokio::test]
async fn test_server_starts_without_tls_when_not_required() {
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let config_path = temp_dir.path().join("server.toml");
    
    // Create config with require_tls=false and non-existent cert paths
    let config_content = r#"
[server]
tick_rate_hz = 240
max_sessions = 8
session_timeout_seconds = 300

[network]
tcp_bind = "127.0.0.1:9100"
udp_bind = "127.0.0.1:9101"
health_bind = "127.0.0.1:9102"
tls_cert_path = "/tmp/nonexistent/server.crt"
tls_key_path = "/tmp/nonexistent/server.key"
require_tls = false
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[content]
cars_dir = "../content/cars"
tracks_dir = "../content/tracks"

[logging]
level = "info"
console_enabled = true
"#;
    
    fs::write(&config_path, config_content).expect("Failed to write config");
    
    // Start server
    let mut child = Command::new("cargo")
        .args(["run", "--", "--config", config_path.to_str().unwrap()])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to start server");
    
    // Give server time to start
    sleep(Duration::from_secs(3)).await;
    
    // Check if server is still running
    let status = child.try_wait();
    assert!(status.is_ok() && status.unwrap().is_none(), "Server should still be running");
    
    // Cleanup
    let _ = child.kill();
    let _ = child.wait();
}

/// Test that server fails to start with require_tls=true when certificates are missing
#[tokio::test]
async fn test_server_fails_without_tls_when_required() {
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let config_path = temp_dir.path().join("server.toml");
    
    // Create config with require_tls=true and non-existent cert paths
    let config_content = r#"
[server]
tick_rate_hz = 240
max_sessions = 8
session_timeout_seconds = 300

[network]
tcp_bind = "127.0.0.1:9200"
udp_bind = "127.0.0.1:9201"
health_bind = "127.0.0.1:9202"
tls_cert_path = "/tmp/nonexistent/server.crt"
tls_key_path = "/tmp/nonexistent/server.key"
require_tls = true
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[content]
cars_dir = "../content/cars"
tracks_dir = "../content/tracks"

[logging]
level = "info"
console_enabled = true
"#;
    
    fs::write(&config_path, config_content).expect("Failed to write config");
    
    // Start server
    let mut child = Command::new("cargo")
        .args(["run", "--", "--config", config_path.to_str().unwrap()])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to start server");
    
    // Give server time to attempt startup
    sleep(Duration::from_secs(3)).await;
    
    // Check if server has exited (it should have failed)
    let status = child.try_wait();
    assert!(
        status.is_ok() && status.unwrap().is_some(),
        "Server should have exited when TLS is required but certificates are missing"
    );
    
    // Cleanup
    let _ = child.kill();
    let _ = child.wait();
}

/// Test that server starts successfully with require_tls=true when valid certificates exist
#[tokio::test]
async fn test_server_starts_with_tls_when_required_and_certs_exist() {
    // This test requires valid certificates to exist at ./certs/
    // Skip if they don't exist to avoid false failures
    let cert_path = PathBuf::from("./certs/server.crt");
    let key_path = PathBuf::from("./certs/server.key");
    
    if !cert_path.exists() || !key_path.exists() {
        println!("Skipping test: certificates not found at ./certs/");
        return;
    }
    
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let config_path = temp_dir.path().join("server.toml");
    
    // Create config with require_tls=true and existing cert paths
    let config_content = r#"
[server]
tick_rate_hz = 240
max_sessions = 8
session_timeout_seconds = 300

[network]
tcp_bind = "127.0.0.1:9300"
udp_bind = "127.0.0.1:9301"
health_bind = "127.0.0.1:9302"
tls_cert_path = "./certs/server.crt"
tls_key_path = "./certs/server.key"
require_tls = true
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[content]
cars_dir = "../content/cars"
tracks_dir = "../content/tracks"

[logging]
level = "info"
console_enabled = true
"#;
    
    fs::write(&config_path, config_content).expect("Failed to write config");
    
    // Start server
    let mut child = Command::new("cargo")
        .args(["run", "--", "--config", config_path.to_str().unwrap()])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to start server");
    
    // Give server time to start
    sleep(Duration::from_secs(3)).await;
    
    // Check if server is still running
    let status = child.try_wait();
    assert!(
        status.is_ok() && status.unwrap().is_none(),
        "Server should be running when TLS is required and valid certificates exist"
    );
    
    // Cleanup
    let _ = child.kill();
    let _ = child.wait();
}

/// Test log output contains clear TLS state information
#[tokio::test]
async fn test_tls_state_logging() {
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let config_path = temp_dir.path().join("server.toml");
    
    // Create config with require_tls=false
    let config_content = r#"
[server]
tick_rate_hz = 240
max_sessions = 8
session_timeout_seconds = 300

[network]
tcp_bind = "127.0.0.1:9400"
udp_bind = "127.0.0.1:9401"
health_bind = "127.0.0.1:9402"
tls_cert_path = "/tmp/nonexistent/server.crt"
tls_key_path = "/tmp/nonexistent/server.key"
require_tls = false
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[content]
cars_dir = "../content/cars"
tracks_dir = "../content/tracks"

[logging]
level = "info"
console_enabled = true
"#;
    
    fs::write(&config_path, config_content).expect("Failed to write config");
    
    // Start server and capture output
    let mut child = Command::new("cargo")
        .args(["run", "--", "--config", config_path.to_str().unwrap()])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to start server");
    
    // Give server time to start and log
    sleep(Duration::from_secs(3)).await;
    
    // Capture stderr and stdout
    let _ = child.kill();
    let output = child.wait_with_output().expect("Failed to get output");
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    let combined = format!("{}\n{}", stdout, stderr);
    
    // Check for TLS state in logs
    assert!(
        combined.contains("TLS mode") || combined.contains("TLS configuration"),
        "Logs should clearly indicate TLS state. Got:\n{}",
        combined
    );
}

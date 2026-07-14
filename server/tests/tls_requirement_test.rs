//! TLS enforcement tests, run in-process against `TransportLayer::new`
//! (the function that implements the require_tls policy).

use apexsim_server::config::AuthSettings;
use apexsim_server::transport::TransportLayer;
use std::path::PathBuf;

/// require_tls=false with missing certificates: the transport starts in
/// plaintext mode (explicit development opt-out).
#[tokio::test]
async fn test_transport_starts_without_tls_when_not_required() {
    let transport = TransportLayer::new(
        "127.0.0.1:0",
        "127.0.0.1:0",
        "/tmp/nonexistent/server.crt",
        "/tmp/nonexistent/server.key",
        false,
        5000,
        AuthSettings::default(),
    )
    .await;

    assert!(
        transport.is_ok(),
        "transport should start in plaintext mode when TLS is explicitly optional"
    );
}

/// require_tls=true with missing certificates: startup must fail (fail-closed).
#[tokio::test]
async fn test_transport_fails_without_tls_when_required() {
    let transport = TransportLayer::new(
        "127.0.0.1:0",
        "127.0.0.1:0",
        "/tmp/nonexistent/server.crt",
        "/tmp/nonexistent/server.key",
        true,
        5000,
        AuthSettings::default(),
    )
    .await;

    assert!(
        transport.is_err(),
        "transport must refuse to start when TLS is required but certificates are missing"
    );
}

/// require_tls=true with valid certificates: starts with TLS enabled.
/// Skipped when no dev certificates exist at ./certs/.
#[tokio::test]
async fn test_transport_starts_with_tls_when_required_and_certs_exist() {
    let cert_path = PathBuf::from("./certs/server.crt");
    let key_path = PathBuf::from("./certs/server.key");

    if !cert_path.exists() || !key_path.exists() {
        println!("Skipping test: certificates not found at ./certs/");
        return;
    }

    let transport = TransportLayer::new(
        "127.0.0.1:0",
        "127.0.0.1:0",
        cert_path.to_str().unwrap(),
        key_path.to_str().unwrap(),
        true,
        5000,
        AuthSettings::default(),
    )
    .await;

    assert!(
        transport.is_ok(),
        "transport should start when TLS is required and valid certificates exist"
    );
}

/// The secure-by-default config: `ServerConfig::default()` must require TLS,
/// so a server without a config file cannot silently run plaintext.
#[test]
fn test_default_config_requires_tls() {
    let config = apexsim_server::config::ServerConfig::default();
    assert!(
        config.network.require_tls,
        "require_tls must default to true (fail closed)"
    );
}

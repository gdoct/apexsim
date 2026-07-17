//! Authentication and pre-auth gating tests against an in-process server.

mod common;

use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::timeout;

use apexsim_server::config::{AuthMode, ServerConfig};
use apexsim_server::network::{ClientMessage, ServerMessage};
use apexsim_server::server::run_server;

async fn start_token_server(tokens: Vec<String>) -> apexsim_server::server::ServerHandle {
    let mut config = ServerConfig::default();
    config.network.tcp_bind = "127.0.0.1:0".to_string();
    config.network.udp_bind = "127.0.0.1:0".to_string();
    config.network.health_bind = "127.0.0.1:0".to_string();
    config.network.require_tls = false;
    config.auth.mode = AuthMode::Token;
    config.auth.tokens = tokens;
    run_server(config).await.expect("server should start")
}

async fn send_msg(stream: &mut TcpStream, msg: &ClientMessage) {
    let data = rmp_serde::to_vec_named(msg).unwrap();
    stream
        .write_all(&(data.len() as u32).to_be_bytes())
        .await
        .unwrap();
    stream.write_all(&data).await.unwrap();
    stream.flush().await.unwrap();
}

async fn recv_msg(stream: &mut TcpStream) -> Option<ServerMessage> {
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await.ok()?;
    let len = u32::from_be_bytes(len_buf) as usize;
    let mut buf = vec![0u8; len];
    stream.read_exact(&mut buf).await.ok()?;
    rmp_serde::from_slice(&buf).ok()
}

#[tokio::test]
async fn test_valid_token_accepted() {
    let server = start_token_server(vec!["secret123".to_string()]).await;
    let mut stream = TcpStream::connect(server.tcp_addr).await.unwrap();

    send_msg(
        &mut stream,
        &ClientMessage::Authenticate {
            token: "secret123".to_string(),
            player_name: "TokenPlayer".to_string(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
        },
    )
    .await;

    let response = timeout(Duration::from_secs(5), recv_msg(&mut stream))
        .await
        .expect("timed out waiting for auth response")
        .expect("connection closed unexpectedly");
    assert!(
        matches!(response, ServerMessage::AuthSuccess(_)),
        "expected AuthSuccess, got {:?}",
        response
    );
}

#[tokio::test]
async fn test_invalid_token_rejected() {
    let server = start_token_server(vec!["secret123".to_string()]).await;
    let mut stream = TcpStream::connect(server.tcp_addr).await.unwrap();

    send_msg(
        &mut stream,
        &ClientMessage::Authenticate {
            token: "wrong".to_string(),
            player_name: "Imposter".to_string(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
        },
    )
    .await;

    let response = timeout(Duration::from_secs(5), recv_msg(&mut stream))
        .await
        .expect("timed out waiting for auth response");
    match response {
        Some(ServerMessage::AuthFailure { reason }) => {
            assert!(reason.contains("token"), "unexpected reason: {}", reason)
        }
        Some(other) => panic!("expected AuthFailure, got {:?}", other),
        // The server closes the connection after rejecting; already-closed is fine
        None => {}
    }

    // Connection should be closed shortly after rejection
    let closed = timeout(Duration::from_secs(5), async {
        let mut buf = [0u8; 1];
        stream.read(&mut buf).await
    })
    .await;
    assert!(
        matches!(closed, Ok(Ok(0)) | Ok(Err(_))),
        "connection should be closed after auth rejection"
    );
}

#[tokio::test]
async fn test_empty_token_rejected_in_token_mode() {
    let server = start_token_server(vec!["secret123".to_string()]).await;
    let mut stream = TcpStream::connect(server.tcp_addr).await.unwrap();

    send_msg(
        &mut stream,
        &ClientMessage::Authenticate {
            token: String::new(),
            player_name: "NoToken".to_string(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
        },
    )
    .await;

    let response = timeout(Duration::from_secs(5), recv_msg(&mut stream))
        .await
        .expect("timed out waiting for auth response");
    assert!(
        matches!(response, Some(ServerMessage::AuthFailure { .. }) | None),
        "empty token must not authenticate, got {:?}",
        response
    );
}

#[tokio::test]
async fn test_pre_auth_messages_dropped() {
    // Dev-mode server (any token works), but messages before Authenticate
    // must not be processed.
    let server = common::start_test_server().await;
    let mut stream = TcpStream::connect(server.tcp_addr).await.unwrap();

    // Send a control message without authenticating first
    send_msg(&mut stream, &ClientMessage::RequestLobbyState).await;

    // The server must not answer with lobby state; give it a moment
    let response = timeout(Duration::from_millis(500), recv_msg(&mut stream)).await;
    assert!(
        !matches!(response, Ok(Some(ServerMessage::LobbyState(_)))),
        "server answered a pre-auth request"
    );

    // Authenticating afterwards still works
    send_msg(
        &mut stream,
        &ClientMessage::Authenticate {
            token: "anything".to_string(),
            player_name: "LateAuth".to_string(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
        },
    )
    .await;
    let response = timeout(Duration::from_secs(5), recv_msg(&mut stream))
        .await
        .expect("timed out waiting for auth response")
        .expect("connection closed unexpectedly");
    assert!(matches!(response, ServerMessage::AuthSuccess(_)));
}

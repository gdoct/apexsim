//! A driver profile's key: the id counts once the key's proof arrives; the
//! first key a server sees keeps the id, and another key, or none, drives
//! without it.

use crate::common;

use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{sleep, timeout};

use apexsim_server::network::{ClientMessage, ServerMessage};
use ring::signature::KeyPair;

const TEST_TIMEOUT: Duration = Duration::from_secs(30);

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

async fn send(stream: &mut TcpStream, msg: &ClientMessage) {
    let data = rmp_serde::to_vec_named(msg).unwrap();
    stream
        .write_all(&(data.len() as u32).to_be_bytes())
        .await
        .unwrap();
    stream.write_all(&data).await.unwrap();
    stream.flush().await.unwrap();
}

async fn recv(stream: &mut TcpStream) -> Option<ServerMessage> {
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await.ok()?;
    let mut buf = vec![0u8; u32::from_be_bytes(len_buf) as usize];
    stream.read_exact(&mut buf).await.ok()?;
    rmp_serde::from_slice(&buf).ok()
}

/// Connect as `name` with a driver id and maybe a key; sign the challenge
/// with `signer` when given. Returns the stream and any notice the server
/// sent within a moment of the handshake.
async fn connect(
    addr: std::net::SocketAddr,
    name: &str,
    driver_id: uuid::Uuid,
    key: Option<&ring::signature::Ed25519KeyPair>,
    signer: Option<&ring::signature::Ed25519KeyPair>,
) -> (TcpStream, Vec<String>) {
    let mut stream = TcpStream::connect(addr).await.unwrap();
    send(
        &mut stream,
        &ClientMessage::Authenticate {
            token: "dev".into(),
            player_name: name.into(),
            protocol_version: apexsim_server::network::PROTOCOL_VERSION,
            resume_token: None,
            driver_id: Some(driver_id),
            driver_key: key.map(|k| hex(k.public_key().as_ref())),
        },
    )
    .await;
    let mut notices = Vec::new();
    let challenge = loop {
        match timeout(Duration::from_secs(5), recv(&mut stream)).await {
            Ok(Some(ServerMessage::AuthSuccess(data))) => break data.key_challenge,
            Ok(Some(ServerMessage::ServerNotice { message })) => notices.push(message),
            Ok(Some(_)) => continue,
            other => panic!("no AuthSuccess: {other:?}"),
        }
    };
    assert_eq!(
        challenge.is_empty(),
        key.is_none(),
        "a challenge only for a key"
    );
    if let Some(signer) = signer {
        let message = apexsim_server::driver_keys::proof_message(&challenge, driver_id);
        send(
            &mut stream,
            &ClientMessage::ProveDriverKey {
                signature: hex(signer.sign(&message).as_ref()),
            },
        )
        .await;
    }
    // Whatever the server says about the id comes before this deadline.
    let until = tokio::time::Instant::now() + Duration::from_millis(400);
    while let Ok(Some(msg)) = timeout(until - tokio::time::Instant::now(), recv(&mut stream)).await
    {
        if let ServerMessage::ServerNotice { message } = msg {
            notices.push(message);
        }
    }
    (stream, notices)
}

async fn driver_of(
    server: &apexsim_server::server::ServerHandle,
    name: &str,
) -> Option<uuid::Uuid> {
    sleep(Duration::from_millis(50)).await;
    server
        .transport
        .read()
        .await
        .connection_snapshots()
        .await
        .into_iter()
        .find(|c| c.name == name)
        .and_then(|c| c.driver_id)
}

#[tokio::test]
async fn a_driver_key_proves_the_id_and_keeps_it() {
    let result = timeout(TEST_TIMEOUT, async {
        let records_dir =
            std::env::temp_dir().join(format!("apexsim-keys-{}", uuid::Uuid::new_v4()));
        let server = common::start_test_server_with_records(&records_dir).await;
        let id = uuid::Uuid::from_u128(0xd1);
        let mine = ring::signature::Ed25519KeyPair::from_seed_unchecked(&[3; 32]).unwrap();
        let theirs = ring::signature::Ed25519KeyPair::from_seed_unchecked(&[4; 32]).unwrap();

        // The first key proves the id and keeps it.
        let (_a, notices) = connect(server.tcp_addr, "Ana", id, Some(&mine), Some(&mine)).await;
        assert!(notices.is_empty(), "{notices:?}");
        assert_eq!(driver_of(&server, "Ana").await, Some(id));

        // Another key with the same id: proved, but not the id's.
        let (_b, notices) = connect(server.tcp_addr, "Bea", id, Some(&theirs), Some(&theirs)).await;
        assert_eq!(notices.len(), 1, "{notices:?}");
        assert_eq!(driver_of(&server, "Bea").await, None);

        // A forged proof: the right key sent, signed with another.
        let (_c, notices) = connect(server.tcp_addr, "Cy", id, Some(&mine), Some(&theirs)).await;
        assert_eq!(notices.len(), 1, "{notices:?}");
        assert_eq!(driver_of(&server, "Cy").await, None);

        // The id with no key at all, once a key has it.
        let (_d, notices) = connect(server.tcp_addr, "Di", id, None, None).await;
        assert_eq!(notices.len(), 1, "{notices:?}");
        assert_eq!(driver_of(&server, "Di").await, None);

        // An id no key has yet is still taken on its word.
        let free = uuid::Uuid::from_u128(0xd2);
        let (_e, notices) = connect(server.tcp_addr, "Ed", free, None, None).await;
        assert!(notices.is_empty(), "{notices:?}");
        assert_eq!(driver_of(&server, "Ed").await, Some(free));

        // The binding outlives the server: it is in drivers.json.
        let text = std::fs::read_to_string(records_dir.join("drivers.json")).unwrap();
        assert!(text.contains(&hex(mine.public_key().as_ref())));
        let _ = std::fs::remove_dir_all(&records_dir);
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .await;
    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => panic!("test failed: {e}"),
        Err(_) => panic!("test timed out"),
    }
}

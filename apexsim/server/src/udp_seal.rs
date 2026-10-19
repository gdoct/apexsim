//! Per-datagram authentication of inbound UDP.
//!
//! A UDP datagram's identity on the server is the bound source address, and
//! a source address is trivially forged on a LAN or from an ISP without
//! ingress filtering. So every inbound datagram, the handshake included, is
//! *sealed*: the client tags it with a MAC under a key that only travelled
//! over the TLS connection, and a sequence number that must rise. The server
//! drops a datagram whose tag does not verify or whose sequence number is
//! not above the last one accepted, so a forged, tampered or replayed
//! datagram never reaches the game loop. Outbound datagrams (telemetry,
//! feedback, spectator frames) stay plain: nothing in them is secret and the
//! client is a puppet of whatever it is told anyway.
//!
//! Layout of a sealed datagram:
//!
//! ```text
//! [0]        0xC1   never a valid MessagePack first byte, so a bare
//!                   (pre-sealing) datagram is told apart at once
//! [1..9]     seq    u64, big-endian; strictly increasing per key
//! [9..25]    tag    HMAC-SHA1(key, seq ‖ payload), first 16 bytes
//! [25..]     payload  the MessagePack `ClientMessage` as before
//! ```
//!
//! The key is the UTF-8 bytes of `AuthSuccess.udp_key`, a hex string, so
//! the client needs no decoding. HMAC-SHA1 is used because the Unreal client
//! has `FSHA1::HMACBuffer` in Core and nothing newer; a 16-byte truncation
//! of it is far beyond what forging a steering input is worth.

use ring::hmac;
use std::sync::atomic::{AtomicU64, Ordering};

/// The leading byte of a sealed datagram.
pub const SEAL_MARKER: u8 = 0xC1;
/// Bytes of the tag kept on the wire.
pub const TAG_LEN: usize = 16;
/// Bytes before the payload: marker, sequence number, tag.
pub const HEADER_LEN: usize = 1 + 8 + TAG_LEN;

/// Compares two tags without an early exit on the first differing byte,
/// so a forger learns nothing from the server's timing.
fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// A fresh `udp_key` for `AuthSuccess`: 32 random bytes as lowercase hex.
pub fn generate_key() -> String {
    use ring::rand::SecureRandom;
    let mut bytes = [0u8; 32];
    ring::rand::SystemRandom::new()
        .fill(&mut bytes)
        .expect("system randomness");
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn hmac_key(key: &str) -> hmac::Key {
    hmac::Key::new(hmac::HMAC_SHA1_FOR_LEGACY_USE_ONLY, key.as_bytes())
}

fn tag(key: &hmac::Key, seq: u64, payload: &[u8]) -> [u8; TAG_LEN] {
    let mut ctx = hmac::Context::with_key(key);
    ctx.update(&seq.to_be_bytes());
    ctx.update(payload);
    let full = ctx.sign();
    let mut out = [0u8; TAG_LEN];
    out.copy_from_slice(&full.as_ref()[..TAG_LEN]);
    out
}

/// Builds a sealed datagram: what a client sends.
pub fn seal(key: &str, seq: u64, payload: &[u8]) -> Vec<u8> {
    let key = hmac_key(key);
    let mut out = Vec::with_capacity(HEADER_LEN + payload.len());
    out.push(SEAL_MARKER);
    out.extend_from_slice(&seq.to_be_bytes());
    out.extend_from_slice(&tag(&key, seq, payload));
    out.extend_from_slice(payload);
    out
}

/// Why a datagram was refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reject {
    /// Not a sealed datagram (too short or no marker): a client from before
    /// sealing, or noise.
    Unsealed,
    /// The tag does not verify: wrong key or tampered bytes.
    BadTag,
    /// The sequence number is not above the last one accepted: a replay or
    /// a datagram overtaken by a newer one.
    Replayed,
}

/// Splits a sealed datagram without checking it: the sequence number, the
/// tag and the payload. `Err(Unsealed)` when it is not one.
pub fn split(datagram: &[u8]) -> Result<(u64, &[u8], &[u8]), Reject> {
    if datagram.len() < HEADER_LEN || datagram[0] != SEAL_MARKER {
        return Err(Reject::Unsealed);
    }
    let mut seq = [0u8; 8];
    seq.copy_from_slice(&datagram[1..9]);
    Ok((
        u64::from_be_bytes(seq),
        &datagram[9..HEADER_LEN],
        &datagram[HEADER_LEN..],
    ))
}

/// The server's side of one client's seal: its key and the highest sequence
/// number accepted so far. Shared between the connection registry and the
/// UDP receiver.
#[derive(Debug)]
pub struct Inbound {
    key: hmac::Key,
    last_seq: AtomicU64,
}

impl Inbound {
    pub fn new(key: &str) -> Self {
        Self {
            key: hmac_key(key),
            last_seq: AtomicU64::new(0),
        }
    }

    /// Verifies a datagram already split by [`split`] and, when it is
    /// genuine and newer than anything accepted before, records its
    /// sequence number. The tag is checked before the sequence number so a
    /// forger cannot push the counter.
    pub fn accept(&self, seq: u64, tag_bytes: &[u8], payload: &[u8]) -> Result<(), Reject> {
        let expected = tag(&self.key, seq, payload);
        if !constant_time_eq(&expected, tag_bytes) {
            return Err(Reject::BadTag);
        }
        // Single receiver task, but a CAS keeps it right regardless.
        let mut last = self.last_seq.load(Ordering::Relaxed);
        loop {
            if seq <= last {
                return Err(Reject::Replayed);
            }
            match self.last_seq.compare_exchange_weak(
                last,
                seq,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => return Ok(()),
                Err(now) => last = now,
            }
        }
    }

    /// Verifies and accepts a whole datagram, returning its payload.
    pub fn open<'a>(&self, datagram: &'a [u8]) -> Result<&'a [u8], Reject> {
        let (seq, tag_bytes, payload) = split(datagram)?;
        self.accept(seq, tag_bytes, payload)?;
        Ok(payload)
    }

    /// The highest sequence number accepted so far (0 before any).
    pub fn last_seq(&self) -> u64 {
        self.last_seq.load(Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: &str = "udp-key";

    #[test]
    fn seal_then_open_roundtrips_and_counts() {
        let inbound = Inbound::new(KEY);
        let d1 = seal(KEY, 1, b"first");
        let d2 = seal(KEY, 2, b"second");
        assert_eq!(inbound.open(&d1).unwrap(), b"first");
        assert_eq!(inbound.open(&d2).unwrap(), b"second");
        assert_eq!(inbound.last_seq(), 2);
    }

    #[test]
    fn replay_and_reorder_are_refused() {
        let inbound = Inbound::new(KEY);
        let d1 = seal(KEY, 5, b"a");
        let d2 = seal(KEY, 6, b"b");
        assert!(inbound.open(&d2).is_ok());
        assert_eq!(inbound.open(&d1), Err(Reject::Replayed));
        assert_eq!(inbound.open(&d2), Err(Reject::Replayed));
        assert_eq!(inbound.last_seq(), 6);
    }

    #[test]
    fn wrong_key_tamper_and_bare_are_refused_without_moving_the_counter() {
        let inbound = Inbound::new(KEY);
        assert_eq!(
            inbound.open(&seal("other-key", 1, b"a")),
            Err(Reject::BadTag)
        );
        let mut tampered = seal(KEY, 1, b"a");
        *tampered.last_mut().unwrap() ^= 1;
        assert_eq!(inbound.open(&tampered), Err(Reject::BadTag));
        let mut reseq = seal(KEY, 1, b"a");
        reseq[8] = 9; // seq edited without re-tagging
        assert_eq!(inbound.open(&reseq), Err(Reject::BadTag));
        // A bare MessagePack map (what a pre-sealing client sends).
        assert_eq!(inbound.open(&[0x82, 0xA4, 0x74]), Err(Reject::Unsealed));
        assert_eq!(inbound.open(&[]), Err(Reject::Unsealed));
        assert_eq!(inbound.last_seq(), 0);
        // The genuine one still goes through afterwards.
        assert!(inbound.open(&seal(KEY, 1, b"a")).is_ok());
    }

    #[test]
    fn generated_keys_are_hex_and_distinct() {
        let a = generate_key();
        let b = generate_key();
        assert_eq!(a.len(), 64);
        assert!(a.bytes().all(|c| c.is_ascii_hexdigit()));
        assert_ne!(a, b);
    }

    /// Golden bytes for the client (`ApexUdpGoldenBlobs.h`,
    /// `C_SealedUdpHandshake`): the `UdpHandshake` blob for token "udp-tok"
    /// sealed under key "udp-key" with sequence number 7. Print with
    /// `cargo test udp_seal_wire_format -- --nocapture`.
    #[test]
    fn udp_seal_wire_format() {
        let payload = rmp_serde::to_vec_named(&crate::network::ClientMessage::UdpHandshake {
            token: "udp-tok".to_string(),
        })
        .unwrap();
        let sealed = seal(KEY, 7, &payload);
        assert_eq!(sealed[0], SEAL_MARKER);
        assert_eq!(&sealed[1..9], &7u64.to_be_bytes());
        assert_eq!(&sealed[HEADER_LEN..], &payload[..]);
        println!("inline constexpr uint8 C_SealedUdpHandshake[] = {{");
        for chunk in sealed.chunks(16) {
            let row: Vec<String> = chunk.iter().map(|b| format!("0x{b:02X}")).collect();
            println!("\t\t{},", row.join(", "));
        }
        println!("}};");
        // HMAC-SHA1 pinned against an independent computation so the client's
        // FSHA1::HMACBuffer and ring cannot drift apart unnoticed.
        let expected_tag = {
            let k = hmac::Key::new(hmac::HMAC_SHA1_FOR_LEGACY_USE_ONLY, KEY.as_bytes());
            let mut msg = 7u64.to_be_bytes().to_vec();
            msg.extend_from_slice(&payload);
            hmac::sign(&k, &msg)
        };
        assert_eq!(&sealed[9..HEADER_LEN], &expected_tag.as_ref()[..TAG_LEN]);
    }
}

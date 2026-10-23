//! Driver keys: what makes a driver profile's id (`Authenticate.driver_id`)
//! a proof rather than a claim.
//!
//! Each profile holds an Ed25519 key pair made on the player's machine
//! and never sent; the public half travels in `Authenticate.driver_key`.
//! The server answers with a random `AuthSuccess.key_challenge`, and the
//! client signs [`proof_message`] with its private key in
//! `ProveDriverKey`. Only then does the connection drive under the id.
//!
//! The first key a server sees for an id is that id's from then on (trust
//! on first use, `drivers.json` beside the records): a later connection
//! claiming the id with another key, or with none, drives without it, as a
//! client from before profiles does. No accounts and no service: a lost
//! install is a new driver.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{Arc, RwLock};
use tracing::warn;
use uuid::Uuid;

/// What the signature covers, so a proof for one server's challenge and one
/// id is good for nothing else.
pub fn proof_message(challenge: &str, driver_id: Uuid) -> Vec<u8> {
    format!("apexsim driver key v1:{challenge}:{driver_id}").into_bytes()
}

/// A fresh challenge: 32 random bytes as hex.
pub fn challenge() -> String {
    crate::udp_seal::generate_key()
}

fn unhex(text: &str) -> Option<Vec<u8>> {
    if !text.len().is_multiple_of(2) {
        return None;
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(text.get(i..i + 2)?, 16).ok())
        .collect()
}

/// Whether `signature` (hex, 64 bytes) is `key`'s (hex, 32 bytes) over
/// [`proof_message`] for this challenge and id.
pub fn verify(key: &str, challenge: &str, driver_id: Uuid, signature: &str) -> bool {
    let (Some(key), Some(signature)) = (unhex(key), unhex(signature)) else {
        return false;
    };
    if key.len() != 32 || signature.len() != 64 {
        return false;
    }
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, key)
        .verify(&proof_message(challenge, driver_id), &signature)
        .is_ok()
}

/// What [`DriverKeys::bind`] found.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Binding {
    /// The id had no key yet: it is this one's now.
    New,
    /// The id's own key.
    Known,
    /// The id belongs to another key on this server.
    Taken,
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct DriversFile {
    version: u32,
    /// Driver id -> its public key, hex.
    keys: BTreeMap<Uuid, String>,
}

#[derive(Default)]
struct Inner {
    keys: BTreeMap<Uuid, String>,
    path: Option<PathBuf>,
}

/// Each driver id's key. Shared, cheap to clone; the default is empty and
/// kept in memory only.
#[derive(Clone, Default)]
pub struct DriverKeys {
    inner: Arc<RwLock<Inner>>,
}

impl DriverKeys {
    /// Load `path` (a missing file is empty; a corrupt one is logged and
    /// treated as empty, like the records) and keep writing there.
    pub fn open(&self, path: impl Into<PathBuf>) {
        let path = path.into();
        let keys = match std::fs::read_to_string(&path) {
            Ok(text) => serde_json::from_str::<DriversFile>(&text)
                .map(|f| f.keys)
                .unwrap_or_else(|e| {
                    warn!("Ignoring unreadable driver keys {}: {}", path.display(), e);
                    BTreeMap::new()
                }),
            Err(_) => BTreeMap::new(),
        };
        let mut inner = self.inner.write().unwrap_or_else(|e| e.into_inner());
        inner.keys = keys;
        inner.path = Some(path);
    }

    /// The key an id is bound to, if any.
    pub fn key_of(&self, driver_id: Uuid) -> Option<String> {
        self.inner
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .keys
            .get(&driver_id)
            .cloned()
    }

    /// Bind `driver_id` to `key` unless another key has it. Call only with a
    /// key whose proof checked out.
    pub fn bind(&self, driver_id: Uuid, key: &str) -> Binding {
        let key = key.to_ascii_lowercase();
        let mut inner = self.inner.write().unwrap_or_else(|e| e.into_inner());
        match inner.keys.get(&driver_id) {
            Some(known) if *known == key => return Binding::Known,
            Some(_) => return Binding::Taken,
            None => {}
        }
        inner.keys.insert(driver_id, key);
        if let Some(path) = inner.path.clone() {
            let file = DriversFile {
                version: 1,
                keys: inner.keys.clone(),
            };
            drop(inner);
            match serde_json::to_vec_pretty(&file) {
                Ok(bytes) => {
                    let tmp = path.with_extension("json.tmp");
                    if let Err(e) =
                        std::fs::write(&tmp, bytes).and_then(|()| std::fs::rename(&tmp, &path))
                    {
                        warn!("Cannot write driver keys {}: {e}", path.display());
                    }
                }
                Err(e) => warn!("Cannot encode driver keys: {e}"),
            }
        }
        Binding::New
    }

    pub fn len(&self) -> usize {
        self.inner
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .keys
            .len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ring::signature::KeyPair;

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    fn key_pair(seed: u8) -> ring::signature::Ed25519KeyPair {
        ring::signature::Ed25519KeyPair::from_seed_unchecked(&[seed; 32]).unwrap()
    }

    #[test]
    fn a_proof_holds_for_its_own_challenge_and_id_only() {
        let pair = key_pair(1);
        let key = hex(pair.public_key().as_ref());
        let id = Uuid::from_u128(7);
        let sig = hex(pair.sign(&proof_message("abcd", id)).as_ref());
        assert!(verify(&key, "abcd", id, &sig));
        assert!(!verify(&key, "abce", id, &sig), "another challenge");
        assert!(
            !verify(&key, "abcd", Uuid::from_u128(8), &sig),
            "another id"
        );
        let other = hex(key_pair(2).public_key().as_ref());
        assert!(!verify(&other, "abcd", id, &sig), "another key");
        assert!(!verify(&key, "abcd", id, "00"), "not a signature");
        assert!(!verify("zz", "abcd", id, &sig), "not a key");
    }

    /// The client's OpenSSL signs the same bytes (Ed25519 is deterministic):
    /// `ApexSim.Net.DriverKey` pins this pair too.
    #[test]
    fn a_known_seed_signs_a_known_proof() {
        let pair = key_pair(3);
        let id = Uuid::from_u128(7);
        let key = hex(pair.public_key().as_ref());
        let sig = hex(pair.sign(&proof_message("abcd", id)).as_ref());
        assert_eq!(
            key,
            "ed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1"
        );
        assert_eq!(
            sig,
            "d1d7215720771c6c9167b0b43456f67e3c1a0f153d1b031d0ced996beb0bcc61\
             32bca87bbeb7dc859a69a70e34785e08d38dbaac09b880890ceeae6a28fdf707"
        );
        assert!(verify(&key, "abcd", id, &sig));
    }

    #[test]
    fn the_first_key_keeps_the_id_across_a_restart() {
        let dir = std::env::temp_dir().join(format!("apexsim-drivers-{}", Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("drivers.json");
        let id = Uuid::from_u128(42);
        let keys = DriverKeys::default();
        keys.open(&path);
        assert_eq!(keys.bind(id, "AA11"), Binding::New);
        assert_eq!(keys.bind(id, "aa11"), Binding::Known);
        assert_eq!(keys.bind(id, "bb22"), Binding::Taken);
        let again = DriverKeys::default();
        again.open(&path);
        assert_eq!(again.key_of(id).as_deref(), Some("aa11"));
        assert_eq!(again.bind(id, "bb22"), Binding::Taken);
        let _ = std::fs::remove_dir_all(&dir);
    }
}

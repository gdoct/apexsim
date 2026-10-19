//! The ban list: who `Authenticate` turns away. Entries match on the
//! client's IP address or its player name (case-insensitive) and live in a
//! JSON file so they outlive a restart.

use serde::{Deserialize, Serialize};
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::{Arc, RwLock};
use tracing::warn;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct BanEntry {
    pub name: String,
    pub ip: String,
    pub reason: String,
    /// Unix seconds.
    pub banned_at: u64,
}

#[derive(Default)]
struct Inner {
    entries: Vec<BanEntry>,
    path: Option<PathBuf>,
}

/// Shared, cheap to clone. The default is empty and kept in memory only.
#[derive(Clone, Default)]
pub struct BanList {
    inner: Arc<RwLock<Inner>>,
}

impl BanList {
    /// Load `path` (a missing file is an empty list; a corrupt one is logged
    /// and treated as empty, like the records file) and keep writing there.
    pub fn open(&self, path: impl Into<PathBuf>) {
        let path = path.into();
        let entries = match std::fs::read_to_string(&path) {
            Ok(text) => serde_json::from_str(&text).unwrap_or_else(|e| {
                warn!("Ignoring unreadable ban list {}: {}", path.display(), e);
                Vec::new()
            }),
            Err(_) => Vec::new(),
        };
        let mut inner = self.inner.write().unwrap_or_else(|e| e.into_inner());
        inner.entries = entries;
        inner.path = Some(path);
    }

    pub fn entries(&self) -> Vec<BanEntry> {
        self.inner
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .entries
            .clone()
    }

    /// The entry that applies to this address and name, if any.
    pub fn check(&self, ip: IpAddr, name: &str) -> Option<BanEntry> {
        let ip = ip.to_string();
        self.inner
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .entries
            .iter()
            .find(|b| {
                (!b.ip.is_empty() && b.ip == ip)
                    || (!b.name.is_empty() && b.name.eq_ignore_ascii_case(name))
            })
            .cloned()
    }

    pub fn add(&self, entry: BanEntry) {
        let mut inner = self.inner.write().unwrap_or_else(|e| e.into_inner());
        inner
            .entries
            .retain(|b| !(b.name == entry.name && b.ip == entry.ip));
        inner.entries.push(entry);
        Self::save(&inner);
    }

    /// Lift the ban with this name and address. False when there is none.
    pub fn remove(&self, name: &str, ip: &str) -> bool {
        let mut inner = self.inner.write().unwrap_or_else(|e| e.into_inner());
        let before = inner.entries.len();
        inner.entries.retain(|b| !(b.name == name && b.ip == ip));
        let removed = inner.entries.len() != before;
        if removed {
            Self::save(&inner);
        }
        removed
    }

    fn save(inner: &Inner) {
        let Some(path) = &inner.path else { return };
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        match serde_json::to_string_pretty(&inner.entries) {
            Ok(text) => {
                if let Err(e) = std::fs::write(path, text) {
                    warn!("Could not write ban list {}: {}", path.display(), e);
                }
            }
            Err(e) => warn!("Could not serialise the ban list: {}", e),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(name: &str, ip: &str) -> BanEntry {
        BanEntry {
            name: name.into(),
            ip: ip.into(),
            reason: "test".into(),
            banned_at: 1,
        }
    }

    #[test]
    fn matches_on_address_or_name_ignoring_case() {
        let bans = BanList::default();
        bans.add(entry("Cheater", "10.0.0.5"));
        let ip = |s: &str| s.parse::<IpAddr>().unwrap();
        assert!(bans.check(ip("10.0.0.5"), "someone else").is_some());
        assert!(bans.check(ip("10.0.0.9"), "CHEATER").is_some());
        assert!(bans.check(ip("10.0.0.9"), "fair").is_none());
    }

    #[test]
    fn an_entry_without_an_address_matches_by_name_only() {
        let bans = BanList::default();
        bans.add(entry("Cheater", ""));
        let ip = "10.0.0.5".parse::<IpAddr>().unwrap();
        assert!(bans.check(ip, "fair").is_none());
        assert!(bans.check(ip, "cheater").is_some());
    }

    #[test]
    fn survives_a_reload_and_lifts() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("bans.json");
        let bans = BanList::default();
        bans.open(&path);
        bans.add(entry("a", "1.1.1.1"));
        let again = BanList::default();
        again.open(&path);
        assert_eq!(again.entries().len(), 1);
        assert!(again.remove("a", "1.1.1.1"));
        assert!(!again.remove("a", "1.1.1.1"));
        let third = BanList::default();
        third.open(&path);
        assert!(third.entries().is_empty());
    }
}

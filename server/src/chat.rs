//! Text chat: a player's line (`ClientMessage::Chat`) to the lobby, or to
//! the session they drive in or watch, sent on as `ServerMessage::Chat`.
//! Lines are cleaned and capped here, each player is held to a few lines a
//! few seconds, and the operator mutes from the dashboard
//! (`POST /api/players/<id>/mute`). Not in the sim path: the wall clock is
//! fine.

use std::collections::{HashMap, HashSet, VecDeque};
use std::time::{Duration, Instant};

use crate::data::PlayerId;

/// The longest line, in characters.
pub const MAX_CHARS: usize = 200;
/// Lines a player may send within `WINDOW`.
pub const BURST: usize = 5;
pub const WINDOW: Duration = Duration::from_secs(10);

/// What becomes of a line.
#[derive(Debug, PartialEq, Eq)]
pub enum Verdict {
    /// Send this (cleaned) text.
    Send(String),
    /// Nothing left once cleaned.
    Empty,
    /// The operator muted the player.
    Muted,
    /// Over `BURST` lines in `WINDOW`.
    TooFast,
}

/// The rate limit and the mutes.
#[derive(Debug, Default)]
pub struct ChatGuard {
    recent: HashMap<PlayerId, VecDeque<Instant>>,
    muted: HashSet<PlayerId>,
}

impl ChatGuard {
    /// Whether a line from `player` at `now` goes out, and as what.
    pub fn check(&mut self, player: PlayerId, text: &str, now: Instant) -> Verdict {
        if self.muted.contains(&player) {
            return Verdict::Muted;
        }
        let line = clean(text);
        if line.is_empty() {
            return Verdict::Empty;
        }
        let sent = self.recent.entry(player).or_default();
        while sent
            .front()
            .is_some_and(|t| now.duration_since(*t) >= WINDOW)
        {
            sent.pop_front();
        }
        if sent.len() >= BURST {
            return Verdict::TooFast;
        }
        sent.push_back(now);
        Verdict::Send(line)
    }

    /// Mute or unmute a player; returns whether that changed anything.
    pub fn set_muted(&mut self, player: PlayerId, muted: bool) -> bool {
        if muted {
            self.muted.insert(player)
        } else {
            self.muted.remove(&player)
        }
    }

    pub fn is_muted(&self, player: &PlayerId) -> bool {
        self.muted.contains(player)
    }

    /// A player gone: their rate count goes (a mute stays for a rejoin).
    pub fn forget(&mut self, player: &PlayerId) {
        self.recent.remove(player);
    }
}

/// The line as it is sent: control characters as spaces, runs of
/// whitespace as one, trimmed, and cut to `MAX_CHARS` characters.
pub fn clean(text: &str) -> String {
    let mut out = String::with_capacity(text.len().min(MAX_CHARS * 4));
    let mut space = false;
    let mut chars = 0;
    for c in text.chars() {
        let c = if c.is_control() { ' ' } else { c };
        if c.is_whitespace() {
            space = !out.is_empty();
            continue;
        }
        if space {
            if chars + 1 >= MAX_CHARS {
                break;
            }
            out.push(' ');
            chars += 1;
            space = false;
        }
        if chars >= MAX_CHARS {
            break;
        }
        out.push(c);
        chars += 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    #[test]
    fn a_line_is_cleaned_and_capped() {
        assert_eq!(clean("  hello\n\tthere  "), "hello there");
        assert_eq!(clean("\u{7}\u{1b}[31m"), "[31m");
        assert_eq!(clean("   "), "");
        let long = "x".repeat(500);
        assert_eq!(clean(&long).chars().count(), MAX_CHARS);
        // Characters, not bytes: a line of accents is not cut mid-letter.
        let accents = "é".repeat(300);
        assert_eq!(clean(&accents).chars().count(), MAX_CHARS);
    }

    #[test]
    fn a_player_gets_a_burst_then_waits() {
        let mut guard = ChatGuard::default();
        let p = Uuid::new_v4();
        let t0 = Instant::now();
        for i in 0..BURST {
            assert_eq!(
                guard.check(p, "hi", t0 + Duration::from_millis(i as u64)),
                Verdict::Send("hi".into())
            );
        }
        assert_eq!(
            guard.check(p, "hi", t0 + Duration::from_secs(1)),
            Verdict::TooFast
        );
        // Another player is not held up.
        assert_eq!(
            guard.check(Uuid::new_v4(), "hi", t0),
            Verdict::Send("hi".into())
        );
        // Once the window has passed the first, one more goes.
        assert_eq!(
            guard.check(p, "again", t0 + WINDOW + Duration::from_millis(1)),
            Verdict::Send("again".into())
        );
        assert_eq!(guard.check(p, " \n", t0 + WINDOW * 3), Verdict::Empty);
    }

    #[test]
    fn a_muted_player_says_nothing_until_unmuted() {
        let mut guard = ChatGuard::default();
        let p = Uuid::new_v4();
        assert!(guard.set_muted(p, true));
        assert!(!guard.set_muted(p, true));
        assert_eq!(guard.check(p, "hi", Instant::now()), Verdict::Muted);
        guard.forget(&p);
        assert!(guard.is_muted(&p), "a mute outlives the connection");
        assert!(guard.set_muted(p, false));
        assert_eq!(
            guard.check(p, "hi", Instant::now()),
            Verdict::Send("hi".into())
        );
    }
}

//! The last few thousand log lines, kept for the dashboard's Logs view.
//!
//! A `tracing` layer copies every event that passes the subscriber's filter
//! into a ring; `main` adds it beside the console and file layers. Without
//! the layer installed (the integration tests) the ring simply stays empty.

use serde::Serialize;
use std::collections::VecDeque;
use std::fmt::Write as _;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};
use tracing::field::{Field, Visit};
use tracing::{Event, Level, Subscriber};
use tracing_subscriber::layer::Context;
use tracing_subscriber::Layer;

#[derive(Debug, Clone, Serialize)]
pub struct LogLine {
    /// Counts up for ever; a poller asks for everything after the last one it saw.
    pub seq: u64,
    pub ts_ms: u64,
    pub level: &'static str,
    pub source: String,
    pub message: String,
}

struct Ring {
    lines: VecDeque<LogLine>,
    next_seq: u64,
    capacity: usize,
}

static RING: OnceLock<Mutex<Ring>> = OnceLock::new();

fn ring() -> &'static Mutex<Ring> {
    RING.get_or_init(|| {
        Mutex::new(Ring {
            lines: VecDeque::new(),
            next_seq: 1,
            capacity: 5000,
        })
    })
}

/// Resize the ring (the `[admin] log_buffer_lines` setting).
pub fn set_capacity(lines: usize) {
    let mut r = ring().lock().unwrap_or_else(|e| e.into_inner());
    r.capacity = lines.max(100);
    while r.lines.len() > r.capacity {
        r.lines.pop_front();
    }
}

/// Lines after `after_seq` at or above `min_level`, containing `query`
/// (case-insensitive), oldest first, at most `limit` of the newest.
pub fn read(after_seq: u64, min_level: &str, query: &str, limit: usize) -> Vec<LogLine> {
    let min = rank(min_level);
    let q = query.to_lowercase();
    let r = ring().lock().unwrap_or_else(|e| e.into_inner());
    let mut out: Vec<LogLine> = r
        .lines
        .iter()
        .filter(|l| l.seq > after_seq)
        .filter(|l| rank(l.level) >= min)
        .filter(|l| {
            q.is_empty()
                || l.message.to_lowercase().contains(&q)
                || l.source.to_lowercase().contains(&q)
        })
        .cloned()
        .collect();
    if out.len() > limit {
        out.drain(..out.len() - limit);
    }
    out
}

fn rank(level: &str) -> u8 {
    match level.to_ascii_uppercase().as_str() {
        "TRACE" => 0,
        "DEBUG" => 1,
        "INFO" => 2,
        "WARN" => 3,
        "ERROR" => 4,
        // "ALL" shows what the server logs at the level it runs at.
        _ => 0,
    }
}

fn level_name(level: &Level) -> &'static str {
    match *level {
        Level::ERROR => "ERROR",
        Level::WARN => "WARN",
        Level::INFO => "INFO",
        Level::DEBUG => "DEBUG",
        Level::TRACE => "TRACE",
    }
}

#[derive(Default)]
struct MessageVisitor {
    message: String,
    fields: String,
}

impl Visit for MessageVisitor {
    fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            let _ = write!(self.message, "{:?}", value);
        } else {
            let _ = write!(self.fields, " {}={:?}", field.name(), value);
        }
    }
    fn record_str(&mut self, field: &Field, value: &str) {
        if field.name() == "message" {
            self.message.push_str(value);
        } else {
            let _ = write!(self.fields, " {}={}", field.name(), value);
        }
    }
}

/// The layer `main` installs.
pub struct LogBufferLayer;

impl<S: Subscriber> Layer<S> for LogBufferLayer {
    fn on_event(&self, event: &Event<'_>, _ctx: Context<'_, S>) {
        let mut v = MessageVisitor::default();
        event.record(&mut v);
        let meta = event.metadata();
        let source = meta
            .target()
            .rsplit("::")
            .next()
            .unwrap_or_else(|| meta.target())
            .to_string();
        let ts_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);
        let mut r = ring().lock().unwrap_or_else(|e| e.into_inner());
        let seq = r.next_seq;
        r.next_seq += 1;
        if r.lines.len() >= r.capacity {
            r.lines.pop_front();
        }
        r.lines.push_back(LogLine {
            seq,
            ts_ms,
            level: level_name(meta.level()),
            source,
            message: format!("{}{}", v.message, v.fields),
        });
    }
}

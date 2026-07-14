//! Hand-rolled server metrics exposed in Prometheus text format on the
//! health endpoint (`/metrics`). Deliberately dependency-free: a handful of
//! atomics is all a single-process game server needs.

use crate::transport::TransportMetrics;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

/// Upper bounds (µs) of the tick-duration histogram buckets. The 240Hz tick
/// budget is ~4167µs, so the buckets bracket it.
const TICK_BUCKETS_US: [u64; 8] = [250, 500, 1000, 2000, 4167, 8000, 16000, 50000];

#[derive(Debug, Default)]
pub struct ServerMetrics {
    pub connected_players: AtomicU64,
    pub active_sessions: AtomicU64,
    pub telemetry_messages_sent: AtomicU64,
    pub input_messages_received: AtomicU64,
    pub session_tick_panics: AtomicU64,
    tick_bucket_counts: [AtomicU64; 8],
    tick_count: AtomicU64,
    tick_sum_us: AtomicU64,
    /// Transport-level counters (drops, backpressure disconnects), shared
    /// with the transport layer via its internal Arcs.
    pub transport: TransportMetrics,
}

impl ServerMetrics {
    pub fn new(transport: TransportMetrics) -> Arc<Self> {
        Arc::new(Self {
            transport,
            ..Default::default()
        })
    }

    pub fn observe_tick(&self, elapsed: Duration) {
        let us = elapsed.as_micros() as u64;
        self.tick_count.fetch_add(1, Ordering::Relaxed);
        self.tick_sum_us.fetch_add(us, Ordering::Relaxed);
        for (i, bound) in TICK_BUCKETS_US.iter().enumerate() {
            if us <= *bound {
                self.tick_bucket_counts[i].fetch_add(1, Ordering::Relaxed);
                break;
            }
        }
    }

    /// Render all metrics in Prometheus text exposition format.
    pub fn render_prometheus(&self) -> String {
        let mut out = String::with_capacity(2048);

        let gauge = |out: &mut String, name: &str, help: &str, value: u64| {
            out.push_str(&format!(
                "# HELP {name} {help}\n# TYPE {name} gauge\n{name} {value}\n"
            ));
        };
        let counter = |out: &mut String, name: &str, help: &str, value: u64| {
            out.push_str(&format!(
                "# HELP {name} {help}\n# TYPE {name} counter\n{name} {value}\n"
            ));
        };

        gauge(
            &mut out,
            "apexsim_connected_players",
            "Currently connected players",
            self.connected_players.load(Ordering::Relaxed),
        );
        gauge(
            &mut out,
            "apexsim_active_sessions",
            "Active game sessions",
            self.active_sessions.load(Ordering::Relaxed),
        );
        counter(
            &mut out,
            "apexsim_telemetry_messages_sent",
            "Telemetry messages sent to clients",
            self.telemetry_messages_sent.load(Ordering::Relaxed),
        );
        counter(
            &mut out,
            "apexsim_input_messages_received",
            "PlayerInput messages received",
            self.input_messages_received.load(Ordering::Relaxed),
        );
        counter(
            &mut out,
            "apexsim_session_tick_panics",
            "Sessions terminated because their tick panicked",
            self.session_tick_panics.load(Ordering::Relaxed),
        );
        counter(
            &mut out,
            "apexsim_tcp_messages_dropped",
            "Droppable TCP messages discarded due to backpressure",
            self.transport.tcp_dropped(),
        );
        counter(
            &mut out,
            "apexsim_udp_messages_dropped",
            "UDP messages discarded due to backpressure",
            self.transport.udp_dropped(),
        );
        counter(
            &mut out,
            "apexsim_clients_disconnected_backpressure",
            "Clients disconnected for sustained backpressure",
            self.transport.clients_disconnected(),
        );

        // Tick-duration histogram (cumulative buckets, per Prometheus format)
        out.push_str("# HELP apexsim_tick_duration_us Game loop tick duration in microseconds\n");
        out.push_str("# TYPE apexsim_tick_duration_us histogram\n");
        let mut cumulative = 0u64;
        for (i, bound) in TICK_BUCKETS_US.iter().enumerate() {
            cumulative += self.tick_bucket_counts[i].load(Ordering::Relaxed);
            out.push_str(&format!(
                "apexsim_tick_duration_us_bucket{{le=\"{bound}\"}} {cumulative}\n"
            ));
        }
        let count = self.tick_count.load(Ordering::Relaxed);
        out.push_str(&format!(
            "apexsim_tick_duration_us_bucket{{le=\"+Inf\"}} {count}\n"
        ));
        out.push_str(&format!(
            "apexsim_tick_duration_us_sum {}\n",
            self.tick_sum_us.load(Ordering::Relaxed)
        ));
        out.push_str(&format!("apexsim_tick_duration_us_count {count}\n"));

        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_observe_tick_populates_histogram() {
        let metrics = ServerMetrics::new(TransportMetrics::new());
        metrics.observe_tick(Duration::from_micros(300));
        metrics.observe_tick(Duration::from_micros(5000));
        let rendered = metrics.render_prometheus();
        assert!(rendered.contains("apexsim_tick_duration_us_count 2"));
        assert!(rendered.contains("apexsim_tick_duration_us_sum 5300"));
        // 300µs falls into the ≤500 bucket; cumulative at 500 is 1
        assert!(rendered.contains("apexsim_tick_duration_us_bucket{le=\"500\"} 1"));
    }

    #[test]
    fn test_render_contains_all_metrics() {
        let metrics = ServerMetrics::new(TransportMetrics::new());
        metrics.connected_players.store(3, Ordering::Relaxed);
        metrics.active_sessions.store(1, Ordering::Relaxed);
        let rendered = metrics.render_prometheus();
        for name in [
            "apexsim_connected_players 3",
            "apexsim_active_sessions 1",
            "apexsim_telemetry_messages_sent",
            "apexsim_input_messages_received",
            "apexsim_tcp_messages_dropped",
        ] {
            assert!(rendered.contains(name), "missing metric: {}", name);
        }
    }
}

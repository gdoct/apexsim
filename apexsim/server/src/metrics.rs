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
    /// Spectator frames sent to showcase viewers (one per viewer per frame).
    pub showcase_frames_sent: AtomicU64,
    /// The showcase channels and who watches them, refreshed by the game
    /// loop once a second: `/showcase` and the viewer gauges read it.
    showcases: std::sync::RwLock<Vec<crate::network::ShowcaseSummary>>,
    tick_bucket_counts: [AtomicU64; 8],
    tick_count: AtomicU64,
    tick_sum_us: AtomicU64,
    /// Longest tick since the dashboard sampler last took it.
    tick_max_us: AtomicU64,
    /// Ticks that took longer than their budget, and that budget (µs, set
    /// by the game loop; 0 until it starts).
    tick_overruns: AtomicU64,
    tick_budget_us: AtomicU64,
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
        self.tick_max_us.fetch_max(us, Ordering::Relaxed);
        let budget = self.tick_budget_us.load(Ordering::Relaxed);
        if budget > 0 && us > budget {
            self.tick_overruns.fetch_add(1, Ordering::Relaxed);
        }
        for (i, bound) in TICK_BUCKETS_US.iter().enumerate() {
            if us <= *bound {
                self.tick_bucket_counts[i].fetch_add(1, Ordering::Relaxed);
                break;
            }
        }
    }

    /// The game loop's per-tick time budget (one tick period).
    pub fn set_tick_budget(&self, budget: Duration) {
        self.tick_budget_us
            .store(budget.as_micros() as u64, Ordering::Relaxed);
    }

    /// Ticks run, the time they took in total (µs), and how many overran
    /// their budget, since the server started.
    pub fn tick_totals(&self) -> (u64, u64, u64) {
        (
            self.tick_count.load(Ordering::Relaxed),
            self.tick_sum_us.load(Ordering::Relaxed),
            self.tick_overruns.load(Ordering::Relaxed),
        )
    }

    /// The longest tick since the last call (µs), and start over.
    pub fn take_tick_max_us(&self) -> u64 {
        self.tick_max_us.swap(0, Ordering::Relaxed)
    }

    /// The showcase channels as last published by the game loop.
    pub fn showcases(&self) -> Vec<crate::network::ShowcaseSummary> {
        self.showcases.read().map(|s| s.clone()).unwrap_or_default()
    }

    pub fn set_showcases(&self, summaries: Vec<crate::network::ShowcaseSummary>) {
        if let Ok(mut slot) = self.showcases.write() {
            *slot = summaries;
        }
    }

    /// The channels as the `/showcase` endpoint serves them.
    pub fn render_showcases_json(&self) -> String {
        let channels: Vec<serde_json::Value> = self
            .showcases()
            .into_iter()
            .map(|s| {
                serde_json::json!({
                    "id": s.id,
                    "track_id": s.track_id.to_string(),
                    "track": s.track_name,
                    "class": s.class,
                    "weather": s.conditions.weather.name(),
                    "time_of_day_minutes": s.conditions.time_of_day_minutes,
                    "duration_s": s.duration_s,
                    "cars": s.cars,
                    "viewers": s.viewers,
                })
            })
            .collect();
        serde_json::json!({ "showcases": channels }).to_string()
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
            "apexsim_udp_datagrams_rejected",
            "Inbound UDP datagrams refused by the seal (bare, forged, tampered or replayed)",
            self.transport.udp_rejected(),
        );
        counter(
            &mut out,
            "apexsim_clients_disconnected_backpressure",
            "Clients disconnected for sustained backpressure",
            self.transport.clients_disconnected(),
        );

        counter(
            &mut out,
            "apexsim_showcase_frames_sent",
            "Spectator frames sent to showcase viewers",
            self.showcase_frames_sent.load(Ordering::Relaxed),
        );
        let showcases = self.showcases();
        if !showcases.is_empty() {
            out.push_str(
                "# HELP apexsim_showcase_viewers Clients watching each showcase channel\n",
            );
            out.push_str("# TYPE apexsim_showcase_viewers gauge\n");
            for s in &showcases {
                // A label value: quotes and backslashes escaped.
                let id = s.id.replace('\\', "\\\\").replace('"', "\\\"");
                out.push_str(&format!(
                    "apexsim_showcase_viewers{{showcase=\"{id}\"}} {}\n",
                    s.viewers
                ));
            }
        }

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
    fn showcase_channels_are_listed_and_counted() {
        let metrics = ServerMetrics::new(TransportMetrics::new());
        assert_eq!(metrics.render_showcases_json(), r#"{"showcases":[]}"#);
        assert!(!metrics
            .render_prometheus()
            .contains("apexsim_showcase_viewers"));
        metrics.set_showcases(vec![crate::network::ShowcaseSummary {
            id: "Zandvoort.gt3.day".into(),
            track_id: uuid::Uuid::nil(),
            track_name: "Zandervoort".into(),
            class: "GT3".into(),
            conditions: crate::data::SessionConditions::DEFAULT,
            duration_s: 260.0,
            cars: 16,
            viewers: 3,
        }]);
        let rendered = metrics.render_prometheus();
        assert!(
            rendered.contains("apexsim_showcase_viewers{showcase=\"Zandvoort.gt3.day\"} 3"),
            "{rendered}"
        );
        let json: serde_json::Value =
            serde_json::from_str(&metrics.render_showcases_json()).unwrap();
        assert_eq!(json["showcases"][0]["viewers"], 3);
        assert_eq!(json["showcases"][0]["track"], "Zandervoort");
        assert_eq!(json["showcases"][0]["weather"], "sunny");
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

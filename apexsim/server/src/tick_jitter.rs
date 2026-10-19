//! How evenly the game loop ticks: the interval from one tick's start to the
//! next, gathered over a window and summarised as its median, 99th
//! percentile and longest, and as a histogram against the tick's budget.
//!
//! Off unless `[debug] tick_jitter_seconds` (or
//! `APEXSIM_DEBUG_TICK_JITTER_SECONDS`) names a window: the loop then logs
//! one line per window. Only the mean rate is checked otherwise (the game
//! loop's rate window); this is how far single ticks stray from it, against
//! the 1 ms Windows timer the loop sleeps on (`timer_resolution`).
//! Measure on an idle machine: anything else running moves the tail.

use std::time::Duration;

/// Histogram edges as multiples of the tick's budget: under half, under 0.9,
/// within 10%, under 1.5, under 2, two budgets or more.
pub const BUCKET_EDGES: [f64; 5] = [0.5, 0.9, 1.1, 1.5, 2.0];

/// One window's intervals.
#[derive(Debug, Clone)]
pub struct TickJitter {
    budget: Duration,
    intervals_us: Vec<u32>,
}

/// A window, summarised.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct JitterSummary {
    pub ticks: usize,
    pub p50_us: u32,
    pub p99_us: u32,
    pub max_us: u32,
    /// Ticks per bucket of [`BUCKET_EDGES`] (six buckets).
    pub buckets: [usize; 6],
}

impl TickJitter {
    pub fn new(budget: Duration) -> Self {
        Self {
            budget,
            intervals_us: Vec::new(),
        }
    }

    pub fn record(&mut self, interval: Duration) {
        self.intervals_us
            .push(interval.as_micros().min(u32::MAX as u128) as u32);
    }

    pub fn len(&self) -> usize {
        self.intervals_us.len()
    }

    pub fn is_empty(&self) -> bool {
        self.intervals_us.is_empty()
    }

    /// The window so far, `None` when it is empty.
    pub fn summary(&self) -> Option<JitterSummary> {
        if self.intervals_us.is_empty() {
            return None;
        }
        let mut sorted = self.intervals_us.clone();
        sorted.sort_unstable();
        let at = |q: f64| sorted[((sorted.len() - 1) as f64 * q).round() as usize];
        let budget = self.budget.as_micros().max(1) as f64;
        let mut buckets = [0usize; 6];
        for &us in &sorted {
            let share = us as f64 / budget;
            let bucket = BUCKET_EDGES
                .iter()
                .position(|edge| share < *edge)
                .unwrap_or(BUCKET_EDGES.len());
            buckets[bucket] += 1;
        }
        Some(JitterSummary {
            ticks: sorted.len(),
            p50_us: at(0.5),
            p99_us: at(0.99),
            max_us: *sorted.last().unwrap_or(&0),
            buckets,
        })
    }

    /// The window summarised and begun again.
    pub fn take(&mut self) -> Option<JitterSummary> {
        let summary = self.summary();
        self.intervals_us.clear();
        summary
    }
}

impl JitterSummary {
    /// One log line: the percentiles and the histogram as shares.
    pub fn line(&self, budget: Duration) -> String {
        let pct = |n: usize| 100.0 * n as f64 / self.ticks.max(1) as f64;
        format!(
            "tick interval over {} ticks (budget {} us): p50 {} us, p99 {} us, max {} us; \
             <0.5x {:.2}%, <0.9x {:.2}%, 0.9-1.1x {:.2}%, <1.5x {:.2}%, <2x {:.2}%, >=2x {:.2}%",
            self.ticks,
            budget.as_micros(),
            self.p50_us,
            self.p99_us,
            self.max_us,
            pct(self.buckets[0]),
            pct(self.buckets[1]),
            pct(self.buckets[2]),
            pct(self.buckets[3]),
            pct(self.buckets[4]),
            pct(self.buckets[5]),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_window_is_summarised_against_its_budget() {
        let budget = Duration::from_micros(2381); // 420 Hz
        let mut jitter = TickJitter::new(budget);
        assert!(jitter.summary().is_none());
        for _ in 0..97 {
            jitter.record(Duration::from_micros(2381));
        }
        jitter.record(Duration::from_micros(1000));
        jitter.record(Duration::from_micros(3000));
        jitter.record(Duration::from_micros(15600));
        let s = jitter.take().expect("a window");
        assert_eq!(s.ticks, 100);
        assert_eq!(s.p50_us, 2381);
        assert_eq!(s.max_us, 15600);
        assert_eq!(s.p99_us, 3000);
        assert_eq!(s.buckets, [1, 0, 97, 1, 0, 1]);
        assert!(jitter.is_empty(), "begun again");
        assert!(s.line(budget).contains("p99 3000 us"));
    }
}

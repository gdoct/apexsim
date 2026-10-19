//! The Performance view's history: once a second the sampler reads the
//! server's counters and the process's CPU and memory and keeps the last two
//! minutes.

use crate::metrics::ServerMetrics;
use crate::server::ServerState;
use serde::Serialize;
use std::collections::VecDeque;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

pub const HISTORY_SECONDS: usize = 120;

#[derive(Debug, Clone, Serialize, Default)]
pub struct Sample {
    /// Ticks run in the last second.
    pub hz: f64,
    /// Mean and worst tick time in the last second, ms.
    pub tick_ms: f64,
    pub tick_max_ms: f64,
    /// Ticks over their budget in the last second.
    pub overruns: u64,
    /// Share of all cores, percent; None where the OS gives no figure.
    pub cpu_pct: Option<f64>,
    pub mem_mb: Option<f64>,
    /// Telemetry frames sent and inputs received per second.
    pub out_per_s: f64,
    pub in_per_s: f64,
    pub players: u64,
    pub sessions: u64,
}

#[derive(Default)]
pub struct History {
    samples: Mutex<VecDeque<Sample>>,
    overruns_total: std::sync::atomic::AtomicU64,
}

impl History {
    pub fn samples(&self) -> Vec<Sample> {
        self.samples
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .iter()
            .cloned()
            .collect()
    }

    /// Ticks over budget since the server started.
    pub fn overruns_total(&self) -> u64 {
        self.overruns_total.load(Ordering::Relaxed)
    }
}

/// Start the once-a-second sampling task.
pub fn spawn(metrics: Arc<ServerMetrics>, state: Arc<RwLock<ServerState>>, history: Arc<History>) {
    tokio::spawn(async move {
        let cores = std::thread::available_parallelism()
            .map(|n| n.get() as f64)
            .unwrap_or(1.0);
        let mut ticker = tokio::time::interval(Duration::from_secs(1));
        ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        let mut last_at = Instant::now();
        let mut last_ticks = metrics.tick_totals();
        let mut last_out = metrics.telemetry_messages_sent.load(Ordering::Relaxed);
        let mut last_in = metrics.input_messages_received.load(Ordering::Relaxed);
        let mut last_cpu = process::cpu_seconds();
        ticker.tick().await;
        loop {
            ticker.tick().await;
            let now = Instant::now();
            let dt = now.duration_since(last_at).as_secs_f64().max(0.001);
            last_at = now;

            let totals = metrics.tick_totals();
            let ticks = totals.0.saturating_sub(last_ticks.0);
            let sum_us = totals.1.saturating_sub(last_ticks.1);
            let overruns = totals.2.saturating_sub(last_ticks.2);
            last_ticks = totals;
            let max_us = metrics.take_tick_max_us();

            let out = metrics.telemetry_messages_sent.load(Ordering::Relaxed);
            let inp = metrics.input_messages_received.load(Ordering::Relaxed);
            let cpu = process::cpu_seconds();
            let cpu_pct = match (cpu, last_cpu) {
                (Some(now_s), Some(then_s)) => {
                    Some(((now_s - then_s) / dt / cores * 100.0).clamp(0.0, 100.0))
                }
                _ => None,
            };
            last_cpu = cpu;

            let sessions = state.read().await.sessions.len() as u64;
            let sample = Sample {
                hz: ticks as f64 / dt,
                tick_ms: if ticks > 0 {
                    sum_us as f64 / ticks as f64 / 1000.0
                } else {
                    0.0
                },
                tick_max_ms: max_us as f64 / 1000.0,
                overruns,
                cpu_pct,
                mem_mb: process::resident_mb(),
                out_per_s: out.saturating_sub(last_out) as f64 / dt,
                in_per_s: inp.saturating_sub(last_in) as f64 / dt,
                players: metrics.connected_players.load(Ordering::Relaxed),
                sessions,
            };
            last_out = out;
            last_in = inp;
            history
                .overruns_total
                .fetch_add(overruns, Ordering::Relaxed);

            let mut samples = history.samples.lock().unwrap_or_else(|e| e.into_inner());
            if samples.len() >= HISTORY_SECONDS {
                samples.pop_front();
            }
            samples.push_back(sample);
        }
    });
}

/// This process's CPU time and resident memory.
mod process {
    #[cfg(windows)]
    mod sys {
        use std::ffi::c_void;

        #[repr(C)]
        #[derive(Default)]
        pub struct FileTime {
            pub low: u32,
            pub high: u32,
        }

        #[repr(C)]
        #[derive(Default)]
        pub struct ProcessMemoryCounters {
            pub cb: u32,
            pub page_fault_count: u32,
            pub peak_working_set_size: usize,
            pub working_set_size: usize,
            pub quota_peak_paged_pool_usage: usize,
            pub quota_paged_pool_usage: usize,
            pub quota_peak_non_paged_pool_usage: usize,
            pub quota_non_paged_pool_usage: usize,
            pub pagefile_usage: usize,
            pub peak_pagefile_usage: usize,
        }

        #[link(name = "kernel32")]
        extern "system" {
            pub fn GetCurrentProcess() -> *mut c_void;
            pub fn GetProcessTimes(
                process: *mut c_void,
                creation: *mut FileTime,
                exit: *mut FileTime,
                kernel: *mut FileTime,
                user: *mut FileTime,
            ) -> i32;
            pub fn K32GetProcessMemoryInfo(
                process: *mut c_void,
                counters: *mut ProcessMemoryCounters,
                cb: u32,
            ) -> i32;
        }
    }

    #[cfg(windows)]
    pub fn cpu_seconds() -> Option<f64> {
        use sys::*;
        let (mut c, mut e, mut k, mut u) = (
            FileTime::default(),
            FileTime::default(),
            FileTime::default(),
            FileTime::default(),
        );
        // SAFETY: the pseudo-handle is always valid and the out-pointers are
        // live locals of the type the API writes.
        let ok = unsafe { GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u) };
        if ok == 0 {
            return None;
        }
        let ticks = |t: &FileTime| ((t.high as u64) << 32 | t.low as u64) as f64;
        // FILETIME counts 100 ns.
        Some((ticks(&k) + ticks(&u)) / 1e7)
    }

    #[cfg(windows)]
    pub fn resident_mb() -> Option<f64> {
        use sys::*;
        let mut counters = ProcessMemoryCounters {
            cb: std::mem::size_of::<ProcessMemoryCounters>() as u32,
            ..Default::default()
        };
        // SAFETY: as above; `cb` tells the API the struct's size.
        let ok =
            unsafe { K32GetProcessMemoryInfo(GetCurrentProcess(), &mut counters, counters.cb) };
        (ok != 0).then(|| counters.working_set_size as f64 / 1048576.0)
    }

    #[cfg(target_os = "linux")]
    pub fn cpu_seconds() -> Option<f64> {
        let stat = std::fs::read_to_string("/proc/self/stat").ok()?;
        // The command name in parentheses may hold spaces: split after it.
        let rest = stat.rsplit_once(')')?.1;
        let fields: Vec<&str> = rest.split_whitespace().collect();
        // utime and stime are fields 14 and 15 of the whole line, 12 and 13 here.
        let utime: f64 = fields.get(11)?.parse().ok()?;
        let stime: f64 = fields.get(12)?.parse().ok()?;
        Some((utime + stime) / 100.0)
    }

    #[cfg(target_os = "linux")]
    pub fn resident_mb() -> Option<f64> {
        let statm = std::fs::read_to_string("/proc/self/statm").ok()?;
        let pages: f64 = statm.split_whitespace().nth(1)?.parse().ok()?;
        Some(pages * 4096.0 / 1048576.0)
    }

    #[cfg(not(any(windows, target_os = "linux")))]
    pub fn cpu_seconds() -> Option<f64> {
        None
    }

    #[cfg(not(any(windows, target_os = "linux")))]
    pub fn resident_mb() -> Option<f64> {
        None
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn the_process_reports_memory_where_the_os_gives_it() {
        if cfg!(any(windows, target_os = "linux")) {
            let mb = super::process::resident_mb().expect("resident memory");
            assert!(mb > 1.0 && mb < 100_000.0, "{mb}");
            assert!(super::process::cpu_seconds().is_some());
        }
    }
}

//! A 1 ms system timer for as long as the game loop runs.
//!
//! Windows wakes sleeping threads on a 15.6 ms timer unless the process asks
//! for better. The game loop's `tokio::time::interval` sleeps between ticks,
//! so without this a 240 Hz loop (4.17 ms per tick) woke every 15.6 ms and,
//! with `MissedTickBehavior::Skip`, ran 64 ticks a second. The simulation
//! then ran at 27% of real time: every car covered a quarter of the distance
//! its speed implied, and lap times (counted in ticks) hid it.
//!
//! Since Windows 10 2004 the resolution is per process. The game client
//! asking for 1 ms does nothing for a server in another process, so the
//! server has to ask for itself. Requests are reference-counted by Windows,
//! so several in-process servers (the integration tests) can each hold one.
//!
//! Elsewhere this is a no-op: Linux and macOS sleeps are already fine-grained.

/// Holds the 1 ms timer resolution until dropped.
pub struct HighResolutionTimer {
    #[cfg(windows)]
    active: bool,
}

#[cfg(windows)]
mod winmm {
    pub const TIMERR_NOERROR: u32 = 0;

    #[link(name = "winmm")]
    extern "system" {
        pub fn timeBeginPeriod(period_ms: u32) -> u32;
        pub fn timeEndPeriod(period_ms: u32) -> u32;
    }
}

/// The resolution asked for, in milliseconds.
#[cfg(windows)]
const PERIOD_MS: u32 = 1;

impl HighResolutionTimer {
    pub fn acquire() -> Self {
        #[cfg(windows)]
        {
            // SAFETY: plain winmm call with no pointers; paired in Drop.
            let active = unsafe { winmm::timeBeginPeriod(PERIOD_MS) } == winmm::TIMERR_NOERROR;
            if active {
                tracing::debug!("System timer resolution raised to {} ms", PERIOD_MS);
            } else {
                tracing::warn!(
                    "Could not raise the system timer resolution; the game loop may tick \
                     slower than configured"
                );
            }
            Self { active }
        }
        #[cfg(not(windows))]
        {
            Self {}
        }
    }
}

impl Drop for HighResolutionTimer {
    fn drop(&mut self) {
        #[cfg(windows)]
        if self.active {
            // SAFETY: matches the successful timeBeginPeriod in acquire().
            unsafe {
                winmm::timeEndPeriod(PERIOD_MS);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};
    use tokio::time::{interval, MissedTickBehavior};

    /// The game loop's own ticker, with the guard held, must deliver its
    /// rate. Without the guard this measured 64 ticks/s of 240 on Windows.
    #[tokio::test]
    async fn a_240hz_interval_keeps_its_rate() {
        let _timer = HighResolutionTimer::acquire();
        let mut ticker = interval(Duration::from_secs_f64(1.0 / 240.0));
        ticker.set_missed_tick_behavior(MissedTickBehavior::Skip);
        let start = Instant::now();
        let mut ticks = 0u32;
        while start.elapsed() < Duration::from_secs(1) {
            ticker.tick().await;
            ticks += 1;
        }
        let rate = ticks as f64 / start.elapsed().as_secs_f64();
        assert!(
            rate > 0.85 * 240.0,
            "interval ticked at {rate:.0} Hz of 240"
        );
    }
}

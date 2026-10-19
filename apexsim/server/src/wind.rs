//! The session's wind: a mean from its conditions
//! (`SessionConditions::wind_mps`), gusting about it.
//!
//! The gusts are deterministic, like the AI's noise: smooth value noise
//! over the session clock from an integer hash, so a replayed session
//! blows the same. [`gusting`] is set onto the session's track once a tick
//! before the physics (`TrackSurface::wind_now_mps`), and the physics reads
//! the car's airspeed against it (`physics::calculate_aerodynamic_forces`).

/// A gust changes the wind's speed by up to this share of the mean.
pub const GUST_SHARE: f32 = 0.35;
/// And its direction by up to this much, degrees.
pub const GUST_VEER_DEG: f32 = 15.0;
/// Seconds between the noise's control points: how long a gust lasts.
const GUST_PERIOD_S: f32 = 4.0;

/// A hash of `(seed, salt)` onto 0..1: splitmix64.
pub fn hash01(seed: u64, salt: u64) -> f32 {
    let mut z = seed ^ salt.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    z = z.wrapping_add(0x9E37_79B9_7F4A_7C15);
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^= z >> 31;
    (z >> 40) as f32 / (1u64 << 24) as f32
}

/// Smooth noise in -1..1 over `t` seconds: control points every
/// [`GUST_PERIOD_S`], eased between.
fn noise(t: f32, salt: u64) -> f32 {
    let x = (t / GUST_PERIOD_S).max(0.0);
    let i = x.floor();
    let f = x - i;
    let ease = f * f * (3.0 - 2.0 * f);
    let a = hash01(i as u64, salt) * 2.0 - 1.0;
    let b = hash01(i as u64 + 1, salt) * 2.0 - 1.0;
    a + (b - a) * ease
}

/// The wind `seconds` into the session, m/s: the mean, faster or slower by
/// up to [`GUST_SHARE`] and veering by up to [`GUST_VEER_DEG`].
pub fn gusting(mean: [f32; 2], seconds: f32) -> [f32; 2] {
    let speed = (mean[0] * mean[0] + mean[1] * mean[1]).sqrt();
    if speed <= 0.0 {
        return [0.0; 2];
    }
    let gain = 1.0 + GUST_SHARE * noise(seconds, 0x6057);
    let veer = (GUST_VEER_DEG * noise(seconds, 0x7EE5)).to_radians();
    let (c, s) = (veer.cos(), veer.sin());
    [
        gain * (mean[0] * c - mean[1] * s),
        gain * (mean[0] * s + mean[1] * c),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_hash_spreads_and_repeats() {
        let a: Vec<f32> = (0..1000).map(|i| hash01(i, 7)).collect();
        assert!(a.iter().all(|v| (0.0..1.0).contains(v)));
        let mean = a.iter().sum::<f32>() / a.len() as f32;
        assert!((mean - 0.5).abs() < 0.05, "{mean}");
        assert_eq!(hash01(42, 7), hash01(42, 7));
        assert_ne!(hash01(42, 7), hash01(42, 8));
    }

    #[test]
    fn a_gust_stays_near_the_mean_and_moves_smoothly() {
        let mean = [5.0, 0.0];
        let mut last = gusting(mean, 0.0);
        for k in 1..(240 * 60) {
            let w = gusting(mean, k as f32 / 240.0);
            let speed = (w[0] * w[0] + w[1] * w[1]).sqrt();
            assert!(
                (5.0 * (1.0 - GUST_SHARE) - 1e-3..=5.0 * (1.0 + GUST_SHARE) + 1e-3)
                    .contains(&speed)
            );
            let step = ((w[0] - last[0]).powi(2) + (w[1] - last[1]).powi(2)).sqrt();
            assert!(step < 0.02, "a tick's change: {step}");
            last = w;
        }
        assert_eq!(gusting([0.0, 0.0], 3.0), [0.0, 0.0]);
    }
}

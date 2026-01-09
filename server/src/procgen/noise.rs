/// Multi-octave noise generation for terrain
use noise::{NoiseFn, Perlin};

/// Multi-layer noise sampler for natural terrain generation
pub struct TerrainNoise {
    /// Low-frequency noise for base terrain shape
    low_freq: Perlin,
    /// Mid-frequency noise for medium detail
    mid_freq: Perlin,
    /// High-frequency noise for fine detail
    high_freq: Perlin,
}

impl TerrainNoise {
    /// Create a new terrain noise generator with the given seed
    pub fn new(seed: u32) -> Self {
        Self {
            low_freq: Perlin::new(seed),
            mid_freq: Perlin::new(seed.wrapping_add(1)),
            high_freq: Perlin::new(seed.wrapping_add(2)),
        }
    }

    /// Sample layered noise at world position
    ///
    /// Returns a value roughly in the range [-1, 1], combining three octaves:
    /// - Low frequency (weight: 1.0) for base terrain shape
    /// - Mid frequency (weight: 0.5) for medium detail
    /// - High frequency (weight: 0.25) for fine detail
    ///
    /// # Arguments
    /// * `x` - World X coordinate
    /// * `y` - World Y coordinate
    /// * `freq_multiplier` - Frequency scaling factor from environment preset
    pub fn sample(&self, x: f32, y: f32, freq_multiplier: f32) -> f32 {
        // Low-frequency base shape
        let low = self.low_freq.get([
            (x * freq_multiplier * 0.01) as f64,
            (y * freq_multiplier * 0.01) as f64,
        ]) as f32;

        // Mid-frequency detail
        let mid = self.mid_freq.get([
            (x * freq_multiplier * 0.05) as f64,
            (y * freq_multiplier * 0.05) as f64,
        ]) as f32 * 0.5;

        // High-frequency fine detail
        let high = self.high_freq.get([
            (x * freq_multiplier * 0.1) as f64,
            (y * freq_multiplier * 0.1) as f64,
        ]) as f32 * 0.25;

        // Combine all layers
        low + mid + high
    }

    /// Sample noise with custom octave weights
    ///
    /// Allows more control over the terrain character by specifying
    /// weights for each frequency layer.
    pub fn sample_weighted(
        &self,
        x: f32,
        y: f32,
        freq_multiplier: f32,
        low_weight: f32,
        mid_weight: f32,
        high_weight: f32,
    ) -> f32 {
        let low = self.low_freq.get([
            (x * freq_multiplier * 0.01) as f64,
            (y * freq_multiplier * 0.01) as f64,
        ]) as f32 * low_weight;

        let mid = self.mid_freq.get([
            (x * freq_multiplier * 0.05) as f64,
            (y * freq_multiplier * 0.05) as f64,
        ]) as f32 * mid_weight;

        let high = self.high_freq.get([
            (x * freq_multiplier * 0.1) as f64,
            (y * freq_multiplier * 0.1) as f64,
        ]) as f32 * high_weight;

        low + mid + high
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_deterministic_noise() {
        let seed = 12345;
        let noise1 = TerrainNoise::new(seed);
        let noise2 = TerrainNoise::new(seed);

        // Same seed should produce same values
        let val1 = noise1.sample(100.0, 200.0, 1.0);
        let val2 = noise2.sample(100.0, 200.0, 1.0);

        assert_eq!(val1, val2);
    }

    #[test]
    fn test_different_seeds_produce_different_values() {
        let noise1 = TerrainNoise::new(12345);
        let noise2 = TerrainNoise::new(54321);

        // Sample multiple points and ensure at least one differs
        let mut found_difference = false;
        for x in 0..5 {
            for y in 0..5 {
                let val1 = noise1.sample(x as f32 * 50.0, y as f32 * 50.0, 1.0);
                let val2 = noise2.sample(x as f32 * 50.0, y as f32 * 50.0, 1.0);
                if val1 != val2 {
                    found_difference = true;
                    break;
                }
            }
            if found_difference {
                break;
            }
        }

        assert!(found_difference, "Different seeds should produce different values");
    }

    #[test]
    fn test_noise_roughly_in_range() {
        let noise = TerrainNoise::new(42);

        // Sample multiple points
        for x in 0..10 {
            for y in 0..10 {
                let val = noise.sample(x as f32 * 10.0, y as f32 * 10.0, 1.0);
                // Perlin noise is roughly in [-1, 1], with three octaves we expect roughly [-1.75, 1.75]
                assert!(val > -2.0 && val < 2.0, "Noise value {} out of expected range", val);
            }
        }
    }
}

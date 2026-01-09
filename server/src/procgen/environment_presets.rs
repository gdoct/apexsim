/// Environment preset definitions for different biomes
use super::world_data::EnvironmentPreset;
use std::collections::HashMap;

/// Load all environment presets
///
/// Returns a HashMap mapping environment type names to their configurations.
/// Currently returns hardcoded presets; future versions may load from YAML.
pub fn load_presets() -> HashMap<String, EnvironmentPreset> {
    let mut presets = HashMap::new();

    presets.insert("desert".to_string(), EnvironmentPreset::desert());
    presets.insert("forest".to_string(), EnvironmentPreset::forest());
    presets.insert("city".to_string(), EnvironmentPreset::city());
    presets.insert("mountains".to_string(), EnvironmentPreset::mountains());
    presets.insert("plains".to_string(), EnvironmentPreset::plains());
    presets.insert("country".to_string(), EnvironmentPreset::country());
    presets.insert("park".to_string(), EnvironmentPreset::park());

    presets
}

/// Get a specific environment preset by name
///
/// Returns None if the environment type is not recognized.
pub fn get_preset(environment_type: &str) -> Option<EnvironmentPreset> {
    match environment_type.to_lowercase().as_str() {
        "desert" => Some(EnvironmentPreset::desert()),
        "forest" => Some(EnvironmentPreset::forest()),
        "city" => Some(EnvironmentPreset::city()),
        "mountains" => Some(EnvironmentPreset::mountains()),
        "plains" => Some(EnvironmentPreset::plains()),
        "country" => Some(EnvironmentPreset::country()),
        "park" => Some(EnvironmentPreset::park()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_all_presets() {
        let presets = load_presets();

        assert_eq!(presets.len(), 7);
        assert!(presets.contains_key("desert"));
        assert!(presets.contains_key("forest"));
        assert!(presets.contains_key("city"));
        assert!(presets.contains_key("mountains"));
        assert!(presets.contains_key("plains"));
        assert!(presets.contains_key("country"));
        assert!(presets.contains_key("park"));
    }

    #[test]
    fn test_get_preset() {
        assert!(get_preset("desert").is_some());
        assert!(get_preset("Desert").is_some()); // Case insensitive
        assert!(get_preset("FOREST").is_some());
        assert!(get_preset("invalid").is_none());
    }

    #[test]
    fn test_preset_characteristics() {
        let desert = get_preset("desert").unwrap();
        assert_eq!(desert.max_height, 15.0);
        assert!(desert.object_density < 0.5); // Desert is sparse

        let forest = get_preset("forest").unwrap();
        assert_eq!(forest.max_height, 40.0);
        assert!(forest.object_density > 0.7); // Forest is dense

        let city = get_preset("city").unwrap();
        assert_eq!(city.max_height, 5.0); // City is mostly flat
    }
}

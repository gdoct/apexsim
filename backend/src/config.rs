use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerConfig {
    pub server: ServerSettings,
    pub network: NetworkSettings,
    pub content: ContentSettings,
    pub logging: LoggingSettings,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerSettings {
    pub tick_rate_hz: u16,
    pub max_sessions: u8,
    pub session_timeout_seconds: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkSettings {
    pub tcp_bind: String,
    pub udp_bind: String,
    pub heartbeat_interval_ms: u64,
    pub heartbeat_timeout_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ContentSettings {
    pub cars_dir: String,
    pub tracks_dir: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LoggingSettings {
    pub level: String,
    pub console_enabled: bool,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            server: ServerSettings {
                tick_rate_hz: 240,
                max_sessions: 8,
                session_timeout_seconds: 300,
            },
            network: NetworkSettings {
                tcp_bind: "127.0.0.1:9000".to_string(),
                udp_bind: "127.0.0.1:9001".to_string(),
                heartbeat_interval_ms: 1000,
                heartbeat_timeout_ms: 5000,
            },
            content: ContentSettings {
                cars_dir: "./content/cars".to_string(),
                tracks_dir: "./content/tracks".to_string(),
            },
            logging: LoggingSettings {
                level: "info".to_string(),
                console_enabled: true,
            },
        }
    }
}

impl ServerConfig {
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self, Box<dyn std::error::Error>> {
        let contents = fs::read_to_string(path)?;
        let config: ServerConfig = toml::from_str(&contents)?;
        Ok(config)
    }

    pub fn load_or_default<P: AsRef<Path>>(path: P) -> Self {
        Self::load(path).unwrap_or_else(|e| {
            eprintln!("Failed to load config: {}, using defaults", e);
            Self::default()
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = ServerConfig::default();
        assert_eq!(config.server.tick_rate_hz, 240);
        assert_eq!(config.server.max_sessions, 8);
        assert_eq!(config.network.tcp_bind, "127.0.0.1:9000");
    }

    #[test]
    fn test_config_serialization() {
        let config = ServerConfig::default();
        let toml_str = toml::to_string(&config).unwrap();
        assert!(toml_str.contains("tick_rate_hz"));
        assert!(toml_str.contains("tcp_bind"));
    }
}

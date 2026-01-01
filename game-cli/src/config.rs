use serde::{Deserialize, Serialize};
use std::fs;
use std::net::SocketAddr;
use std::path::Path;

/// Client configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientConfig {
    pub server: ServerConfig,
    pub player: PlayerConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerConfig {
    pub tcp_address: String,
    pub udp_address: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlayerConfig {
    pub name: String,
    pub token: String,
}

impl Default for ClientConfig {
    fn default() -> Self {
        Self {
            server: ServerConfig {
                tcp_address: "127.0.0.1:9000".to_string(),
                udp_address: "127.0.0.1:9001".to_string(),
            },
            player: PlayerConfig {
                name: "Player".to_string(),
                token: "dev-token".to_string(),
            },
        }
    }
}

impl ClientConfig {
    /// Load configuration from file or create default
    pub fn load_or_default(path: &str) -> Self {
        if Path::new(path).exists() {
            match fs::read_to_string(path) {
                Ok(content) => match toml::from_str(&content) {
                    Ok(config) => {
                        tracing::info!("Loaded configuration from {}", path);
                        return config;
                    }
                    Err(e) => {
                        tracing::warn!("Failed to parse config file: {}", e);
                    }
                },
                Err(e) => {
                    tracing::warn!("Failed to read config file: {}", e);
                }
            }
        }

        // Return default and optionally save it
        let default_config = Self::default();
        if let Err(e) = default_config.save(path) {
            tracing::warn!("Failed to save default config: {}", e);
        }
        default_config
    }

    /// Save configuration to file
    pub fn save(&self, path: &str) -> anyhow::Result<()> {
        let content = toml::to_string_pretty(self)?;
        fs::write(path, content)?;
        Ok(())
    }

    /// Get TCP server address
    pub fn get_tcp_addr(&self) -> anyhow::Result<SocketAddr> {
        self.server
            .tcp_address
            .parse()
            .map_err(|e| anyhow::anyhow!("Invalid TCP address: {}", e))
    }

    /// Get UDP server address
    pub fn get_udp_addr(&self) -> anyhow::Result<SocketAddr> {
        self.server
            .udp_address
            .parse()
            .map_err(|e| anyhow::anyhow!("Invalid UDP address: {}", e))
    }
}

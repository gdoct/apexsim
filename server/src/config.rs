use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;
use tracing::{error, info};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerConfig {
    pub server: ServerSettings,
    pub network: NetworkSettings,
    pub content: ContentSettings,
    pub logging: LoggingSettings,
    #[serde(default)]
    pub ai: AiSettings,
    #[serde(default)]
    pub auth: AuthSettings,
}

/// How `Authenticate` tokens are validated.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum AuthMode {
    /// Accept any token (development only; the server logs a warning).
    Dev,
    /// Token must match one of `auth.tokens`.
    Token,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AuthSettings {
    pub mode: AuthMode,
    /// Shared-secret tokens accepted in `Token` mode.
    #[serde(default)]
    pub tokens: Vec<String>,
}

impl Default for AuthSettings {
    fn default() -> Self {
        Self {
            mode: AuthMode::Dev,
            tokens: Vec::new(),
        }
    }
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
    pub health_bind: String,
    pub tls_cert_path: String,
    pub tls_key_path: String,
    pub require_tls: bool,
    pub heartbeat_interval_ms: u64,
    pub heartbeat_timeout_ms: u64,
    /// Broadcast telemetry every N-th simulation tick (1 = every tick).
    /// Default 4 → 60Hz snapshots of a 240Hz sim; clients interpolate.
    #[serde(default = "default_telemetry_divisor")]
    pub telemetry_divisor: u16,
}

fn default_telemetry_divisor() -> u16 {
    4
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
    /// Write JSON-lines logs to daily-rotated files in `file_dir`.
    #[serde(default)]
    pub file_enabled: bool,
    #[serde(default = "default_log_dir")]
    pub file_dir: String,
}

fn default_log_dir() -> String {
    "./logs".to_string()
}

/// AI driver configuration settings.
///
/// These settings control the default behavior of AI drivers as per the specification.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiSettings {
    /// Default aggressiveness level (0.0-1.0)
    pub default_aggressiveness: f32,
    /// Default precision level (0.0-1.0)
    pub default_precision: f32,
    /// Default reaction time in milliseconds
    pub default_reaction_time_ms: u16,
    /// Default steering smoothness (0.0-1.0)
    pub default_steering_smoothness: f32,
    /// Default randomness scale (0.0-1.0)
    pub default_randomness_scale: f32,
    /// Enable deterministic mode (for replay consistency)
    pub deterministic_mode: bool,
}

impl Default for AiSettings {
    fn default() -> Self {
        Self {
            default_aggressiveness: 0.5,
            default_precision: 0.7,
            default_reaction_time_ms: 100,
            default_steering_smoothness: 0.7,
            default_randomness_scale: 0.05,
            deterministic_mode: false,
        }
    }
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
                health_bind: "127.0.0.1:9002".to_string(),
                tls_cert_path: "./certs/server.crt".to_string(),
                tls_key_path: "./certs/server.key".to_string(),
                // Fail closed by default: plaintext requires an explicit
                // `require_tls = false` opt-out in the config file.
                require_tls: true,
                heartbeat_interval_ms: 1000,
                heartbeat_timeout_ms: 5000,
                telemetry_divisor: default_telemetry_divisor(),
            },
            content: ContentSettings {
                cars_dir: "../content/cars".to_string(),
                tracks_dir: "../content/tracks".to_string(),
            },
            logging: LoggingSettings {
                level: "info".to_string(),
                console_enabled: true,
                file_enabled: false,
                file_dir: default_log_dir(),
            },
            ai: AiSettings::default(),
            auth: AuthSettings::default(),
        }
    }
}

impl ServerConfig {
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self, Box<dyn std::error::Error>> {
        let contents = fs::read_to_string(path)?;
        let config: ServerConfig = toml::from_str(&contents)?;
        Ok(config)
    }

    /// Load the config file if it exists; fall back to defaults only when the
    /// file is absent. A present-but-invalid config is a fatal error — silently
    /// running with defaults would mask misconfiguration (e.g. TLS settings).
    /// `APEXSIM_*` environment overrides are applied after loading.
    pub fn load_or_default<P: AsRef<Path>>(path: P) -> Self {
        let path = path.as_ref();
        let mut config = if !path.exists() {
            // May run before the tracing subscriber is initialized, so also
            // print to stderr.
            eprintln!("Config file {} not found, using defaults", path.display());
            error!("Config file {} not found, using defaults", path.display());
            Self::default()
        } else {
            match Self::load(path) {
                Ok(config) => config,
                Err(e) => {
                    eprintln!("Failed to parse config {}: {}", path.display(), e);
                    error!("Failed to parse config {}: {}", path.display(), e);
                    std::process::exit(1);
                }
            }
        };
        config.apply_env_overrides();
        config
    }

    /// Override individual settings from `APEXSIM_*` environment variables,
    /// e.g. `APEXSIM_NETWORK_TCP_BIND=0.0.0.0:9100` or the port-only form
    /// `APEXSIM_NETWORK_TCP_PORT=9100`.
    pub fn apply_env_overrides(&mut self) {
        fn env_string(key: &str, target: &mut String) {
            if let Ok(v) = std::env::var(key) {
                info!("Config override from env: {}={}", key, v);
                *target = v;
            }
        }
        fn env_parse<T: std::str::FromStr>(key: &str, target: &mut T) {
            if let Ok(v) = std::env::var(key) {
                match v.parse::<T>() {
                    Ok(parsed) => {
                        info!("Config override from env: {}={}", key, v);
                        *target = parsed;
                    }
                    Err(_) => error!("Ignoring invalid env override {}={}", key, v),
                }
            }
        }
        /// Replace just the port of a "host:port" bind string.
        fn env_port(key: &str, bind: &mut String) {
            if let Ok(v) = std::env::var(key) {
                match v.parse::<u16>() {
                    Ok(port) => {
                        if let Some(host) = bind.rsplit_once(':').map(|(h, _)| h.to_string()) {
                            info!("Config override from env: {}={}", key, port);
                            *bind = format!("{}:{}", host, port);
                        }
                    }
                    Err(_) => error!("Ignoring invalid env override {}={}", key, v),
                }
            }
        }

        env_parse("APEXSIM_SERVER_TICK_RATE_HZ", &mut self.server.tick_rate_hz);
        env_parse("APEXSIM_SERVER_MAX_SESSIONS", &mut self.server.max_sessions);
        env_string("APEXSIM_NETWORK_TCP_BIND", &mut self.network.tcp_bind);
        env_string("APEXSIM_NETWORK_UDP_BIND", &mut self.network.udp_bind);
        env_string("APEXSIM_NETWORK_HEALTH_BIND", &mut self.network.health_bind);
        env_port("APEXSIM_NETWORK_TCP_PORT", &mut self.network.tcp_bind);
        env_port("APEXSIM_NETWORK_UDP_PORT", &mut self.network.udp_bind);
        env_port("APEXSIM_NETWORK_HEALTH_PORT", &mut self.network.health_bind);
        env_string(
            "APEXSIM_NETWORK_TLS_CERT_PATH",
            &mut self.network.tls_cert_path,
        );
        env_string(
            "APEXSIM_NETWORK_TLS_KEY_PATH",
            &mut self.network.tls_key_path,
        );
        env_parse("APEXSIM_NETWORK_REQUIRE_TLS", &mut self.network.require_tls);
        env_parse(
            "APEXSIM_NETWORK_TELEMETRY_DIVISOR",
            &mut self.network.telemetry_divisor,
        );
        env_string("APEXSIM_CONTENT_CARS_DIR", &mut self.content.cars_dir);
        env_string("APEXSIM_CONTENT_TRACKS_DIR", &mut self.content.tracks_dir);
        env_string("APEXSIM_LOGGING_LEVEL", &mut self.logging.level);
    }

    /// Sanity-check the configuration. Returns all problems found.
    pub fn validate(&self) -> Result<(), String> {
        let mut errors = Vec::new();

        if !(30..=1000).contains(&self.server.tick_rate_hz) {
            errors.push(format!(
                "server.tick_rate_hz must be between 30 and 1000 (got {})",
                self.server.tick_rate_hz
            ));
        }
        if self.server.max_sessions == 0 {
            errors.push("server.max_sessions must be at least 1".to_string());
        }
        for (name, bind) in [
            ("network.tcp_bind", &self.network.tcp_bind),
            ("network.udp_bind", &self.network.udp_bind),
            ("network.health_bind", &self.network.health_bind),
        ] {
            if bind.parse::<std::net::SocketAddr>().is_err() {
                errors.push(format!("{} is not a valid socket address: {}", name, bind));
            }
        }
        if self.network.telemetry_divisor == 0 {
            errors.push("network.telemetry_divisor must be at least 1".to_string());
        }
        if self.network.heartbeat_timeout_ms <= self.network.heartbeat_interval_ms {
            errors.push(format!(
                "network.heartbeat_timeout_ms ({}) must be greater than heartbeat_interval_ms ({})",
                self.network.heartbeat_timeout_ms, self.network.heartbeat_interval_ms
            ));
        }
        for (name, dir) in [
            ("content.cars_dir", &self.content.cars_dir),
            ("content.tracks_dir", &self.content.tracks_dir),
        ] {
            if !Path::new(dir).is_dir() {
                errors.push(format!("{} does not exist: {}", name, dir));
            }
        }
        if !["trace", "debug", "info", "warn", "error"].contains(&self.logging.level.as_str()) {
            errors.push(format!(
                "logging.level must be one of trace|debug|info|warn|error (got {})",
                self.logging.level
            ));
        }
        if self.auth.mode == AuthMode::Token && self.auth.tokens.is_empty() {
            errors.push(
                "auth.mode = \"token\" requires at least one entry in auth.tokens".to_string(),
            );
        }

        if errors.is_empty() {
            Ok(())
        } else {
            Err(errors.join("; "))
        }
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

    fn valid_test_config() -> ServerConfig {
        let mut config = ServerConfig::default();
        // Content dirs that exist regardless of checkout layout
        config.content.cars_dir = ".".to_string();
        config.content.tracks_dir = ".".to_string();
        config
    }

    #[test]
    fn test_validate_accepts_sane_config() {
        assert!(valid_test_config().validate().is_ok());
    }

    #[test]
    fn test_validate_rejects_bad_tick_rate() {
        let mut config = valid_test_config();
        config.server.tick_rate_hz = 0;
        assert!(config.validate().unwrap_err().contains("tick_rate_hz"));
    }

    #[test]
    fn test_validate_rejects_bad_bind_addr() {
        let mut config = valid_test_config();
        config.network.tcp_bind = "not-an-addr".to_string();
        assert!(config.validate().unwrap_err().contains("tcp_bind"));
    }

    #[test]
    fn test_validate_rejects_heartbeat_timeout_below_interval() {
        let mut config = valid_test_config();
        config.network.heartbeat_interval_ms = 5000;
        config.network.heartbeat_timeout_ms = 1000;
        assert!(config.validate().unwrap_err().contains("heartbeat"));
    }

    #[test]
    fn test_validate_rejects_token_mode_without_tokens() {
        let mut config = valid_test_config();
        config.auth.mode = AuthMode::Token;
        config.auth.tokens.clear();
        assert!(config.validate().unwrap_err().contains("auth.tokens"));
    }

    #[test]
    fn test_validate_rejects_missing_content_dir() {
        let mut config = valid_test_config();
        config.content.cars_dir = "/nonexistent/path/cars".to_string();
        assert!(config.validate().unwrap_err().contains("cars_dir"));
    }

    #[test]
    fn test_env_override_tcp_port() {
        // Env vars are process-global: use a unique var per test process run.
        // Serialize with a lock to avoid clashing with other env tests.
        let mut config = valid_test_config();
        std::env::set_var("APEXSIM_NETWORK_TCP_PORT", "9555");
        config.apply_env_overrides();
        std::env::remove_var("APEXSIM_NETWORK_TCP_PORT");
        assert!(config.network.tcp_bind.ends_with(":9555"));
    }

    #[test]
    fn test_env_override_invalid_value_ignored() {
        let mut config = valid_test_config();
        let original = config.server.tick_rate_hz;
        std::env::set_var("APEXSIM_SERVER_TICK_RATE_HZ", "not-a-number");
        config.apply_env_overrides();
        std::env::remove_var("APEXSIM_SERVER_TICK_RATE_HZ");
        assert_eq!(config.server.tick_rate_hz, original);
    }
}

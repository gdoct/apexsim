use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;
use tracing::{error, info};

/// The settings a running server takes from a config saved on the dashboard
/// (`ServerConfig::take_live`): each is read where it is used, never cached
/// at startup. Every other key needs a restart.
/// What `[admin] restart` may be (`AdminSettings::restart`).
pub const RESTART_MODES: &[&str] = &["respawn", "exit", "off"];

pub const LIVE_KEYS: &[&str] = &[
    "server.max_sessions",
    "server.session_timeout_seconds",
    "server.reconnect_grace_seconds",
    "auth.mode",
    "auth.tokens",
    "logging.level",
    "admin.log_buffer_lines",
    "debug.stand_in_driver",
    "debug.events",
];

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerConfig {
    pub server: ServerSettings,
    pub network: NetworkSettings,
    pub content: ContentSettings,
    pub logging: LoggingSettings,
    #[serde(default)]
    pub auth: AuthSettings,
    #[serde(default)]
    pub records: RecordsSettings,
    #[serde(default)]
    pub physics: PhysicsSettings,
    #[serde(default)]
    pub showcase: ShowcaseSettings,
    #[serde(default)]
    pub debug: DebugSettings,
    #[serde(default)]
    pub admin: AdminSettings,
    /// Balance of performance: `[[balance]]` tables, one a car
    /// (`crate::balance`).
    #[serde(default)]
    pub balance: Vec<crate::balance::CarBalance>,
}

/// The web dashboard (`crate::admin`, docs/server/operations.md): an HTTP and
/// an HTTPS listener on their own ports, behind a shared access token.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct AdminSettings {
    pub enabled: bool,
    /// Plain HTTP listener; empty switches it off. Loopback by default: a
    /// token that crosses a network in clear text is no token.
    pub http_bind: String,
    /// HTTPS listener; empty switches it off.
    pub https_bind: String,
    /// PEM certificate chain and key for the HTTPS listener. Both empty:
    /// `network.tls_cert_path` / `tls_key_path` when those are set, else a
    /// self-signed certificate made at startup (browsers warn about it).
    pub tls_cert_path: String,
    pub tls_key_path: String,
    /// The access token the login asks for. Empty: a random one is made at
    /// startup and printed once in the log.
    pub token: String,
    /// Answer every plain HTTP request except the login page's own assets
    /// with a redirect to the HTTPS listener.
    pub redirect_http_to_https: bool,
    /// Where the ban list is kept.
    pub bans_file: String,
    /// Where the cars and tracks switched off on the dashboard are kept.
    pub content_file: String,
    /// Where the dashboard's schedule of sessions is kept.
    pub schedule_file: String,
    /// Log lines kept for the Logs view.
    pub log_buffer_lines: usize,
    /// What the dashboard's RESTART SERVER does (`admin::api::server_restart`):
    /// `"respawn"` starts a copy of this process with the same command line,
    /// which waits for the game ports to come free, and stops this one;
    /// `"exit"` stops this one and leaves starting it again to whatever
    /// supervises it (Docker's `restart: unless-stopped`, systemd); `"off"`
    /// refuses.
    pub restart: String,
}

impl Default for AdminSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            http_bind: "127.0.0.1:9003".to_string(),
            https_bind: "127.0.0.1:9004".to_string(),
            tls_cert_path: String::new(),
            tls_key_path: String::new(),
            token: String::new(),
            redirect_http_to_https: false,
            bans_file: "./records/bans.json".to_string(),
            content_file: "./records/content.json".to_string(),
            schedule_file: "./records/schedule.json".to_string(),
            log_buffer_lines: 5000,
            restart: "respawn".to_string(),
        }
    }
}

/// Debug-only hooks for reaching states a short race seldom does
/// (`crate::debug_hooks`). Both off by default; never set them on a server
/// people race on.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct DebugSettings {
    /// Every human's car driven by an AI profile instead of its input.
    pub stand_in_driver: bool,
    /// `"<seconds of green>:<host|all|car index>:<action>"`, `;` separated.
    pub events: String,
    /// Log how evenly the game loop ticks, one line per this many seconds
    /// (`crate::tick_jitter`); 0 for never.
    pub tick_jitter_seconds: u32,
}

/// What a showcase channel does when its file ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ShowcaseMode {
    /// One file per channel, forever.
    Loop,
    /// The next file of the playlist at each end.
    Rotate,
}

/// Rendered races the server plays to clients sitting in the menu
/// (`crate::showcase`, docs/game/spectator.md): a file read and a fan-out in
/// place of a simulated demo race per client.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct ShowcaseSettings {
    pub enabled: bool,
    /// Folder of `.apxs` files, relative to the working directory. A folder
    /// that is not there is no showcase, not an error.
    pub dir: String,
    /// The list (`showcase.yml`) a showcase is rendered from at startup,
    /// into `dir`, when `dir` has no file of it this server can play (none,
    /// or one raced on other content). Empty, or a file that is not there:
    /// nothing is rendered.
    pub list: String,
    /// File names without `.apxs`, in channel order; `"*"` is every file
    /// not named. The first is what a client gets when it asks for none.
    pub playlist: Vec<String>,
    pub mode: ShowcaseMode,
    /// Send every Nth frame of a file (1 = all of them).
    pub stream_divisor: u16,
}

impl Default for ShowcaseSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            dir: "./showcase".to_string(),
            list: "./showcase.yml".to_string(),
            playlist: vec!["*".to_string()],
            mode: ShowcaseMode::Loop,
            stream_divisor: 1,
        }
    }
}

pub use crate::track_loader::RoadContactMode;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PhysicsSettings {
    /// `"mesh"` (the default) or `"centerline"`.
    pub road_contact: RoadContactMode,
}

impl Default for PhysicsSettings {
    fn default() -> Self {
        Self {
            road_contact: RoadContactMode::Mesh,
        }
    }
}

/// Lap records: the best legal lap each driver has set on a track in a car,
/// kept across sessions (`crate::records`). Off means nothing is written and
/// nothing is read; the server still times and broadcasts every lap.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct RecordsSettings {
    pub enabled: bool,
    /// Folder for `lap_records.json` and the `ghosts/` traces beside it,
    /// relative to the working directory.
    pub dir: String,
    /// Folder the server writes its replays to: an `.apxs` per finished
    /// session and a `.meta.json` beside it (`crate::stream_recorder`).
    pub replay_dir: String,
    /// The health port lists them (`/recordings`) and hands each out
    /// (`/recordings/<file>`), and `LobbyState.recordings_port` tells the
    /// clients: the session browser's Recorded list.
    pub serve_replays: bool,
    /// Finished replays the folder keeps, the oldest deleted beyond it as
    /// each new one is written (`stream_recorder::prune`); 0 is no limit.
    pub keep_replays: u32,
    /// MB the finished replays may take, the same way; 0 is no limit.
    pub keep_replays_mb: u64,
    /// Each finished race's classification as JSON and CSV in `results/`
    /// under `dir` (`crate::results`).
    pub results: bool,
    /// An `http://` or `https://` endpoint each race result is also POSTed
    /// to (`crate::webhook`); empty is off.
    pub webhook_url: String,
    /// `json` (the result as written) or `discord` (a Discord message).
    pub webhook_format: crate::webhook::WebhookFormat,
}

impl Default for RecordsSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            dir: "./records".to_string(),
            replay_dir: "./replays".to_string(),
            serve_replays: true,
            keep_replays: 0,
            keep_replays_mb: 0,
            results: true,
            webhook_url: String::new(),
            webhook_format: crate::webhook::WebhookFormat::Json,
        }
    }
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
    /// How long a session is kept once every human in it has lost their
    /// connection, for one of them to come back; then it ends.
    #[serde(default = "default_reconnect_grace_seconds")]
    pub reconnect_grace_seconds: u32,
}

fn default_reconnect_grace_seconds() -> u32 {
    60
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
    /// Default 7 → 60 Hz snapshots of a 420 Hz sim; clients interpolate.
    #[serde(default = "default_telemetry_divisor")]
    pub telemetry_divisor: u16,
}

fn default_telemetry_divisor() -> u16 {
    7
}

/// The content root a server finds its cars and tracks under when the config
/// names none: the sibling content checkout, seen from `apexsim/server/server` (the
/// server's working directory in development).
pub const DEFAULT_CONTENT_ROOT: &str = "../../../apexsim-content";

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ContentSettings {
    /// The content root (`APEXSIM_CONTENT`): `cars_dir` and `tracks_dir`
    /// default to its `cars` and `tracks`. Empty is `DEFAULT_CONTENT_ROOT`.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub root: String,
    /// The cars folder; empty is `<root>/cars`. Read it through
    /// `ContentSettings::cars_dir()`.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub cars_dir: String,
    /// The tracks folder; empty is `<root>/tracks`. Read it through
    /// `ContentSettings::tracks_dir()`.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub tracks_dir: String,
    /// Leave out tracks the AC importer wrote (a `<Stem>.import.json` beside
    /// the YAML): the player's own data, with sidecars a debug build reads
    /// slowly when a session lands on one. The in-process test servers set
    /// it in a debug build.
    #[serde(default)]
    pub skip_imported_tracks: bool,
}

impl ContentSettings {
    /// The content root: `root`, or `DEFAULT_CONTENT_ROOT` when it is empty.
    pub fn root(&self) -> &str {
        if self.root.is_empty() {
            DEFAULT_CONTENT_ROOT
        } else {
            &self.root
        }
    }

    /// The cars folder: `cars_dir` when it is set, else `<root>/cars`.
    pub fn cars_dir(&self) -> String {
        self.resolve(&self.cars_dir, "cars")
    }

    /// The tracks folder: `tracks_dir` when it is set, else `<root>/tracks`.
    pub fn tracks_dir(&self) -> String {
        self.resolve(&self.tracks_dir, "tracks")
    }

    fn resolve(&self, explicit: &str, sub: &str) -> String {
        if explicit.is_empty() {
            Path::new(self.root())
                .join(sub)
                .to_string_lossy()
                .into_owned()
        } else {
            explicit.to_string()
        }
    }
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

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            server: ServerSettings {
                tick_rate_hz: 420,
                max_sessions: 8,
                session_timeout_seconds: 300,
                reconnect_grace_seconds: default_reconnect_grace_seconds(),
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
                root: String::new(),
                cars_dir: String::new(),
                tracks_dir: String::new(),
                skip_imported_tracks: false,
            },
            logging: LoggingSettings {
                level: "info".to_string(),
                console_enabled: true,
                file_enabled: false,
                file_dir: default_log_dir(),
            },
            auth: AuthSettings::default(),
            records: RecordsSettings::default(),
            physics: PhysicsSettings::default(),
            showcase: ShowcaseSettings::default(),
            debug: DebugSettings::default(),
            admin: AdminSettings::default(),
            balance: Vec::new(),
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
        env_string("APEXSIM_CONTENT", &mut self.content.root);
        env_string("APEXSIM_CONTENT_CARS_DIR", &mut self.content.cars_dir);
        env_string("APEXSIM_CONTENT_TRACKS_DIR", &mut self.content.tracks_dir);
        env_string("APEXSIM_LOGGING_LEVEL", &mut self.logging.level);
        env_parse(
            "APEXSIM_PHYSICS_ROAD_CONTACT",
            &mut self.physics.road_contact,
        );
        env_parse("APEXSIM_SHOWCASE_ENABLED", &mut self.showcase.enabled);
        env_string("APEXSIM_SHOWCASE_DIR", &mut self.showcase.dir);
        env_string("APEXSIM_SHOWCASE_LIST", &mut self.showcase.list);
        env_string("APEXSIM_REPLAY_DIR", &mut self.records.replay_dir);
        env_parse(
            "APEXSIM_DEBUG_STAND_IN_DRIVER",
            &mut self.debug.stand_in_driver,
        );
        env_string("APEXSIM_DEBUG_EVENTS", &mut self.debug.events);
        env_parse(
            "APEXSIM_DEBUG_TICK_JITTER_SECONDS",
            &mut self.debug.tick_jitter_seconds,
        );
        env_parse("APEXSIM_ADMIN_ENABLED", &mut self.admin.enabled);
        env_string("APEXSIM_ADMIN_HTTP_BIND", &mut self.admin.http_bind);
        env_string("APEXSIM_ADMIN_HTTPS_BIND", &mut self.admin.https_bind);
        env_port("APEXSIM_ADMIN_HTTP_PORT", &mut self.admin.http_bind);
        env_port("APEXSIM_ADMIN_HTTPS_PORT", &mut self.admin.https_bind);
        env_string("APEXSIM_ADMIN_TOKEN", &mut self.admin.token);
    }

    /// Sanity-check the configuration. Returns all problems found.
    /// Every `section.key` whose value differs between two configs, sorted.
    pub fn changed_keys(&self, other: &ServerConfig) -> Vec<String> {
        fn flat(config: &ServerConfig) -> std::collections::BTreeMap<String, toml::Value> {
            let mut out = std::collections::BTreeMap::new();
            if let Ok(toml::Value::Table(table)) = toml::Value::try_from(config) {
                for (section, value) in table {
                    match value {
                        toml::Value::Table(keys) => {
                            for (key, v) in keys {
                                out.insert(format!("{section}.{key}"), v);
                            }
                        }
                        v => {
                            out.insert(section, v);
                        }
                    }
                }
            }
            out
        }
        let (a, b) = (flat(self), flat(other));
        let mut keys: Vec<String> = a.keys().chain(b.keys()).cloned().collect();
        keys.sort();
        keys.dedup();
        keys.into_iter().filter(|k| a.get(k) != b.get(k)).collect()
    }

    /// Take the settings a running server applies without a restart
    /// ([`LIVE_KEYS`]) from `saved`, leaving every other one as it started.
    pub fn take_live(&mut self, saved: &ServerConfig) {
        self.server.max_sessions = saved.server.max_sessions;
        self.server.session_timeout_seconds = saved.server.session_timeout_seconds;
        self.server.reconnect_grace_seconds = saved.server.reconnect_grace_seconds;
        self.auth = saved.auth.clone();
        self.logging.level = saved.logging.level.clone();
        self.admin.log_buffer_lines = saved.admin.log_buffer_lines;
        self.debug = saved.debug.clone();
    }

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
        errors.extend(self.balance.iter().filter_map(|b| b.problem()));
        if self.showcase.stream_divisor == 0 {
            errors.push("showcase.stream_divisor must be at least 1".to_string());
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
        if self.admin.enabled {
            for (name, bind) in [
                ("admin.http_bind", &self.admin.http_bind),
                ("admin.https_bind", &self.admin.https_bind),
            ] {
                if !bind.is_empty() && bind.parse::<std::net::SocketAddr>().is_err() {
                    errors.push(format!("{} is not a valid socket address: {}", name, bind));
                }
            }
            if self.admin.tls_cert_path.is_empty() != self.admin.tls_key_path.is_empty() {
                errors.push(
                    "admin.tls_cert_path and admin.tls_key_path must be set together".to_string(),
                );
            }
            if !RESTART_MODES.contains(&self.admin.restart.as_str()) {
                errors.push(format!(
                    "admin.restart must be one of {:?}, not {:?}",
                    RESTART_MODES, self.admin.restart
                ));
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
            ("content.cars_dir", self.content.cars_dir()),
            ("content.tracks_dir", self.content.tracks_dir()),
        ] {
            if !Path::new(&dir).is_dir() {
                errors.push(format!("{} does not exist: {}", name, dir));
            }
        }
        if !["trace", "debug", "info", "warn", "error"].contains(&self.logging.level.as_str()) {
            errors.push(format!(
                "logging.level must be one of trace|debug|info|warn|error (got {})",
                self.logging.level
            ));
        }
        if self.network.require_tls
            && (self.network.tls_cert_path.trim().is_empty()
                || self.network.tls_key_path.trim().is_empty())
        {
            errors.push(
                "network.require_tls = true requires network.tls_cert_path and network.tls_key_path"
                    .to_string(),
            );
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
        assert_eq!(config.server.tick_rate_hz, 420);
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

    /// A server.toml from before the replay folder was a setting has a
    /// `[records]` table without it; it loads with the default.
    #[test]
    fn a_records_table_without_the_replay_dir_still_loads() {
        let toml_str = toml::to_string(&ServerConfig::default()).unwrap().replace(
            "replay_dir = \"./replays\"
",
            "",
        );
        assert!(!toml_str.contains("replay_dir"));
        let config: ServerConfig = toml::from_str(&toml_str).unwrap();
        assert_eq!(config.records.replay_dir, "./replays");
    }

    /// An older server.toml may still carry the `[ai]` table, which nothing
    /// ever read; it loads and the table is ignored.
    #[test]
    fn an_old_ai_table_still_loads() {
        let mut toml_str = toml::to_string(&ServerConfig::default()).unwrap();
        toml_str.push_str(
            "\n[ai]\ndefault_aggressiveness = 0.5\ndefault_precision = 0.7\n\
             default_reaction_time_ms = 100\ndefault_steering_smoothness = 0.7\n\
             default_randomness_scale = 0.05\ndeterministic_mode = false\n",
        );
        let parsed: ServerConfig = toml::from_str(&toml_str).unwrap();
        assert_eq!(parsed.server.tick_rate_hz, 420);
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

    /// Without `cars_dir` and `tracks_dir` the folders are the content
    /// root's `cars` and `tracks`; the root defaults to the sibling content
    /// checkout, and a folder that is named wins over the root.
    #[test]
    fn content_dirs_resolve_under_the_content_root() {
        let written = toml::to_string(&ServerConfig::default()).unwrap();
        assert!(
            !written.contains("cars_dir"),
            "an empty folder stays off the file"
        );
        let with_root = written.replace("[content]\n", "[content]\nroot = \"/srv/content\"\n");
        let config: ServerConfig = toml::from_str(&with_root).unwrap();
        assert_eq!(config.content.root(), "/srv/content");
        assert_eq!(
            Path::new(&config.content.cars_dir()),
            Path::new("/srv/content").join("cars")
        );
        assert_eq!(
            Path::new(&config.content.tracks_dir()),
            Path::new("/srv/content").join("tracks")
        );

        let mut content = ServerConfig::default().content;
        assert_eq!(content.root(), DEFAULT_CONTENT_ROOT);
        assert_eq!(
            Path::new(&content.cars_dir()),
            Path::new(DEFAULT_CONTENT_ROOT).join("cars")
        );
        content.root = "/srv/content".to_string();
        content.tracks_dir = "/elsewhere/tracks".to_string();
        assert_eq!(content.tracks_dir(), "/elsewhere/tracks");
        assert_eq!(
            Path::new(&content.cars_dir()),
            Path::new("/srv/content").join("cars")
        );
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
    fn test_road_contact_parses_and_defaults_to_the_mesh() {
        let config = ServerConfig::default();
        assert_eq!(config.physics.road_contact, RoadContactMode::Mesh);
        let written = toml::to_string(&config).unwrap();
        assert!(written.contains("road_contact = \"mesh\""));
        let parsed: ServerConfig = toml::from_str(
            &written.replace("road_contact = \"mesh\"", "road_contact = \"centerline\""),
        )
        .unwrap();
        assert_eq!(parsed.physics.road_contact, RoadContactMode::Centerline);
        assert_eq!("Mesh".parse::<RoadContactMode>(), Ok(RoadContactMode::Mesh));
        assert!("triangles".parse::<RoadContactMode>().is_err());
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

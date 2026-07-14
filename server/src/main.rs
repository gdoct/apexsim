use apexsim_server::config::ServerConfig;
use apexsim_server::server::run_server;
use clap::Parser;
use tracing::{error, info};
use tracing_subscriber::layer::SubscriberExt;
use tracing_subscriber::util::SubscriberInitExt;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Path to server.toml configuration file
    #[arg(short, long, default_value = "./server.toml")]
    config: String,

    /// Override log level (trace|debug|info|warn|error)
    #[arg(short, long)]
    log_level: Option<String>,

    /// Generate procedural terrain for all tracks with environment_type metadata
    #[arg(long)]
    generate_terrain: bool,
}

/// Initialize tracing: console output plus optional JSON-lines file output
/// with daily rotation. Returns the appender guard, which must stay alive for
/// the lifetime of the process so buffered log lines get flushed.
fn init_tracing(
    config: &ServerConfig,
    cli_log_level: Option<&str>,
) -> Option<tracing_appender::non_blocking::WorkerGuard> {
    let level = cli_log_level.unwrap_or(&config.logging.level);
    let env_filter = tracing_subscriber::EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new(level));

    let console_layer = config
        .logging
        .console_enabled
        .then(tracing_subscriber::fmt::layer);

    let (file_layer, guard) = if config.logging.file_enabled {
        if let Err(e) = std::fs::create_dir_all(&config.logging.file_dir) {
            eprintln!(
                "Failed to create log directory {}: {}",
                config.logging.file_dir, e
            );
            std::process::exit(1);
        }
        let appender = tracing_appender::rolling::daily(&config.logging.file_dir, "apexsim.log");
        let (non_blocking, guard) = tracing_appender::non_blocking(appender);
        let layer = tracing_subscriber::fmt::layer()
            .json()
            .with_ansi(false)
            .with_writer(non_blocking);
        (Some(layer), Some(guard))
    } else {
        (None, None)
    };

    tracing_subscriber::registry()
        .with(env_filter)
        .with(console_layer)
        .with(file_layer)
        .init();

    guard
}

/// Wait for SIGINT (Ctrl+C) or SIGTERM (e.g. from systemd/Kubernetes).
async fn shutdown_signal() {
    #[cfg(unix)]
    {
        let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("failed to install SIGTERM handler");
        tokio::select! {
            _ = tokio::signal::ctrl_c() => info!("SIGINT received"),
            _ = sigterm.recv() => info!("SIGTERM received"),
        }
    }
    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
        info!("Ctrl+C received");
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    // Load configuration first so logging settings can shape the subscriber
    let config = ServerConfig::load_or_default(&args.config);
    let _log_guard = init_tracing(&config, args.log_level.as_deref());

    info!("Starting ApexSim Racing Server v0.1.0");
    info!("Configuration loaded from: {}", args.config);

    // Check if we're in terrain generation mode
    if args.generate_terrain {
        info!("🌍 TERRAIN GENERATION MODE");
        info!("Generating procedural terrain for all tracks...");

        use apexsim_server::procgen;
        let result = procgen::terrain::generate_all_terrain(&config.content.tracks_dir);

        match result {
            Ok(count) => {
                info!("✅ Successfully generated terrain for {} track(s)", count);
                return Ok(());
            }
            Err(e) => {
                error!("❌ Terrain generation failed: {}", e);
                return Err(e.into());
            }
        }
    }

    info!("TCP bind: {}", config.network.tcp_bind);
    info!("UDP bind: {}", config.network.udp_bind);
    info!("Tick rate: {}Hz", config.server.tick_rate_hz);

    let handle = run_server(config).await?;

    info!("Server is running. Send SIGINT (Ctrl+C) or SIGTERM to stop.");

    shutdown_signal().await;

    info!("Shutdown signal received. Cleaning up...");
    handle.shutdown().await;

    let final_state = handle.state.read().await;
    info!(
        "Server shutting down with {} active sessions",
        final_state.sessions.len()
    );

    Ok(())
}

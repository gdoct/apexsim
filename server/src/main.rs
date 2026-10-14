use apexsim_server::config::ServerConfig;
use apexsim_server::server::run_server_from;
use clap::Parser;
use tracing::info;
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
}

/// Initialize tracing: console output plus optional JSON-lines file output
/// with daily rotation. Returns the appender guard, which must stay alive for
/// the lifetime of the process so buffered log lines get flushed.
fn init_tracing(
    config: &ServerConfig,
    cli_log_level: Option<&str>,
) -> Option<tracing_appender::non_blocking::WorkerGuard> {
    let level = cli_log_level.unwrap_or(&config.logging.level);
    let from_env = tracing_subscriber::EnvFilter::try_from_default_env().ok();
    // The file's level is in force when neither RUST_LOG nor --log-level
    // overrides it, and then a level saved on the dashboard replaces it live.
    let file_in_charge = from_env.is_none() && cli_log_level.is_none();
    let env_filter = from_env.unwrap_or_else(|| tracing_subscriber::EnvFilter::new(level));
    let (env_filter, filter_handle) = tracing_subscriber::reload::Layer::new(env_filter);
    if file_in_charge {
        apexsim_server::admin::set_log_level_hook(Box::new(move |level: &str| {
            let filter =
                tracing_subscriber::EnvFilter::try_new(level).map_err(|e| e.to_string())?;
            filter_handle.reload(filter).map_err(|e| e.to_string())
        }));
    }

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
        .with(apexsim_server::admin::logbuf::LogBufferLayer)
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

    info!("TCP bind: {}", config.network.tcp_bind);
    info!("UDP bind: {}", config.network.udp_bind);
    info!("Tick rate: {}Hz", config.server.tick_rate_hz);

    let handle = run_server_from(config, Some(std::path::PathBuf::from(&args.config))).await?;

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

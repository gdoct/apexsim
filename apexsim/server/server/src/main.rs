use apexsim_server::config::ServerConfig;
use apexsim_server::server::run_server_from;
use clap::{Parser, Subcommand};
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

    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Content packs in a release install (CMDLINE.md, "content")
    #[command(subcommand)]
    Content(ContentCommand),
}

#[derive(Subcommand, Debug)]
enum ContentCommand {
    /// Install a pack: an .apxpack checked whole, written where the server
    /// and the game look and recorded in Content/installed.json; or a loose
    /// .acp, its cars, tracks and HUD components written into custom/
    Install {
        pack: std::path::PathBuf,
        /// The install folder (holding Server and Game); default: the one
        /// this server sits in
        #[arg(long)]
        root: Option<std::path::PathBuf>,
        /// An .acp into a source checkout instead: its code root (apexsim/,
        /// with apexsim-content/ beside it)
        #[arg(long, conflicts_with = "root")]
        checkout: Option<std::path::PathBuf>,
        /// An .acp: say what it holds and where it would go, write nothing
        #[arg(long)]
        dry_run: bool,
        /// An .acp: install only this item (`car:<name>`, `track:<Stem>`,
        /// `hud:<id>`); repeat for more. Default: every item
        #[arg(long, value_name = "KIND:NAME")]
        only: Vec<String>,
    },
    /// Remove the player's own cars, tracks and HUD components from custom/
    Uninstall {
        /// `car:<folder>`, `track:<Stem>` or `hud:<id>`; one or more
        #[arg(value_name = "KIND:NAME", required = true)]
        items: Vec<String>,
        /// The install folder (holding Server and Game); default: the one
        /// this server sits in
        #[arg(long)]
        root: Option<std::path::PathBuf>,
        /// A source checkout instead: its code root (apexsim/, with
        /// apexsim-content/ beside it)
        #[arg(long, conflicts_with = "root")]
        checkout: Option<std::path::PathBuf>,
    },
    /// List the installed packs
    List {
        #[arg(long)]
        root: Option<std::path::PathBuf>,
    },
    /// Check every installed pack's files against the pack it came from
    Verify {
        #[arg(long)]
        root: Option<std::path::PathBuf>,
    },
}

/// `apexsim-server content ...`: prints JSON on stdout, exits 1 on an
/// error with `apexsim-server: <message>` on stderr.
fn run_content(command: ContentCommand) -> std::process::ExitCode {
    use apexsim_server::content_pack;
    let result = (|| -> Result<serde_json::Value, String> {
        match command {
            ContentCommand::Install {
                pack,
                root,
                checkout,
                dry_run,
                only,
            } => {
                use apexsim_formats::pack::acp::ACP_EXTENSION;
                use apexsim_server::content_acp::{self, Layout};
                if pack
                    .extension()
                    .is_some_and(|e| e.eq_ignore_ascii_case(ACP_EXTENSION))
                {
                    let layout = match checkout {
                        Some(code) => Layout::checkout(&code)?,
                        None => Layout::Release(content_pack::install_root(root)?),
                    };
                    let report = content_acp::install(&layout, &pack, dry_run, &only)?;
                    return serde_json::to_value(report).map_err(|e| e.to_string());
                }
                if checkout.is_some() || dry_run || !only.is_empty() {
                    return Err("--checkout, --dry-run and --only are for .acp packs".into());
                }
                let root = content_pack::install_root(root)?;
                let entry = content_pack::install(&root, &pack)?;
                serde_json::to_value(entry).map_err(|e| e.to_string())
            }
            ContentCommand::Uninstall {
                items,
                root,
                checkout,
            } => {
                use apexsim_server::content_acp::{self, Layout};
                let layout = match checkout {
                    Some(code) => Layout::checkout(&code)?,
                    None => Layout::Release(content_pack::install_root(root)?),
                };
                serde_json::to_value(content_acp::uninstall(&layout, &items)?)
                    .map_err(|e| e.to_string())
            }
            ContentCommand::List { root } => {
                let root = content_pack::install_root(root)?;
                serde_json::to_value(content_pack::read_installed(&root)?)
                    .map_err(|e| e.to_string())
            }
            ContentCommand::Verify { root } => {
                let root = content_pack::install_root(root)?;
                let report = content_pack::verify(&root)?;
                let broken = report.iter().any(|(_, problems)| !problems.is_empty());
                let rows: Vec<serde_json::Value> = report
                    .into_iter()
                    .map(|(entry, problems)| {
                        serde_json::json!({ "folder": entry.folder, "version": entry.version, "problems": problems })
                    })
                    .collect();
                if broken {
                    println!(
                        "{}",
                        serde_json::to_string_pretty(&rows).unwrap_or_default()
                    );
                    return Err("some installed files are not their pack's".into());
                }
                Ok(serde_json::Value::Array(rows))
            }
        }
    })();
    match result {
        Ok(value) => {
            println!(
                "{}",
                serde_json::to_string_pretty(&value).unwrap_or_default()
            );
            std::process::ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("apexsim-server: {e}");
            std::process::ExitCode::FAILURE
        }
    }
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

/// Set on the copy of the process a dashboard restart starts: the game
/// binds of the process it replaces, which it waits to come free.
const RESTART_WAIT_ENV: &str = "APEXSIM_RESTART_WAIT";
/// How long that copy waits before giving up and binding anyway (and
/// failing as any server on a taken port does).
const RESTART_WAIT: std::time::Duration = std::time::Duration::from_secs(30);

/// A dashboard restart's copy waits here until the process it replaces has
/// let go of the game ports: a bind of each succeeds (and is dropped
/// again). Nothing to wait for when the variable is not set.
async fn wait_for_ports(config: &ServerConfig) {
    if std::env::var_os(RESTART_WAIT_ENV).is_none() {
        return;
    }
    std::env::remove_var(RESTART_WAIT_ENV);
    let (tcp, udp) = (
        config.network.tcp_bind.clone(),
        config.network.udp_bind.clone(),
    );
    info!(
        "Restarted from the dashboard: waiting for {} / {} to come free",
        tcp, udp
    );
    let started = std::time::Instant::now();
    loop {
        let free =
            std::net::TcpListener::bind(&tcp).is_ok() && std::net::UdpSocket::bind(&udp).is_ok();
        if free || started.elapsed() > RESTART_WAIT {
            if !free {
                tracing::warn!(
                    "The ports did not come free in {:?}; starting anyway",
                    RESTART_WAIT
                );
            }
            return;
        }
        tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    }
}

/// `[admin] restart = "respawn"`: start a copy of this process with the same
/// command line, told to wait for the ports this one still holds.
fn respawn() -> std::io::Result<u32> {
    let exe = std::env::current_exe()?;
    let child = std::process::Command::new(&exe)
        .args(std::env::args_os().skip(1))
        .env(RESTART_WAIT_ENV, "1")
        .spawn()?;
    Ok(child.id())
}

fn main() -> std::process::ExitCode {
    let args = Args::parse();
    if let Some(Command::Content(command)) = args.command {
        return run_content(command);
    }
    match serve(args) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("apexsim-server: {e}");
            std::process::ExitCode::FAILURE
        }
    }
}

#[tokio::main]
async fn serve(args: Args) -> Result<(), Box<dyn std::error::Error>> {
    // Load configuration first so logging settings can shape the subscriber
    let config = ServerConfig::load_or_default(&args.config);
    let _log_guard = init_tracing(&config, args.log_level.as_deref());

    info!("Starting ApexSim Racing Server v0.1.0");
    info!("Configuration loaded from: {}", args.config);

    info!("TCP bind: {}", config.network.tcp_bind);
    info!("UDP bind: {}", config.network.udp_bind);
    info!("Tick rate: {}Hz", config.server.tick_rate_hz);

    wait_for_ports(&config).await;
    let restart_mode = config.admin.restart.clone();
    let handle = run_server_from(config, Some(std::path::PathBuf::from(&args.config))).await?;

    info!("Server is running. Send SIGINT (Ctrl+C) or SIGTERM to stop.");

    let restarting = tokio::select! {
        _ = shutdown_signal() => false,
        _ = handle.restart.notified() => {
            info!("Restart asked from the dashboard ({})", restart_mode);
            true
        }
    };

    info!("Shutdown signal received. Cleaning up...");
    handle.shutdown().await;

    let final_state = handle.state.read().await;
    info!(
        "Server shutting down with {} active sessions",
        final_state.sessions.len()
    );
    drop(final_state);

    if restarting && restart_mode == "respawn" {
        match respawn() {
            Ok(pid) => info!(
                "Started the new server process (pid {}); this one stops",
                pid
            ),
            Err(e) => tracing::error!("Could not start the new server process: {}", e),
        }
    } else if restarting {
        info!("Stopping for the supervisor to start the server again");
    }

    Ok(())
}

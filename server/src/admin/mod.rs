//! The web dashboard (docs/server/operations.md): the server's operator
//! interface, served on two ports of its own, plain HTTP and HTTPS, from the
//! server binary itself (the page and its fonts are compiled in).
//!
//! Everything behind `/api` needs the access token (`[admin] token`): the
//! login page trades it for a session cookie, and scripts may send it as
//! `Authorization: Bearer`. The pages and fonts are public, since they hold
//! nothing but the login form and the app that asks the API for the rest.
//!
//! - [`api`]: the JSON endpoints, built from the live server state
//! - [`bans`]: the ban list `Authenticate` consults
//! - [`logbuf`]: the log ring the Logs view reads
//! - [`sampler`]: the Performance view's history

pub mod api;
pub mod bans;
pub mod logbuf;
pub mod sampler;

use crate::config::AdminSettings;
use crate::health::HealthState;
use crate::metrics::ServerMetrics;
use crate::server::ServerState;
use crate::transport::TransportLayer;
use bytes::Bytes;
use http_body_util::{BodyExt, Full, Limited};
use hyper::body::Incoming;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{header, Method, Request, Response, StatusCode};
use hyper_util::rt::{TokioIo, TokioTimer};
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::net::TcpListener;
use tokio::sync::RwLock;
use tokio_rustls::TlsAcceptor;
use tracing::{debug, info, warn};

type Resp = Response<Full<Bytes>>;

/// How long a login lasts.
const SESSION_TTL: Duration = Duration::from_secs(12 * 3600);
/// Wrong tokens from one address before it is told to wait.
const MAX_LOGIN_FAILURES: u32 = 5;
const LOGIN_LOCKOUT: Duration = Duration::from_secs(60);
/// Largest request body any endpoint takes (the config file is the biggest).
const MAX_BODY_BYTES: usize = 512 * 1024;

/// Everything the endpoints read, shared by both listeners.
pub struct AdminContext {
    pub settings: AdminSettings,
    pub token: String,
    pub state: Arc<RwLock<ServerState>>,
    pub transport: Arc<RwLock<TransportLayer>>,
    pub metrics: Arc<ServerMetrics>,
    pub health: HealthState,
    pub bans: bans::BanList,
    pub history: Arc<sampler::History>,
    /// The `server.toml` this process was started from, when it was started
    /// from one; the Config view edits it.
    pub config_path: Option<PathBuf>,
    pub started: Instant,
    sessions: Mutex<HashMap<String, Instant>>,
    failures: Mutex<HashMap<IpAddr, (u32, Instant)>>,
    pub http_addr: Mutex<Option<SocketAddr>>,
    pub https_addr: Mutex<Option<SocketAddr>>,
}

/// What `run_server` keeps of the dashboard: where it listens and the token
/// (generated when the config left it empty).
#[derive(Debug, Clone)]
pub struct AdminHandle {
    pub http_addr: Option<SocketAddr>,
    pub https_addr: Option<SocketAddr>,
    pub token: String,
}

pub struct AdminInputs {
    pub settings: AdminSettings,
    pub state: Arc<RwLock<ServerState>>,
    pub transport: Arc<RwLock<TransportLayer>>,
    pub metrics: Arc<ServerMetrics>,
    pub health: HealthState,
    pub config_path: Option<PathBuf>,
    /// The server's own TLS certificate paths, the HTTPS listener's fallback.
    pub network_cert: (String, String),
}

/// Bind the listeners and start serving. Returns once they are bound; an
/// address that cannot be bound is an error, so a typo is not a silent
/// dashboard that never came up.
pub async fn start(inputs: AdminInputs) -> Result<Option<AdminHandle>, Box<dyn std::error::Error>> {
    let settings = inputs.settings.clone();
    if !settings.enabled {
        info!("Admin dashboard disabled");
        return Ok(None);
    }
    logbuf::set_capacity(settings.log_buffer_lines);

    let bans = inputs.transport.read().await.bans();
    if !settings.bans_file.trim().is_empty() {
        bans.open(&settings.bans_file);
    }

    let generated = settings.token.trim().is_empty();
    let token = if generated {
        generate_secret()
    } else {
        settings.token.clone()
    };

    let history = Arc::new(sampler::History::default());
    sampler::spawn(
        Arc::clone(&inputs.metrics),
        Arc::clone(&inputs.state),
        Arc::clone(&history),
    );

    let ctx = Arc::new(AdminContext {
        settings: settings.clone(),
        token: token.clone(),
        state: inputs.state,
        transport: inputs.transport,
        metrics: inputs.metrics,
        health: inputs.health,
        bans,
        history,
        config_path: inputs.config_path,
        started: Instant::now(),
        sessions: Mutex::new(HashMap::new()),
        failures: Mutex::new(HashMap::new()),
        http_addr: Mutex::new(None),
        https_addr: Mutex::new(None),
    });

    let mut handle = AdminHandle {
        http_addr: None,
        https_addr: None,
        token: token.clone(),
    };

    // HTTPS first: its certificate is the part that can fail to load.
    if !settings.https_bind.trim().is_empty() {
        let acceptor = tls_acceptor(&settings, &inputs.network_cert)?;
        let listener = TcpListener::bind(&settings.https_bind)
            .await
            .map_err(|e| format!("Failed to bind admin HTTPS {}: {}", settings.https_bind, e))?;
        let addr = listener.local_addr()?;
        *ctx.https_addr.lock().unwrap_or_else(|e| e.into_inner()) = Some(addr);
        handle.https_addr = Some(addr);
        info!("Admin dashboard (HTTPS) on https://{}", addr);
        tokio::spawn(serve(listener, Some(acceptor), Arc::clone(&ctx)));
    }
    if !settings.http_bind.trim().is_empty() {
        let listener = TcpListener::bind(&settings.http_bind)
            .await
            .map_err(|e| format!("Failed to bind admin HTTP {}: {}", settings.http_bind, e))?;
        let addr = listener.local_addr()?;
        *ctx.http_addr.lock().unwrap_or_else(|e| e.into_inner()) = Some(addr);
        handle.http_addr = Some(addr);
        info!("Admin dashboard (HTTP) on http://{}", addr);
        if !addr.ip().is_loopback() && !settings.redirect_http_to_https {
            warn!(
                "Admin HTTP listener is reachable from the network: the access token crosses it \
                 unencrypted. Bind it to 127.0.0.1 or set admin.redirect_http_to_https."
            );
        }
        tokio::spawn(serve(listener, None, Arc::clone(&ctx)));
    }
    if handle.http_addr.is_none() && handle.https_addr.is_none() {
        warn!("Admin dashboard enabled but both admin.http_bind and admin.https_bind are empty");
    }
    if generated {
        info!(
            "Admin access token (none configured, generated for this run): {}",
            token
        );
    }
    Ok(Some(handle))
}

fn generate_secret() -> String {
    format!(
        "{}{}",
        uuid::Uuid::new_v4().simple(),
        uuid::Uuid::new_v4().simple()
    )
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

fn tls_acceptor(
    settings: &AdminSettings,
    network_cert: &(String, String),
) -> Result<TlsAcceptor, Box<dyn std::error::Error>> {
    use rustls::pki_types::{CertificateDer, PrivateKeyDer};

    let (cert_path, key_path) = if !settings.tls_cert_path.is_empty() {
        (
            settings.tls_cert_path.clone(),
            settings.tls_key_path.clone(),
        )
    } else if Path::new(network_cert.0.trim()).is_file()
        && Path::new(network_cert.1.trim()).is_file()
    {
        // The game port's certificate, when it really is there (the default
        // config names paths that were never created).
        network_cert.clone()
    } else {
        self_signed(settings)?
    };

    let mut cert_reader = std::io::BufReader::new(
        std::fs::File::open(&cert_path)
            .map_err(|e| format!("cannot open admin certificate {}: {}", cert_path, e))?,
    );
    let certs: Vec<CertificateDer<'static>> =
        rustls_pemfile::certs(&mut cert_reader).collect::<Result<_, _>>()?;
    if certs.is_empty() {
        return Err(format!("no certificate in {}", cert_path).into());
    }
    let mut key_reader = std::io::BufReader::new(
        std::fs::File::open(&key_path)
            .map_err(|e| format!("cannot open admin key {}: {}", key_path, e))?,
    );
    let key: PrivateKeyDer<'static> = rustls_pemfile::private_key(&mut key_reader)?
        .ok_or_else(|| format!("no private key in {}", key_path))?;
    let mut config = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)?;
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(TlsAcceptor::from(Arc::new(config)))
}

/// A self-signed certificate for `localhost` and this machine, kept beside
/// the ban list so the browser's exception for it survives a restart. Returns
/// the certificate and key paths.
fn self_signed(settings: &AdminSettings) -> Result<(String, String), Box<dyn std::error::Error>> {
    let dir = Path::new(&settings.bans_file)
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."))
        .to_path_buf();
    let cert_path = dir.join("admin-selfsigned.crt");
    let key_path = dir.join("admin-selfsigned.key");
    if cert_path.is_file() && key_path.is_file() {
        return Ok((
            cert_path.to_string_lossy().into_owned(),
            key_path.to_string_lossy().into_owned(),
        ));
    }

    let mut names = vec![
        "localhost".to_string(),
        "127.0.0.1".to_string(),
        "::1".to_string(),
    ];
    for var in ["COMPUTERNAME", "HOSTNAME"] {
        if let Ok(host) = std::env::var(var) {
            if !host.trim().is_empty() && !names.contains(&host) {
                names.push(host);
            }
        }
    }
    if let Ok(addr) = settings.https_bind.parse::<SocketAddr>() {
        let ip = addr.ip().to_string();
        if !addr.ip().is_unspecified() && !names.contains(&ip) {
            names.push(ip);
        }
    }
    let certified = rcgen::generate_simple_self_signed(names)?;
    std::fs::create_dir_all(&dir)?;
    std::fs::write(&cert_path, certified.cert.pem())?;
    std::fs::write(&key_path, certified.signing_key.serialize_pem())?;
    restrict_to_owner(&key_path);
    info!(
        "Generated a self-signed certificate for the admin dashboard: {}",
        cert_path.display()
    );
    Ok((
        cert_path.to_string_lossy().into_owned(),
        key_path.to_string_lossy().into_owned(),
    ))
}

#[cfg(unix)]
fn restrict_to_owner(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
}

#[cfg(not(unix))]
fn restrict_to_owner(_path: &Path) {}

// ---------------------------------------------------------------------------
// Serving
// ---------------------------------------------------------------------------

async fn serve(listener: TcpListener, tls: Option<TlsAcceptor>, ctx: Arc<AdminContext>) {
    loop {
        let (stream, peer) = match listener.accept().await {
            Ok(c) => c,
            Err(e) => {
                warn!("Admin accept failed: {}", e);
                // Do not spin if the listener is broken (descriptor limit).
                tokio::time::sleep(Duration::from_millis(100)).await;
                continue;
            }
        };
        let ctx = Arc::clone(&ctx);
        let tls = tls.clone();
        tokio::spawn(async move {
            let secure = tls.is_some();
            if let Some(acceptor) = tls {
                match tokio::time::timeout(Duration::from_secs(10), acceptor.accept(stream)).await {
                    Ok(Ok(stream)) => serve_connection(stream, peer, secure, ctx).await,
                    Ok(Err(e)) => debug!("Admin TLS handshake with {} failed: {}", peer, e),
                    Err(_) => debug!("Admin TLS handshake with {} timed out", peer),
                }
            } else {
                serve_connection(stream, peer, secure, ctx).await;
            }
        });
    }
}

async fn serve_connection<S>(stream: S, peer: SocketAddr, secure: bool, ctx: Arc<AdminContext>)
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + 'static,
{
    let service = service_fn(move |req| {
        let ctx = Arc::clone(&ctx);
        async move { Ok::<_, hyper::Error>(handle(req, peer, secure, ctx).await) }
    });
    let result = http1::Builder::new()
        .timer(TokioTimer::new())
        .header_read_timeout(Duration::from_secs(10))
        .keep_alive(true)
        .serve_connection(TokioIo::new(stream), service)
        .await;
    if let Err(e) = result {
        debug!("Admin connection from {} ended: {}", peer, e);
    }
}

async fn handle(
    req: Request<Incoming>,
    peer: SocketAddr,
    secure: bool,
    ctx: Arc<AdminContext>,
) -> Resp {
    let mut resp = route(req, peer, secure, &ctx).await;
    let h = resp.headers_mut();
    h.insert(header::X_CONTENT_TYPE_OPTIONS, "nosniff".parse().unwrap());
    h.insert(header::X_FRAME_OPTIONS, "DENY".parse().unwrap());
    h.insert(header::REFERRER_POLICY, "no-referrer".parse().unwrap());
    h.insert(
        header::CONTENT_SECURITY_POLICY,
        "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; \
         frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
            .parse()
            .unwrap(),
    );
    if secure {
        h.insert(
            header::STRICT_TRANSPORT_SECURITY,
            "max-age=31536000".parse().unwrap(),
        );
    }
    resp
}

async fn route(req: Request<Incoming>, peer: SocketAddr, secure: bool, ctx: &AdminContext) -> Resp {
    let method = req.method().clone();
    let path = req.uri().path().to_string();

    // Plain HTTP can be told to send everyone to HTTPS.
    if !secure && ctx.settings.redirect_http_to_https {
        if let Some(https) = *ctx.https_addr.lock().unwrap_or_else(|e| e.into_inner()) {
            let host = req
                .headers()
                .get(header::HOST)
                .and_then(|h| h.to_str().ok())
                .map(|h| h.rsplit_once(':').map(|(h, _)| h).unwrap_or(h).to_string())
                .unwrap_or_else(|| "localhost".to_string());
            let query = req
                .uri()
                .query()
                .map(|q| format!("?{}", q))
                .unwrap_or_default();
            return Response::builder()
                .status(StatusCode::PERMANENT_REDIRECT)
                .header(
                    header::LOCATION,
                    format!("https://{}:{}{}{}", host, https.port(), path, query),
                )
                .body(Full::new(Bytes::new()))
                .unwrap();
        }
    }

    if !path.starts_with("/api/") {
        return if method == Method::GET || method == Method::HEAD {
            serve_asset(&path)
        } else {
            text(StatusCode::METHOD_NOT_ALLOWED, "Method Not Allowed")
        };
    }

    // A browser sends Origin on every cross-site write; refuse one that is
    // not this page's own origin. (SameSite=Strict on the cookie already
    // keeps it from being attached.)
    if method != Method::GET && !same_origin(&req) {
        return api::error(StatusCode::FORBIDDEN, "cross-origin request refused");
    }

    match (&method, path.as_str()) {
        (&Method::POST, "/api/login") => return login(req, peer, secure, ctx).await,
        (&Method::POST, "/api/logout") => return logout(&req, secure, ctx),
        _ => {}
    }

    if !authorised(&req, ctx) {
        return api::error(StatusCode::UNAUTHORIZED, "login required");
    }

    let query = req.uri().query().unwrap_or("").to_string();
    let body = if method == Method::POST || method == Method::PUT || method == Method::DELETE {
        match Limited::new(req.into_body(), MAX_BODY_BYTES)
            .collect()
            .await
        {
            Ok(b) => b.to_bytes(),
            Err(_) => return api::error(StatusCode::PAYLOAD_TOO_LARGE, "request body too large"),
        }
    } else {
        Bytes::new()
    };
    api::handle(ctx, &method, &path, &query, body).await
}

fn same_origin(req: &Request<Incoming>) -> bool {
    let Some(origin) = req.headers().get(header::ORIGIN) else {
        return true;
    };
    let Some(host) = req
        .headers()
        .get(header::HOST)
        .and_then(|h| h.to_str().ok())
    else {
        return false;
    };
    let Ok(origin) = origin.to_str() else {
        return false;
    };
    origin
        .strip_prefix("https://")
        .or_else(|| origin.strip_prefix("http://"))
        .is_some_and(|o| o.eq_ignore_ascii_case(host))
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

/// Compare without stopping at the first difference.
fn secrets_equal(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    let mut diff = (a.len() ^ b.len()) as u8;
    for i in 0..a.len().max(b.len()) {
        diff |= a.get(i).copied().unwrap_or(0) ^ b.get(i).copied().unwrap_or(0);
    }
    diff == 0
}

fn cookie_value<'a>(req: &'a Request<Incoming>, name: &str) -> Option<&'a str> {
    req.headers()
        .get_all(header::COOKIE)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .flat_map(|v| v.split(';'))
        .filter_map(|kv| kv.trim().split_once('='))
        .find(|(k, _)| *k == name)
        .map(|(_, v)| v)
}

const COOKIE: &str = "apex_admin";

fn authorised(req: &Request<Incoming>, ctx: &AdminContext) -> bool {
    if let Some(auth) = req
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|h| h.to_str().ok())
        .and_then(|h| h.strip_prefix("Bearer "))
    {
        return secrets_equal(auth.trim(), &ctx.token);
    }
    let Some(id) = cookie_value(req, COOKIE) else {
        return false;
    };
    let mut sessions = ctx.sessions.lock().unwrap_or_else(|e| e.into_inner());
    sessions.retain(|_, at| at.elapsed() < SESSION_TTL);
    sessions.contains_key(id)
}

async fn login(req: Request<Incoming>, peer: SocketAddr, secure: bool, ctx: &AdminContext) -> Resp {
    let ip = peer.ip();
    {
        let mut failures = ctx.failures.lock().unwrap_or_else(|e| e.into_inner());
        failures.retain(|_, (_, at)| at.elapsed() < LOGIN_LOCKOUT);
        if failures
            .get(&ip)
            .is_some_and(|(n, _)| *n >= MAX_LOGIN_FAILURES)
        {
            return api::error(
                StatusCode::TOO_MANY_REQUESTS,
                "too many wrong tokens: wait a minute and try again",
            );
        }
    }
    let body = match Limited::new(req.into_body(), 4096).collect().await {
        Ok(b) => b.to_bytes(),
        Err(_) => return api::error(StatusCode::BAD_REQUEST, "bad request"),
    };
    let supplied = serde_json::from_slice::<serde_json::Value>(&body)
        .ok()
        .and_then(|v| v.get("token").and_then(|t| t.as_str()).map(str::to_string))
        .unwrap_or_default();

    if supplied.is_empty() || !secrets_equal(&supplied, &ctx.token) {
        let mut failures = ctx.failures.lock().unwrap_or_else(|e| e.into_inner());
        let entry = failures.entry(ip).or_insert((0, Instant::now()));
        entry.0 += 1;
        entry.1 = Instant::now();
        warn!("Admin login from {} refused (wrong token)", ip);
        return api::error(StatusCode::UNAUTHORIZED, "wrong access token");
    }

    ctx.failures
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&ip);
    let id = generate_secret();
    ctx.sessions
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(id.clone(), Instant::now());
    info!("Admin login from {}", ip);
    let mut resp = api::json(StatusCode::OK, serde_json::json!({ "ok": true }));
    resp.headers_mut().insert(
        header::SET_COOKIE,
        format!(
            "{}={}; Path=/; HttpOnly; SameSite=Strict; Max-Age={}{}",
            COOKIE,
            id,
            SESSION_TTL.as_secs(),
            if secure { "; Secure" } else { "" }
        )
        .parse()
        .unwrap(),
    );
    resp
}

fn logout(req: &Request<Incoming>, secure: bool, ctx: &AdminContext) -> Resp {
    if let Some(id) = cookie_value(req, COOKIE) {
        ctx.sessions
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(id);
    }
    let mut resp = api::json(StatusCode::OK, serde_json::json!({ "ok": true }));
    resp.headers_mut().insert(
        header::SET_COOKIE,
        format!(
            "{}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0{}",
            COOKIE,
            if secure { "; Secure" } else { "" }
        )
        .parse()
        .unwrap(),
    );
    resp
}

// ---------------------------------------------------------------------------
// Static assets
// ---------------------------------------------------------------------------

fn text(status: StatusCode, body: &'static str) -> Resp {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
        .body(Full::new(Bytes::from_static(body.as_bytes())))
        .unwrap()
}

fn asset(content_type: &'static str, body: &'static [u8], cache: &'static str) -> Resp {
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, content_type)
        .header(header::CACHE_CONTROL, cache)
        .body(Full::new(Bytes::from_static(body)))
        .unwrap()
}

macro_rules! font {
    ($name:literal) => {
        asset(
            "font/woff2",
            include_bytes!(concat!("web/fonts/", $name, ".woff2")),
            "public, max-age=31536000, immutable",
        )
    };
}

fn serve_asset(path: &str) -> Resp {
    const NO_STORE: &str = "no-store";
    match path {
        "/" | "/index.html" => asset(
            "text/html; charset=utf-8",
            include_bytes!("web/index.html"),
            NO_STORE,
        ),
        "/app.js" => asset(
            "text/javascript; charset=utf-8",
            include_bytes!("web/app.js"),
            NO_STORE,
        ),
        "/app.css" => asset(
            "text/css; charset=utf-8",
            include_bytes!("web/app.css"),
            NO_STORE,
        ),
        "/fonts/barlow-400.woff2" => font!("barlow-400"),
        "/fonts/barlow-500.woff2" => font!("barlow-500"),
        "/fonts/barlow-600.woff2" => font!("barlow-600"),
        "/fonts/barlow-700.woff2" => font!("barlow-700"),
        "/fonts/plex-400.woff2" => font!("plex-400"),
        "/fonts/plex-500.woff2" => font!("plex-500"),
        _ => text(StatusCode::NOT_FOUND, "Not Found"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn secrets_compare_whole_values() {
        assert!(secrets_equal("abc", "abc"));
        assert!(!secrets_equal("abc", "abd"));
        assert!(!secrets_equal("abc", "abcd"));
        assert!(!secrets_equal("", "a"));
        assert!(secrets_equal("", ""));
    }

    #[test]
    fn generated_tokens_are_long_and_differ() {
        let (a, b) = (generate_secret(), generate_secret());
        assert_eq!(a.len(), 64);
        assert_ne!(a, b);
    }
}

use crate::metrics::ServerMetrics;
use bytes::Bytes;
use http_body_util::Full;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{body::Incoming, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use std::sync::Arc;
use tokio::net::TcpListener;

/// The server state the health port reads the replay folder from, when it
/// serves recordings (`[records] serve_replays`).
pub type RecordingsSource = Arc<RwLock<crate::server::ServerState>>;
use tokio::sync::RwLock;
use tracing::{debug, error, info};

fn text_response(status: StatusCode, body: &'static str) -> Response<Full<Bytes>> {
    Response::builder()
        .status(status)
        .body(Full::new(Bytes::from(body)))
        .expect("static response construction cannot fail")
}

#[derive(Clone)]
pub struct HealthState {
    pub is_ready: Arc<RwLock<bool>>,
    pub is_healthy: Arc<RwLock<bool>>,
}

impl HealthState {
    pub fn new() -> Self {
        Self {
            is_ready: Arc::new(RwLock::new(false)),
            is_healthy: Arc::new(RwLock::new(true)),
        }
    }

    pub async fn set_ready(&self, ready: bool) {
        *self.is_ready.write().await = ready;
    }

    pub async fn set_healthy(&self, healthy: bool) {
        *self.is_healthy.write().await = healthy;
    }
}

impl Default for HealthState {
    fn default() -> Self {
        Self::new()
    }
}

/// The replay folder and the car names to list it with.
async fn recordings_dir(
    source: &RecordingsSource,
) -> (
    std::path::PathBuf,
    std::collections::HashMap<crate::data::CarConfigId, String>,
) {
    let st = source.read().await;
    let names = st
        .car_configs
        .iter()
        .map(|(id, c)| (*id, c.name.clone()))
        .collect();
    (st.replay.dir().to_path_buf(), names)
}

/// `/recordings`: the finished sessions as JSON; `/recordings/<file>`: one's
/// stream, for a client's Replays screen.
async fn handle_recordings(path: &str, source: &RecordingsSource) -> Response<Full<Bytes>> {
    let (dir, names) = recordings_dir(source).await;
    if path == "/recordings" {
        let rows =
            tokio::task::spawn_blocking(move || crate::stream_recorder::history(&dir, &names))
                .await
                .unwrap_or_default();
        return Response::builder()
            .status(StatusCode::OK)
            .header("Content-Type", "application/json")
            .body(Full::new(Bytes::from(
                serde_json::json!({ "sessions": rows }).to_string(),
            )))
            .expect("recordings response construction cannot fail");
    }
    let name = path.trim_start_matches("/recordings/");
    if !crate::stream_recorder::is_served_stream(name) {
        return text_response(StatusCode::NOT_FOUND, "Not Found");
    }
    match tokio::fs::read(dir.join(name)).await {
        Ok(bytes) => Response::builder()
            .status(StatusCode::OK)
            .header("Content-Type", "application/octet-stream")
            .body(Full::new(Bytes::from(bytes)))
            .expect("recording response construction cannot fail"),
        Err(_) => text_response(StatusCode::NOT_FOUND, "Not Found"),
    }
}

async fn handle_health(
    req: Request<Incoming>,
    health_state: HealthState,
    metrics: Option<Arc<ServerMetrics>>,
    recordings: Option<RecordingsSource>,
) -> Result<Response<Full<Bytes>>, hyper::Error> {
    let path = req.uri().path();
    if path == "/recordings" || path.starts_with("/recordings/") {
        return Ok(match recordings {
            Some(source) => handle_recordings(path, &source).await,
            None => text_response(StatusCode::NOT_FOUND, "Not Found"),
        });
    }

    match path {
        "/health" => {
            let is_healthy = *health_state.is_healthy.read().await;
            if is_healthy {
                Ok(text_response(StatusCode::OK, "OK"))
            } else {
                Ok(text_response(
                    StatusCode::SERVICE_UNAVAILABLE,
                    "Service Unavailable",
                ))
            }
        }
        "/ready" => {
            let is_ready = *health_state.is_ready.read().await;
            if is_ready {
                Ok(text_response(StatusCode::OK, "Ready"))
            } else {
                Ok(text_response(StatusCode::SERVICE_UNAVAILABLE, "Not Ready"))
            }
        }
        "/metrics" => match metrics {
            Some(m) => Ok(Response::builder()
                .status(StatusCode::OK)
                .header("Content-Type", "text/plain; version=0.0.4")
                .body(Full::new(Bytes::from(m.render_prometheus())))
                .expect("metrics response construction cannot fail")),
            None => Ok(text_response(StatusCode::NOT_FOUND, "Not Found")),
        },
        // The showcase channels and how many watch each (`crate::showcase`).
        "/showcase" => match metrics {
            Some(m) => Ok(Response::builder()
                .status(StatusCode::OK)
                .header("Content-Type", "application/json")
                .body(Full::new(Bytes::from(m.render_showcases_json())))
                .expect("showcase response construction cannot fail")),
            None => Ok(text_response(StatusCode::NOT_FOUND, "Not Found")),
        },
        _ => Ok(text_response(StatusCode::NOT_FOUND, "Not Found")),
    }
}

pub async fn run_health_server(
    bind_addr: String,
    health_state: HealthState,
    metrics: Option<Arc<ServerMetrics>>,
) -> Result<(), std::io::Error> {
    debug!("Starting health check server on {}", bind_addr);
    let listener = TcpListener::bind(&bind_addr).await?;
    serve_health(listener, health_state, metrics, None).await
}

/// Serve health/readiness/metrics on an already-bound listener (lets the
/// caller learn the ephemeral port before serving starts).
pub async fn serve_health(
    listener: TcpListener,
    health_state: HealthState,
    metrics: Option<Arc<ServerMetrics>>,
    recordings: Option<RecordingsSource>,
) -> Result<(), std::io::Error> {
    info!(
        "Health check server listening on {}",
        listener.local_addr()?
    );

    loop {
        let (stream, _) = match listener.accept().await {
            Ok(conn) => conn,
            Err(e) => {
                error!("Failed to accept health check connection: {}", e);
                continue;
            }
        };

        let io = TokioIo::new(stream);
        let health_state = health_state.clone();
        let metrics = metrics.clone();
        let recordings = recordings.clone();

        tokio::spawn(async move {
            let service = service_fn(move |req| {
                handle_health(
                    req,
                    health_state.clone(),
                    metrics.clone(),
                    recordings.clone(),
                )
            });

            if let Err(err) = http1::Builder::new().serve_connection(io, service).await {
                error!("Error serving health check connection: {}", err);
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_health_state_creation() {
        let state = HealthState::new();
        assert!(!*state.is_ready.read().await);
        assert!(*state.is_healthy.read().await);
    }

    #[tokio::test]
    async fn test_health_state_updates() {
        let state = HealthState::new();

        state.set_ready(true).await;
        assert!(*state.is_ready.read().await);

        state.set_healthy(false).await;
        assert!(!*state.is_healthy.read().await);
    }
}

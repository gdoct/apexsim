use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{body::Incoming, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use http_body_util::Full;
use bytes::Bytes;
use tokio::net::TcpListener;
use std::sync::Arc;
use tokio::sync::RwLock;
use tracing::{info, error};

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

async fn handle_health(
    req: Request<Incoming>,
    health_state: HealthState,
) -> Result<Response<Full<Bytes>>, hyper::Error> {
    let path = req.uri().path();

    match path {
        "/health" => {
            let is_healthy = *health_state.is_healthy.read().await;
            if is_healthy {
                Ok(Response::builder()
                    .status(StatusCode::OK)
                    .body(Full::new(Bytes::from("OK")))
                    .unwrap())
            } else {
                Ok(Response::builder()
                    .status(StatusCode::SERVICE_UNAVAILABLE)
                    .body(Full::new(Bytes::from("Service Unavailable")))
                    .unwrap())
            }
        }
        "/ready" => {
            let is_ready = *health_state.is_ready.read().await;
            if is_ready {
                Ok(Response::builder()
                    .status(StatusCode::OK)
                    .body(Full::new(Bytes::from("Ready")))
                    .unwrap())
            } else {
                Ok(Response::builder()
                    .status(StatusCode::SERVICE_UNAVAILABLE)
                    .body(Full::new(Bytes::from("Not Ready")))
                    .unwrap())
            }
        }
        _ => {
            Ok(Response::builder()
                .status(StatusCode::NOT_FOUND)
                .body(Full::new(Bytes::from("Not Found")))
                .unwrap())
        }
    }
}

pub async fn run_health_server(bind_addr: String, health_state: HealthState) -> Result<(), Box<dyn std::error::Error>> {
    info!("Starting health check server on {}", bind_addr);

    let listener = TcpListener::bind(&bind_addr).await?;
    info!("Health check server listening on {}", bind_addr);

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

        tokio::spawn(async move {
            let service = service_fn(move |req| {
                handle_health(req, health_state.clone())
            });

            if let Err(err) = http1::Builder::new()
                .serve_connection(io, service)
                .await
            {
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

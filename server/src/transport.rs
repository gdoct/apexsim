use crate::config::{AuthMode, AuthSettings};
use crate::data::*;
use crate::network::{
    AuthSuccessData, ClientMessage, MessagePriority, ServerMessage, PROTOCOL_VERSION,
};
use bytes::Bytes;
use rustls::pki_types::CertificateDer;
use rustls::ServerConfig as TlsConfig;
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use thiserror::Error;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream, UdpSocket};
use tokio::sync::mpsc;
use tokio::sync::RwLock;
use tokio_rustls::TlsAcceptor;
use tracing::{debug, error, info, warn};
use uuid::Uuid;

// Channel capacity constants
const TCP_INBOUND_CHANNEL_SIZE: usize = 1000;
const UDP_OUTBOUND_CHANNEL_SIZE: usize = 2000;
const PER_CLIENT_TCP_CHANNEL_SIZE: usize = 100;

// Per-connection rate limits (see SPEC §6). PlayerInput is high-frequency;
// everything else is lobby/control traffic and should be rare.
const INPUT_RATE_PER_SEC: f32 = 300.0;
const INPUT_RATE_BURST: f32 = 60.0;
const CONTROL_RATE_PER_SEC: f32 = 10.0;
const CONTROL_RATE_BURST: f32 = 20.0;
/// Disconnect a connection after this many rate-limit/protocol violations.
const MAX_VIOLATIONS: u32 = 200;

/// Minimal token bucket: refills continuously, allows short bursts.
struct TokenBucket {
    tokens: f32,
    burst: f32,
    rate_per_sec: f32,
    last_refill: Instant,
}

impl TokenBucket {
    fn new(rate_per_sec: f32, burst: f32) -> Self {
        Self {
            tokens: burst,
            burst,
            rate_per_sec,
            last_refill: Instant::now(),
        }
    }

    fn try_take(&mut self) -> bool {
        let now = Instant::now();
        let elapsed = now.duration_since(self.last_refill).as_secs_f32();
        self.last_refill = now;
        self.tokens = (self.tokens + elapsed * self.rate_per_sec).min(self.burst);
        if self.tokens >= 1.0 {
            self.tokens -= 1.0;
            true
        } else {
            false
        }
    }
}

/// Metrics for tracking dropped messages
#[derive(Debug, Default, Clone)]
pub struct TransportMetrics {
    pub tcp_messages_dropped: Arc<AtomicU64>,
    pub udp_messages_dropped: Arc<AtomicU64>,
    pub clients_disconnected_backpressure: Arc<AtomicU64>,
}

impl TransportMetrics {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn tcp_dropped(&self) -> u64 {
        self.tcp_messages_dropped.load(Ordering::Relaxed)
    }

    pub fn udp_dropped(&self) -> u64 {
        self.udp_messages_dropped.load(Ordering::Relaxed)
    }

    pub fn clients_disconnected(&self) -> u64 {
        self.clients_disconnected_backpressure
            .load(Ordering::Relaxed)
    }
}

#[derive(Debug, Error)]
pub enum TransportError {
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),
    #[error("Serialization error: {0}")]
    Serialization(#[from] rmp_serde::encode::Error),
    #[error("Deserialization error: {0}")]
    Deserialization(#[from] rmp_serde::decode::Error),
    #[error("TLS error: {0}")]
    Tls(#[from] rustls::Error),
    #[error("Connection not found")]
    ConnectionNotFound,
    #[error("Invalid message")]
    InvalidMessage,
    #[error("Queue full - client too slow")]
    QueueFull,
}

/// Outbound frame for a per-connection writer task. `Message` is serialized
/// by the writer; `Serialized` carries pre-encoded bytes so a broadcast can
/// serialize once and fan out cheap `Bytes` clones.
#[derive(Debug, Clone)]
pub enum OutboundFrame {
    Message(ServerMessage),
    Serialized {
        data: Bytes,
        priority: MessagePriority,
    },
}

/// Inbound event delivered from the transport layer to the game loop.
#[derive(Debug)]
pub enum TransportEvent {
    /// A client message received on an established connection.
    Message(ConnectionId, ClientMessage),
    /// The connection was torn down (stream closed). Carries everything the
    /// game loop needs, captured before the connection maps were cleaned up.
    Disconnected {
        connection_id: ConnectionId,
        player_id: PlayerId,
        session_id: Option<SessionId>,
    },
}

#[derive(Debug, Clone)]
pub struct ConnectionInfo {
    pub player_id: PlayerId,
    pub player_name: String,
    pub connected_at: Instant,
    pub last_heartbeat: Instant,
    pub tcp_addr: SocketAddr,
    pub tcp_tx: mpsc::Sender<OutboundFrame>,
    pub in_session: Option<SessionId>,
    /// Token issued in `AuthSuccess`; presenting it in a `UdpHandshake`
    /// binds the sender's UDP address to this connection.
    pub udp_token: String,
    /// Bound UDP address, set by a completed `UdpHandshake`. Telemetry is
    /// sent here when present (TCP fallback otherwise).
    pub udp_addr: Option<SocketAddr>,
}

/// Shared connection registry: every map needed to resolve a connection from
/// a TCP address, player ID, UDP token or UDP address. Cheap to clone (Arcs).
#[derive(Clone)]
struct ConnRegistry {
    connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
    player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
    udp_token_to_connection: Arc<RwLock<HashMap<String, ConnectionId>>>,
    udp_addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
}

impl ConnRegistry {
    fn new() -> Self {
        Self {
            connections: Arc::new(RwLock::new(HashMap::new())),
            player_to_connection: Arc::new(RwLock::new(HashMap::new())),
            addr_to_connection: Arc::new(RwLock::new(HashMap::new())),
            udp_token_to_connection: Arc::new(RwLock::new(HashMap::new())),
            udp_addr_to_connection: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    /// Remove a connection from every map. Returns the removed info.
    async fn remove_connection(&self, connection_id: ConnectionId) -> Option<ConnectionInfo> {
        let conn = self.connections.write().await.remove(&connection_id)?;
        self.addr_to_connection.write().await.remove(&conn.tcp_addr);
        self.player_to_connection
            .write()
            .await
            .remove(&conn.player_id);
        self.udp_token_to_connection
            .write()
            .await
            .remove(&conn.udp_token);
        if let Some(udp_addr) = conn.udp_addr {
            self.udp_addr_to_connection.write().await.remove(&udp_addr);
        }
        Some(conn)
    }
}

pub struct TransportLayer {
    // Connection tracking
    registry: ConnRegistry,

    // Network sockets
    tcp_listener: Option<TcpListener>,
    udp_socket: Arc<UdpSocket>,
    tls_acceptor: Option<TlsAcceptor>,
    tcp_local_addr: SocketAddr,
    udp_local_addr: SocketAddr,

    // Channels for communication (bounded). `event_rx` can be taken out of
    // the transport (see `take_event_receiver`) so the game loop can drain
    // inbound events without holding any transport lock. Both TCP reads and
    // handshake-bound UDP packets feed this channel.
    event_rx: Option<mpsc::Receiver<TransportEvent>>,
    event_tx: mpsc::Sender<TransportEvent>,

    // Outbound UDP queue (TCP uses per-connection channels). Carries
    // pre-serialized bytes so a broadcast can serialize once and fan out.
    udp_out_tx: mpsc::Sender<(SocketAddr, Bytes)>,
    udp_out_rx: mpsc::Receiver<(SocketAddr, Bytes)>,

    // Shutdown channel (unbounded - low volume, critical)
    shutdown_tx: mpsc::UnboundedSender<()>,
    #[allow(dead_code)]
    shutdown_rx: Option<mpsc::UnboundedReceiver<()>>,

    heartbeat_timeout: Duration,

    // Token validation policy for `Authenticate`
    auth: AuthSettings,

    // Metrics
    pub metrics: TransportMetrics,
}

impl TransportLayer {
    pub async fn new(
        tcp_bind: &str,
        udp_bind: &str,
        tls_cert_path: &str,
        tls_key_path: &str,
        require_tls: bool,
        heartbeat_timeout_ms: u64,
        auth: AuthSettings,
    ) -> Result<Self, TransportError> {
        if auth.mode == AuthMode::Dev {
            warn!("Auth mode: dev — all tokens accepted. Do not use in production.");
        } else if auth.tokens.is_empty() {
            warn!("Auth mode: token, but no tokens configured — every client will be rejected");
        }
        // Setup TCP with TLS
        let tcp_listener = TcpListener::bind(tcp_bind).await?;
        let tcp_local_addr = tcp_listener.local_addr()?;
        debug!("TCP listener bound to {}", tcp_local_addr);

        // Setup UDP
        let udp_socket = Arc::new(UdpSocket::bind(udp_bind).await?);
        let udp_local_addr = udp_socket.local_addr()?;
        debug!("UDP socket bound to {}", udp_local_addr);

        // No certificate paths configured at all: TLS is deliberately off
        // (the development default). Only an announcement is warranted — the
        // warning below is for paths that are set but unusable.
        let tls_configured = !tls_cert_path.trim().is_empty() && !tls_key_path.trim().is_empty();
        if !tls_configured {
            if require_tls {
                error!("✗ FATAL: network.require_tls = true but no TLS certificate/key paths are configured");
                return Err(TransportError::Io(std::io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    "TLS is required but network.tls_cert_path/tls_key_path are empty",
                )));
            }
            info!("TLS disabled: no certificate configured, accepting plaintext connections");
        }

        // Load TLS configuration
        let tls_acceptor = match Self::load_tls_config(tls_cert_path, tls_key_path) {
            Ok(config) => {
                debug!("✓ TLS configuration loaded successfully");
                debug!("  Certificate: {}", tls_cert_path);
                debug!("  Private key: {}", tls_key_path);
                if require_tls {
                    debug!("  TLS mode: REQUIRED (enforcement: fail if unavailable)");
                } else {
                    debug!("  TLS mode: ENABLED (enforcement: optional, currently active)");
                }
                Some(TlsAcceptor::from(Arc::new(config)))
            }
            Err(e) => {
                if require_tls {
                    error!("✗ FATAL: TLS is required but failed to load");
                    error!("  Certificate path: {}", tls_cert_path);
                    error!("  Private key path: {}", tls_key_path);
                    error!("  Error: {}", e);
                    error!("  Set network.require_tls = false in config to allow plaintext connections");
                    return Err(e);
                } else if tls_configured {
                    warn!("⚠ TLS configuration failed to load: {}", e);
                    warn!("  Certificate path: {}", tls_cert_path);
                    warn!("  Private key path: {}", tls_key_path);
                    warn!("  TLS mode: OPTIONAL (accepting plaintext connections)");
                    warn!("  For production, set network.require_tls = true and provide valid certificates");
                    None
                } else {
                    // Already announced above: TLS is off by configuration.
                    None
                }
            }
        };

        // Create bounded channels
        let (event_tx, event_rx) = mpsc::channel(TCP_INBOUND_CHANNEL_SIZE);
        let (udp_out_tx, udp_out_rx) = mpsc::channel(UDP_OUTBOUND_CHANNEL_SIZE);
        let (shutdown_tx, shutdown_rx) = mpsc::unbounded_channel();

        Ok(Self {
            registry: ConnRegistry::new(),
            tcp_listener: Some(tcp_listener),
            udp_socket,
            tls_acceptor,
            tcp_local_addr,
            udp_local_addr,
            event_rx: Some(event_rx),
            event_tx,
            udp_out_tx,
            udp_out_rx,
            shutdown_tx,
            shutdown_rx: Some(shutdown_rx),
            heartbeat_timeout: Duration::from_millis(heartbeat_timeout_ms),
            auth,
            metrics: TransportMetrics::new(),
        })
    }

    fn load_tls_config(cert_path: &str, key_path: &str) -> Result<TlsConfig, TransportError> {
        // Load certificates
        let cert_file = File::open(cert_path)?;
        let mut cert_reader = BufReader::new(cert_file);
        let certs: Vec<CertificateDer> =
            rustls_pemfile::certs(&mut cert_reader).collect::<Result<Vec<_>, _>>()?;

        if certs.is_empty() {
            return Err(TransportError::Io(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "No certificates found in cert file",
            )));
        }

        // Load private key
        let key_file = File::open(key_path)?;
        let mut key_reader = BufReader::new(key_file);
        let key = rustls_pemfile::private_key(&mut key_reader)?.ok_or_else(|| {
            TransportError::Io(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "No private key found in key file",
            ))
        })?;

        let config = TlsConfig::builder()
            .with_no_client_auth()
            .with_single_cert(certs, key)?;

        Ok(config)
    }

    pub async fn start(&mut self) {
        // Spawn TCP acceptor
        if let Some(listener) = self.tcp_listener.take() {
            let event_tx = self.event_tx.clone();
            let tls_acceptor = self.tls_acceptor.clone();
            let registry = self.registry.clone();
            let auth = self.auth.clone();
            let udp_port = self.udp_local_addr.port();

            tokio::spawn(async move {
                Self::tcp_acceptor(listener, event_tx, tls_acceptor, registry, auth, udp_port)
                    .await;
            });
        }

        // Spawn UDP receiver (resolves handshakes and routes bound-address
        // messages into the shared event channel)
        let udp_socket = Arc::clone(&self.udp_socket);
        let registry = self.registry.clone();
        let event_tx = self.event_tx.clone();
        let udp_out_tx = self.udp_out_tx.clone();
        tokio::spawn(async move {
            Self::udp_receiver(udp_socket, registry, event_tx, udp_out_tx).await;
        });

        // Spawn UDP sender
        let udp_socket = Arc::clone(&self.udp_socket);
        let mut udp_out_rx = std::mem::replace(&mut self.udp_out_rx, mpsc::channel(1).1);
        let metrics = self.metrics.clone();
        tokio::spawn(async move {
            Self::udp_sender(udp_socket, &mut udp_out_rx, metrics).await;
        });
    }

    async fn tcp_acceptor(
        listener: TcpListener,
        event_tx: mpsc::Sender<TransportEvent>,
        tls_acceptor: Option<TlsAcceptor>,
        registry: ConnRegistry,
        auth: AuthSettings,
        udp_port: u16,
    ) {
        loop {
            match listener.accept().await {
                Ok((stream, addr)) => {
                    debug!("New TCP connection from {}", addr);
                    let event_tx = event_tx.clone();
                    let tls_acceptor = tls_acceptor.clone();
                    let registry = registry.clone();
                    let auth = auth.clone();

                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_tcp_connection(
                            stream,
                            addr,
                            event_tx,
                            tls_acceptor,
                            registry,
                            auth,
                            udp_port,
                        )
                        .await
                        {
                            error!("TCP connection error: {}", e);
                        }
                    });
                }
                Err(e) => {
                    error!("Failed to accept TCP connection: {}", e);
                }
            }
        }
    }

    async fn handle_tcp_connection(
        stream: TcpStream,
        addr: SocketAddr,
        event_tx: mpsc::Sender<TransportEvent>,
        tls_acceptor: Option<TlsAcceptor>,
        registry: ConnRegistry,
        auth: AuthSettings,
        udp_port: u16,
    ) -> Result<(), TransportError> {
        // Generate unique connection ID
        let connection_id = Uuid::new_v4();

        // Create per-connection send channel (BOUNDED)
        let (conn_tx, conn_rx) = mpsc::channel::<OutboundFrame>(PER_CLIENT_TCP_CHANNEL_SIZE);

        // Handle TLS if available
        if let Some(acceptor) = tls_acceptor {
            match acceptor.accept(stream).await {
                Ok(tls_stream) => {
                    debug!("TLS connection established for {}", addr);
                    Self::handle_stream(
                        tls_stream,
                        addr,
                        connection_id,
                        conn_tx,
                        conn_rx,
                        event_tx,
                        registry,
                        auth,
                        udp_port,
                    )
                    .await
                }
                Err(e) => {
                    error!("TLS handshake failed for {}: {}", addr, e);
                    Err(TransportError::Io(std::io::Error::other(format!(
                        "TLS handshake failed: {}",
                        e
                    ))))
                }
            }
        } else {
            // Non-TLS connection
            Self::handle_stream(
                stream,
                addr,
                connection_id,
                conn_tx,
                conn_rx,
                event_tx,
                registry,
                auth,
                udp_port,
            )
            .await
        }
    }

    #[allow(clippy::too_many_arguments)]
    async fn handle_stream<S>(
        stream: S,
        addr: SocketAddr,
        connection_id: ConnectionId,
        conn_tx: mpsc::Sender<OutboundFrame>,
        mut conn_rx: mpsc::Receiver<OutboundFrame>,
        event_tx: mpsc::Sender<TransportEvent>,
        registry: ConnRegistry,
        auth: AuthSettings,
        udp_port: u16,
    ) -> Result<(), TransportError>
    where
        S: AsyncReadExt + AsyncWriteExt + Unpin + Send + 'static,
    {
        // Split into reader and writer
        let (mut reader, mut writer) = tokio::io::split(stream);

        // Spawn writer task
        let writer_addr = addr;
        tokio::spawn(async move {
            while let Some(frame) = conn_rx.recv().await {
                // Message frames are serialized here; Serialized frames carry
                // pre-encoded bytes (same rmp_serde named encoding) already.
                let data: Bytes = match frame {
                    OutboundFrame::Message(msg) => match rmp_serde::to_vec_named(&msg) {
                        Ok(data) => Bytes::from(data),
                        Err(e) => {
                            error!("Failed to serialize message: {}", e);
                            break;
                        }
                    },
                    OutboundFrame::Serialized { data, .. } => data,
                };
                // Write length prefix (4 bytes) then data
                let len = data.len() as u32;
                if writer.write_all(&len.to_be_bytes()).await.is_err() {
                    break;
                }
                if writer.write_all(&data).await.is_err() {
                    break;
                }
                if writer.flush().await.is_err() {
                    break;
                }
            }
            debug!("Writer task closed for {}", writer_addr);
        });

        // Reader task (runs in this function)
        // Read with length-prefix framing
        let mut len_buf = [0u8; 4];
        let mut authenticated = false;
        let mut input_bucket = TokenBucket::new(INPUT_RATE_PER_SEC, INPUT_RATE_BURST);
        let mut control_bucket = TokenBucket::new(CONTROL_RATE_PER_SEC, CONTROL_RATE_BURST);
        let mut violations: u32 = 0;

        loop {
            // Read length prefix
            match reader.read_exact(&mut len_buf).await {
                Ok(_) => {
                    let len = u32::from_be_bytes(len_buf) as usize;

                    // Sanity check to prevent memory exhaustion
                    if len > 1_000_000 {
                        // 1MB max message size
                        warn!("Message too large from {}: {} bytes", addr, len);
                        break;
                    }

                    // Read message data
                    let mut msg_buf = vec![0u8; len];
                    match reader.read_exact(&mut msg_buf).await {
                        Ok(_) => {
                            match rmp_serde::from_slice::<ClientMessage>(&msg_buf) {
                                Ok(msg) => {
                                    // Rate limiting: high-frequency input gets its own
                                    // (generous) bucket; control traffic a strict one.
                                    let bucket = match &msg {
                                        ClientMessage::PlayerInput { .. }
                                        | ClientMessage::Heartbeat { .. } => &mut input_bucket,
                                        _ => &mut control_bucket,
                                    };
                                    if !bucket.try_take() {
                                        violations += 1;
                                        if violations >= MAX_VIOLATIONS {
                                            warn!(
                                                "Disconnecting {} after {} rate-limit violations",
                                                addr, violations
                                            );
                                            break;
                                        }
                                        if violations % 50 == 1 {
                                            warn!(
                                                "Rate limiting {} ({} violations)",
                                                addr, violations
                                            );
                                        }
                                        continue;
                                    }

                                    // Everything except Authenticate requires a
                                    // completed authentication first.
                                    if !authenticated
                                        && !matches!(&msg, ClientMessage::Authenticate { .. })
                                    {
                                        violations += 10;
                                        warn!(
                                            "Dropping pre-auth message from {}: {:?}",
                                            addr,
                                            std::mem::discriminant(&msg)
                                        );
                                        if violations >= MAX_VIOLATIONS {
                                            break;
                                        }
                                        continue;
                                    }

                                    // Handle authentication - register connection
                                    if let ClientMessage::Authenticate {
                                        token,
                                        player_name,
                                        protocol_version,
                                    } = &msg
                                    {
                                        if *protocol_version != PROTOCOL_VERSION {
                                            warn!(
                                                "Rejecting authentication from {} (protocol version {} != server {})",
                                                addr, protocol_version, PROTOCOL_VERSION
                                            );
                                            let _ = conn_tx
                                                .send(OutboundFrame::Message(
                                                    ServerMessage::AuthFailure {
                                                        reason: format!(
                                                            "protocol version mismatch: server speaks v{}, client sent v{} — please update your client",
                                                            PROTOCOL_VERSION, protocol_version
                                                        ),
                                                    },
                                                ))
                                                .await;
                                            break;
                                        }
                                        let token_ok = match auth.mode {
                                            AuthMode::Dev => true,
                                            AuthMode::Token => {
                                                !token.is_empty()
                                                    && auth.tokens.iter().any(|t| t == token)
                                            }
                                        };
                                        if !token_ok {
                                            warn!(
                                                "Rejecting authentication from {} (invalid token)",
                                                addr
                                            );
                                            let _ = conn_tx
                                                .send(OutboundFrame::Message(
                                                    ServerMessage::AuthFailure {
                                                        reason: "invalid token".to_string(),
                                                    },
                                                ))
                                                .await;
                                            break;
                                        }
                                        authenticated = true;
                                        let player_id = Uuid::new_v4();
                                        let udp_token = Uuid::new_v4().to_string();
                                        let conn_info = ConnectionInfo {
                                            player_id,
                                            player_name: player_name.clone(),
                                            connected_at: Instant::now(),
                                            last_heartbeat: Instant::now(),
                                            tcp_addr: addr,
                                            tcp_tx: conn_tx.clone(),
                                            in_session: None,
                                            udp_token: udp_token.clone(),
                                            udp_addr: None,
                                        };

                                        registry
                                            .connections
                                            .write()
                                            .await
                                            .insert(connection_id, conn_info.clone());
                                        registry
                                            .addr_to_connection
                                            .write()
                                            .await
                                            .insert(addr, connection_id);
                                        // Also track player_id -> connection_id mapping for broadcast lookups
                                        registry
                                            .player_to_connection
                                            .write()
                                            .await
                                            .insert(player_id, connection_id);
                                        registry
                                            .udp_token_to_connection
                                            .write()
                                            .await
                                            .insert(udp_token.clone(), connection_id);
                                        debug!(
                                            "Player {} authenticated as {} (connection: {})",
                                            player_name, player_id, connection_id
                                        );

                                        // Send auth success response
                                        let response =
                                            ServerMessage::AuthSuccess(AuthSuccessData {
                                                player_id,
                                                server_version: 1,
                                                protocol_version: PROTOCOL_VERSION,
                                                udp_token,
                                                udp_port,
                                            });
                                        // Critical message - if queue full, client is too slow
                                        if conn_tx
                                            .send(OutboundFrame::Message(response))
                                            .await
                                            .is_err()
                                        {
                                            warn!("Failed to send AuthSuccess to slow client {}, disconnecting", addr);
                                            break;
                                        }
                                    } else if let ClientMessage::Heartbeat { .. } = &msg {
                                        // Update last heartbeat time
                                        if let Some(conn) = registry
                                            .connections
                                            .write()
                                            .await
                                            .get_mut(&connection_id)
                                        {
                                            conn.last_heartbeat = Instant::now();
                                        }

                                        // Send heartbeat ack (droppable - can be skipped if queue full)
                                        let response = ServerMessage::HeartbeatAck {
                                            server_tick: 0, // Will be updated later with actual tick
                                        };
                                        let _ = conn_tx.try_send(OutboundFrame::Message(response));
                                    }

                                    if event_tx
                                        .send(TransportEvent::Message(connection_id, msg))
                                        .await
                                        .is_err()
                                    {
                                        error!("Failed to send message to handler");
                                        break;
                                    }
                                }
                                Err(e) => {
                                    warn!("Failed to deserialize message from {}: {}", addr, e);
                                }
                            }
                        }
                        Err(e) => {
                            debug!("Failed to read message data from {}: {}", addr, e);
                            break;
                        }
                    }
                }
                Err(e) => {
                    if e.kind() != std::io::ErrorKind::UnexpectedEof {
                        debug!("Connection closed by {}: {}", addr, e);
                    } else {
                        debug!("Connection closed by client: {}", addr);
                    }
                    break;
                }
            }
        }

        // Capture the connection info, clean up the maps immediately, then
        // notify the game loop with everything it needs to remove the player.
        // No sleep, no lookup race: the event carries the captured info.
        if let Some(conn) = registry.remove_connection(connection_id).await {
            debug!(
                "Connection cleaned up: {} (player: {}, session: {:?})",
                addr, conn.player_name, conn.in_session
            );
            let _ = event_tx
                .send(TransportEvent::Disconnected {
                    connection_id,
                    player_id: conn.player_id,
                    session_id: conn.in_session,
                })
                .await;
        }

        Ok(())
    }

    /// Receive UDP datagrams. `UdpHandshake` binds the sender's address to
    /// the connection that owns the presented token (and is acked over UDP);
    /// all other messages are only accepted from bound addresses and are
    /// forwarded into the shared game-loop event channel.
    async fn udp_receiver(
        socket: Arc<UdpSocket>,
        registry: ConnRegistry,
        event_tx: mpsc::Sender<TransportEvent>,
        udp_out_tx: mpsc::Sender<(SocketAddr, Bytes)>,
    ) {
        let mut buf = vec![0u8; 2048];
        // Per-address rate limiting, mirroring the TCP input bucket.
        let mut buckets: HashMap<SocketAddr, TokenBucket> = HashMap::new();
        loop {
            match socket.recv_from(&mut buf).await {
                Ok((n, addr)) => {
                    // Defensive bound on the bucket map: drop it entirely if
                    // it somehow grows past any realistic client count.
                    if buckets.len() > 4096 {
                        buckets.clear();
                    }
                    let bucket = buckets
                        .entry(addr)
                        .or_insert_with(|| TokenBucket::new(INPUT_RATE_PER_SEC, INPUT_RATE_BURST));
                    if !bucket.try_take() {
                        continue;
                    }

                    let msg = match rmp_serde::from_slice::<ClientMessage>(&buf[..n]) {
                        Ok(msg) => msg,
                        Err(e) => {
                            debug!("Failed to deserialize UDP message from {}: {}", addr, e);
                            continue;
                        }
                    };

                    match msg {
                        ClientMessage::UdpHandshake { token } => {
                            let conn_id = registry
                                .udp_token_to_connection
                                .read()
                                .await
                                .get(&token)
                                .copied();
                            let Some(conn_id) = conn_id else {
                                debug!("UDP handshake from {} with unknown token", addr);
                                continue;
                            };
                            // Bind (or re-bind) the sender's address.
                            let old_addr = {
                                let mut connections = registry.connections.write().await;
                                match connections.get_mut(&conn_id) {
                                    Some(conn) => conn.udp_addr.replace(addr),
                                    None => continue,
                                }
                            };
                            {
                                let mut udp_addrs = registry.udp_addr_to_connection.write().await;
                                if let Some(old) = old_addr {
                                    if old != addr {
                                        udp_addrs.remove(&old);
                                    }
                                }
                                udp_addrs.insert(addr, conn_id);
                            }
                            debug!("UDP address {} bound to connection {}", addr, conn_id);
                            match rmp_serde::to_vec_named(&ServerMessage::UdpHandshakeAck) {
                                Ok(ack) => {
                                    let _ = udp_out_tx.try_send((addr, Bytes::from(ack)));
                                }
                                Err(e) => error!("Failed to serialize UdpHandshakeAck: {}", e),
                            }
                        }
                        msg => {
                            // Identity is derived from the bound source
                            // address; unbound addresses are dropped.
                            let conn_id = registry
                                .udp_addr_to_connection
                                .read()
                                .await
                                .get(&addr)
                                .copied();
                            let Some(conn_id) = conn_id else {
                                debug!("Dropping UDP message from unbound address {}", addr);
                                continue;
                            };
                            if event_tx
                                .send(TransportEvent::Message(conn_id, msg))
                                .await
                                .is_err()
                            {
                                error!("UDP receiver event channel closed");
                                return;
                            }
                        }
                    }
                }
                Err(e) => {
                    debug!("UDP receive error: {}", e);
                }
            }
        }
    }

    async fn udp_sender(
        socket: Arc<UdpSocket>,
        rx: &mut mpsc::Receiver<(SocketAddr, Bytes)>,
        _metrics: TransportMetrics,
    ) {
        while let Some((addr, data)) = rx.recv().await {
            if let Err(e) = socket.send_to(&data, addr).await {
                debug!("Failed to send UDP message to {}: {}", addr, e);
            }
        }
    }

    /// Receive the next inbound event (TCP or handshake-bound UDP). Returns
    /// `None` if the receiver was taken out via `take_event_receiver` (or the
    /// channel closed).
    pub async fn recv_event(&mut self) -> Option<TransportEvent> {
        match self.event_rx.as_mut() {
            Some(rx) => rx.recv().await,
            None => None,
        }
    }

    /// Move the inbound event receiver out of the transport layer. Called
    /// once at game-loop startup so message draining needs no transport lock.
    pub fn take_event_receiver(&mut self) -> Option<mpsc::Receiver<TransportEvent>> {
        self.event_rx.take()
    }

    pub async fn send_tcp(
        &self,
        connection_id: ConnectionId,
        msg: ServerMessage,
    ) -> Result<(), TransportError> {
        let priority = msg.priority();
        self.send_frame(connection_id, OutboundFrame::Message(msg), priority)
            .await
    }

    /// Send pre-serialized bytes (rmp_serde named encoding, no length prefix —
    /// the writer task adds it). Mirrors `send_tcp` priority semantics.
    pub async fn send_serialized(
        &self,
        connection_id: ConnectionId,
        data: Bytes,
        priority: MessagePriority,
    ) -> Result<(), TransportError> {
        self.send_frame(
            connection_id,
            OutboundFrame::Serialized { data, priority },
            priority,
        )
        .await
    }

    async fn send_frame(
        &self,
        connection_id: ConnectionId,
        frame: OutboundFrame,
        priority: MessagePriority,
    ) -> Result<(), TransportError> {
        // Find the connection and use its dedicated channel
        if let Some(conn_info) = self.registry.connections.read().await.get(&connection_id) {
            match priority {
                MessagePriority::Critical => {
                    // Critical messages must be delivered or client disconnected
                    match conn_info.tcp_tx.send(frame).await {
                        Ok(_) => Ok(()),
                        Err(_) => {
                            // Channel full or closed - this is a slow/dead client
                            warn!("Critical message could not be sent to connection {}, client should be disconnected", connection_id);
                            self.metrics
                                .clients_disconnected_backpressure
                                .fetch_add(1, Ordering::Relaxed);
                            Err(TransportError::QueueFull)
                        }
                    }
                }
                MessagePriority::Droppable => {
                    // Droppable messages can be dropped if queue is full
                    match conn_info.tcp_tx.try_send(frame) {
                        Ok(_) => Ok(()),
                        Err(mpsc::error::TrySendError::Full(_)) => {
                            // Queue full, drop the message and log it
                            self.metrics
                                .tcp_messages_dropped
                                .fetch_add(1, Ordering::Relaxed);
                            let dropped = self.metrics.tcp_dropped();
                            if dropped % 100 == 1 {
                                warn!("TCP queue full for connection {}, dropped droppable message (total dropped: {})", 
                                    connection_id, dropped);
                            }
                            Ok(()) // Not an error - dropping is expected behavior
                        }
                        Err(mpsc::error::TrySendError::Closed(_)) => {
                            Err(TransportError::ConnectionNotFound)
                        }
                    }
                }
            }
        } else {
            Err(TransportError::ConnectionNotFound)
        }
    }

    /// Send a message over UDP (serialized with the named encoding). Prefer
    /// `send_udp_serialized` on hot paths so serialization happens once.
    pub async fn send_udp(
        &self,
        addr: SocketAddr,
        msg: ServerMessage,
    ) -> Result<(), TransportError> {
        let data = Bytes::from(rmp_serde::to_vec_named(&msg)?);
        self.send_udp_serialized(addr, data)
    }

    /// Send pre-serialized bytes over UDP. Always droppable: the queue never
    /// blocks the caller, and a full queue counts as a dropped message.
    pub fn send_udp_serialized(&self, addr: SocketAddr, data: Bytes) -> Result<(), TransportError> {
        match self.udp_out_tx.try_send((addr, data)) {
            Ok(_) => Ok(()),
            Err(mpsc::error::TrySendError::Full(_)) => {
                self.metrics
                    .udp_messages_dropped
                    .fetch_add(1, Ordering::Relaxed);
                let dropped = self.metrics.udp_dropped();
                if dropped % 1000 == 1 {
                    warn!(
                        "UDP queue full for addr {}, dropped droppable message (total dropped: {})",
                        addr, dropped
                    );
                }
                Ok(()) // Not an error - dropping is expected behavior
            }
            Err(mpsc::error::TrySendError::Closed(_)) => Err(TransportError::ConnectionNotFound),
        }
    }

    pub async fn get_connection(&self, connection_id: ConnectionId) -> Option<ConnectionInfo> {
        self.registry
            .connections
            .read()
            .await
            .get(&connection_id)
            .cloned()
    }

    pub async fn get_player_connection(&self, player_id: PlayerId) -> Option<ConnectionId> {
        self.registry
            .player_to_connection
            .read()
            .await
            .get(&player_id)
            .copied()
    }

    /// Resolve a player's connection and (if UDP-handshaken) UDP address in
    /// one pass, without cloning the full `ConnectionInfo`. Used by the
    /// telemetry fan-out to prefer UDP with a TCP fallback.
    pub async fn get_player_route(
        &self,
        player_id: PlayerId,
    ) -> Option<(ConnectionId, Option<SocketAddr>)> {
        let conn_id = self.get_player_connection(player_id).await?;
        let udp_addr = self
            .registry
            .connections
            .read()
            .await
            .get(&conn_id)
            .and_then(|c| c.udp_addr);
        Some((conn_id, udp_addr))
    }

    pub async fn cleanup_stale_connections(&self) -> Vec<(PlayerId, Option<SessionId>)> {
        let now = Instant::now();
        let timeout = self.heartbeat_timeout;
        // Use a much longer timeout for lobby players (30 seconds)
        // Only players in racing sessions need strict heartbeat enforcement
        let lobby_timeout = Duration::from_secs(30);

        let to_remove: Vec<ConnectionId> = {
            let connections = self.registry.connections.read().await;
            connections
                .iter()
                .filter_map(|(conn_id, info)| {
                    let elapsed = now.duration_since(info.last_heartbeat);

                    // Use different timeouts based on session state
                    let timeout_to_use = if info.in_session.is_some() {
                        // Strict timeout for players in racing sessions
                        timeout
                    } else {
                        // Lenient timeout for lobby players
                        lobby_timeout
                    };

                    if elapsed > timeout_to_use {
                        warn!(
                            "Connection {} timed out (player: {}, in_session: {}, elapsed: {:?})",
                            conn_id,
                            info.player_name,
                            info.in_session.is_some(),
                            elapsed
                        );
                        Some(*conn_id)
                    } else {
                        None
                    }
                })
                .collect()
        };

        let mut disconnected_players = Vec::new();
        for conn_id in to_remove {
            if let Some(info) = self.registry.remove_connection(conn_id).await {
                disconnected_players.push((info.player_id, info.in_session));
            }
        }

        disconnected_players
    }

    pub async fn update_heartbeat(&self, connection_id: ConnectionId) {
        if let Some(info) = self
            .registry
            .connections
            .write()
            .await
            .get_mut(&connection_id)
        {
            info.last_heartbeat = Instant::now();
        }
    }

    pub async fn set_player_session(
        &self,
        connection_id: ConnectionId,
        session_id: Option<SessionId>,
    ) {
        if let Some(info) = self
            .registry
            .connections
            .write()
            .await
            .get_mut(&connection_id)
        {
            info.in_session = session_id;
        }
    }

    /// Actual bound TCP address (useful when binding to port 0 in tests).
    pub fn tcp_local_addr(&self) -> SocketAddr {
        self.tcp_local_addr
    }

    /// Actual bound UDP address (useful when binding to port 0 in tests).
    pub fn udp_local_addr(&self) -> SocketAddr {
        self.udp_local_addr
    }

    pub async fn shutdown(&self) {
        info!("Initiating transport layer shutdown");

        // Send shutdown message to all connected clients
        let connections = self.registry.connections.read().await;
        for conn_info in connections.values() {
            debug!(
                "Sending shutdown notification to player: {}",
                conn_info.player_name
            );
            let _ = conn_info
                .tcp_tx
                .try_send(OutboundFrame::Message(ServerMessage::Error {
                    code: 503,
                    message: "Server is shutting down".to_string(),
                }));
        }
        drop(connections);

        // Signal shutdown to all tasks
        let _ = self.shutdown_tx.send(());

        // Give connections time to send shutdown messages. Only `&self` is
        // borrowed here, so callers can hold a read lock (never the write
        // lock) across this grace period.
        tokio::time::sleep(std::time::Duration::from_millis(500)).await;

        debug!("Transport layer shutdown complete");
    }

    pub async fn broadcast_tcp(&self, msg: ServerMessage) {
        let priority = msg.priority();
        self.broadcast_frame(OutboundFrame::Message(msg), priority)
            .await
    }

    /// Broadcast pre-serialized bytes to every connection: serialize once,
    /// fan out cheap `Bytes` clones. Mirrors `broadcast_tcp` semantics.
    pub async fn broadcast_serialized(&self, data: Bytes, priority: MessagePriority) {
        self.broadcast_frame(OutboundFrame::Serialized { data, priority }, priority)
            .await
    }

    async fn broadcast_frame(&self, frame: OutboundFrame, priority: MessagePriority) {
        let connections = self.registry.connections.read().await;
        let mut dropped_count = 0;
        let mut failed_critical = 0;

        for conn_info in connections.values() {
            match priority {
                MessagePriority::Critical => {
                    // Critical messages should be sent
                    if conn_info.tcp_tx.send(frame.clone()).await.is_err() {
                        failed_critical += 1;
                        warn!("Failed to broadcast critical message to connection, client should be disconnected");
                    }
                }
                MessagePriority::Droppable => {
                    // Try to send, but drop if queue full
                    if let Err(mpsc::error::TrySendError::Full(_)) =
                        conn_info.tcp_tx.try_send(frame.clone())
                    {
                        dropped_count += 1;
                    }
                }
            }
        }

        if dropped_count > 0 {
            self.metrics
                .tcp_messages_dropped
                .fetch_add(dropped_count, Ordering::Relaxed);
            debug!(
                "Broadcast dropped {} droppable messages to slow clients",
                dropped_count
            );
        }

        if failed_critical > 0 {
            self.metrics
                .clients_disconnected_backpressure
                .fetch_add(failed_critical, Ordering::Relaxed);
            warn!(
                "Broadcast failed for {} critical messages, clients marked for disconnect",
                failed_critical
            );
        }
    }

    pub fn get_connection_count(&self) -> usize {
        // Use try_read for non-blocking synchronous access
        // Returns 0 if the lock is currently held for writing
        self.registry
            .connections
            .try_read()
            .map(|connections| connections.len())
            .unwrap_or(0)
    }

    pub async fn get_connection_count_async(&self) -> usize {
        self.registry.connections.read().await.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Helper to build a ConnectionInfo for tests
    fn test_connection_info(name: &str, addr: SocketAddr) -> ConnectionInfo {
        let (conn_tx, _) = mpsc::channel(PER_CLIENT_TCP_CHANNEL_SIZE);
        ConnectionInfo {
            player_id: Uuid::new_v4(),
            player_name: name.to_string(),
            connected_at: Instant::now(),
            last_heartbeat: Instant::now(),
            tcp_addr: addr,
            tcp_tx: conn_tx,
            in_session: None,
            udp_token: Uuid::new_v4().to_string(),
            udp_addr: None,
        }
    }

    // Helper function to create a minimal TransportLayer for testing
    async fn create_test_transport_layer() -> TransportLayer {
        let (event_tx, event_rx) = mpsc::channel(TCP_INBOUND_CHANNEL_SIZE);
        let (udp_out_tx, udp_out_rx) = mpsc::channel(UDP_OUTBOUND_CHANNEL_SIZE);
        let (shutdown_tx, shutdown_rx) = mpsc::unbounded_channel();

        TransportLayer {
            registry: ConnRegistry::new(),
            tcp_listener: None,
            udp_socket: Arc::new(UdpSocket::bind("127.0.0.1:0").await.unwrap()),
            tls_acceptor: None,
            tcp_local_addr: "127.0.0.1:0".parse().unwrap(),
            udp_local_addr: "127.0.0.1:0".parse().unwrap(),
            event_rx: Some(event_rx),
            event_tx,
            udp_out_tx,
            udp_out_rx,
            shutdown_tx,
            shutdown_rx: Some(shutdown_rx),
            heartbeat_timeout: Duration::from_secs(30),
            auth: AuthSettings::default(),
            metrics: TransportMetrics::new(),
        }
    }

    #[test]
    fn test_token_bucket_allows_burst_then_limits() {
        let mut bucket = TokenBucket::new(10.0, 5.0);
        // Full burst is available immediately
        for _ in 0..5 {
            assert!(bucket.try_take());
        }
        // Bucket is now empty; an immediate take must fail
        assert!(!bucket.try_take());
    }

    #[test]
    fn test_token_bucket_refills_over_time() {
        let mut bucket = TokenBucket::new(1000.0, 2.0);
        assert!(bucket.try_take());
        assert!(bucket.try_take());
        assert!(!bucket.try_take());
        // At 1000 tokens/sec, 10ms refills well over one token
        std::thread::sleep(Duration::from_millis(10));
        assert!(bucket.try_take());
    }

    #[test]
    fn test_unique_connection_ids() {
        // Test that Uuid::new_v4() generates unique IDs
        let id1 = Uuid::new_v4();
        let id2 = Uuid::new_v4();
        let id3 = Uuid::new_v4();

        // All IDs should be different
        assert_ne!(id1, id2);
        assert_ne!(id2, id3);
        assert_ne!(id1, id3);
    }

    #[tokio::test]
    async fn test_connection_count_empty() {
        let transport = create_test_transport_layer().await;

        // Test with no connections
        let sync_count = transport.get_connection_count();
        let async_count = transport.get_connection_count_async().await;

        assert_eq!(
            sync_count, 0,
            "Sync count should be 0 for empty connections"
        );
        assert_eq!(
            async_count, 0,
            "Async count should be 0 for empty connections"
        );
        assert_eq!(
            sync_count, async_count,
            "Sync and async counts should match"
        );
    }

    #[tokio::test]
    async fn test_connection_count_with_connections() {
        let transport = create_test_transport_layer().await;

        // Add some mock connections
        {
            let mut connections = transport.registry.connections.write().await;

            for i in 0..3 {
                let addr: SocketAddr = format!("127.0.0.1:{}", 8000 + i).parse().unwrap();
                let conn_id = Uuid::new_v4();
                connections.insert(conn_id, test_connection_info(&format!("Player{}", i), addr));
            }
        }

        // Test with 3 connections
        let sync_count = transport.get_connection_count();
        let async_count = transport.get_connection_count_async().await;

        assert_eq!(sync_count, 3, "Sync count should be 3 with 3 connections");
        assert_eq!(async_count, 3, "Async count should be 3 with 3 connections");
        assert_eq!(
            sync_count, async_count,
            "Sync and async counts should match"
        );
    }

    #[tokio::test]
    async fn test_connection_count_consistency() {
        let transport = create_test_transport_layer().await;

        // Test adding connections one by one and verifying counts match
        for expected_count in 0..=5 {
            let sync_count = transport.get_connection_count();
            let async_count = transport.get_connection_count_async().await;

            assert_eq!(
                sync_count, expected_count,
                "Sync count should be {} after adding {} connections",
                expected_count, expected_count
            );
            assert_eq!(
                async_count, expected_count,
                "Async count should be {} after adding {} connections",
                expected_count, expected_count
            );
            assert_eq!(
                sync_count, async_count,
                "Sync and async counts must always match (expected {})",
                expected_count
            );

            // Add one more connection for next iteration
            if expected_count < 5 {
                let mut connections = transport.registry.connections.write().await;
                let addr: SocketAddr = format!("127.0.0.1:{}", 9000 + expected_count)
                    .parse()
                    .unwrap();
                let conn_id = Uuid::new_v4();
                connections.insert(
                    conn_id,
                    test_connection_info(&format!("Player{}", expected_count), addr),
                );
            }
        }
    }

    #[test]
    fn test_connection_id_type_is_uuid() {
        // Verify that ConnectionId is indeed a Uuid type
        let conn_id: ConnectionId = Uuid::new_v4();

        // Should be able to convert to string
        let id_string = conn_id.to_string();
        assert!(!id_string.is_empty());

        // Should have standard UUID format (8-4-4-4-12 hex digits)
        assert_eq!(id_string.len(), 36); // UUID string length with dashes
    }
}

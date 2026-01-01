use crate::data::*;
use crate::network::{ClientMessage, ServerMessage, MessagePriority};
use rustls::pki_types::CertificateDer;
use rustls::ServerConfig as TlsConfig;
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
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
const UDP_INBOUND_CHANNEL_SIZE: usize = 2000;
const UDP_OUTBOUND_CHANNEL_SIZE: usize = 2000;
const PER_CLIENT_TCP_CHANNEL_SIZE: usize = 100;

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
        self.clients_disconnected_backpressure.load(Ordering::Relaxed)
    }
}

#[derive(Debug, Error)]
pub enum TransportError {
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),
    #[error("Serialization error: {0}")]
    Serialization(#[from] bincode::Error),
    #[error("TLS error: {0}")]
    Tls(#[from] rustls::Error),
    #[error("Connection not found")]
    ConnectionNotFound,
    #[error("Invalid message")]
    InvalidMessage,
    #[error("Queue full - client too slow")]
    QueueFull,
}

#[derive(Debug, Clone)]
pub struct ConnectionInfo {
    pub player_id: PlayerId,
    pub player_name: String,
    pub connected_at: Instant,
    pub last_heartbeat: Instant,
    pub tcp_addr: SocketAddr,
    pub tcp_tx: mpsc::Sender<ServerMessage>,
}

pub struct TransportLayer {
    // Connection tracking
    connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
    player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,

    // Network sockets
    tcp_listener: Option<TcpListener>,
    udp_socket: Arc<UdpSocket>,
    tls_acceptor: Option<TlsAcceptor>,

    // Channels for communication (bounded)
    tcp_rx: mpsc::Receiver<(ConnectionId, ClientMessage)>,
    tcp_tx: mpsc::Sender<(ConnectionId, ClientMessage)>,
    udp_rx: mpsc::Receiver<(SocketAddr, ClientMessage)>,
    udp_tx: mpsc::Sender<(SocketAddr, ClientMessage)>,

    // Outbound message queues (UDP only - TCP uses per-connection channels)
    udp_out_tx: mpsc::Sender<(SocketAddr, ServerMessage)>,
    udp_out_rx: mpsc::Receiver<(SocketAddr, ServerMessage)>,

    // Shutdown channel (unbounded - low volume, critical)
    shutdown_tx: mpsc::UnboundedSender<()>,
    #[allow(dead_code)]
    shutdown_rx: Option<mpsc::UnboundedReceiver<()>>,

    heartbeat_timeout: Duration,

    // Metrics
    pub metrics: TransportMetrics,
}

impl TransportLayer {
    pub async fn new(
        tcp_bind: &str,
        udp_bind: &str,
        tls_cert_path: &str,
        tls_key_path: &str,
        heartbeat_timeout_ms: u64,
    ) -> Result<Self, TransportError> {
        // Setup TCP with TLS
        let tcp_listener = TcpListener::bind(tcp_bind).await?;
        info!("TCP listener bound to {}", tcp_bind);

        // Setup UDP
        let udp_socket = Arc::new(UdpSocket::bind(udp_bind).await?);
        info!("UDP socket bound to {}", udp_bind);

        // Load TLS configuration
        let tls_acceptor = match Self::load_tls_config(tls_cert_path, tls_key_path) {
            Ok(config) => {
                info!("TLS configuration loaded successfully");
                Some(TlsAcceptor::from(Arc::new(config)))
            }
            Err(e) => {
                warn!("Failed to load TLS config: {}. Running without TLS encryption.", e);
                None
            }
        };

        // Create bounded channels
        let (tcp_tx, tcp_rx) = mpsc::channel(TCP_INBOUND_CHANNEL_SIZE);
        let (udp_tx, udp_rx) = mpsc::channel(UDP_INBOUND_CHANNEL_SIZE);
        let (udp_out_tx, udp_out_rx) = mpsc::channel(UDP_OUTBOUND_CHANNEL_SIZE);
        let (shutdown_tx, shutdown_rx) = mpsc::unbounded_channel();

        Ok(Self {
            connections: Arc::new(RwLock::new(HashMap::new())),
            player_to_connection: Arc::new(RwLock::new(HashMap::new())),
            addr_to_connection: Arc::new(RwLock::new(HashMap::new())),
            tcp_listener: Some(tcp_listener),
            udp_socket,
            tls_acceptor,
            tcp_rx,
            tcp_tx,
            udp_rx,
            udp_tx,
            udp_out_tx,
            udp_out_rx,
            shutdown_tx,
            shutdown_rx: Some(shutdown_rx),
            heartbeat_timeout: Duration::from_millis(heartbeat_timeout_ms),
            metrics: TransportMetrics::new(),
        })
    }

    fn load_tls_config(cert_path: &str, key_path: &str) -> Result<TlsConfig, TransportError> {
        // Load certificates
        let cert_file = File::open(cert_path)?;
        let mut cert_reader = BufReader::new(cert_file);
        let certs: Vec<CertificateDer> = rustls_pemfile::certs(&mut cert_reader)
            .collect::<Result<Vec<_>, _>>()?;

        if certs.is_empty() {
            return Err(TransportError::Io(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "No certificates found in cert file",
            )));
        }

        // Load private key
        let key_file = File::open(key_path)?;
        let mut key_reader = BufReader::new(key_file);
        let key = rustls_pemfile::private_key(&mut key_reader)?
            .ok_or_else(|| {
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
            let tcp_tx = self.tcp_tx.clone();
            let tls_acceptor = self.tls_acceptor.clone();
            let connections = Arc::clone(&self.connections);
            let addr_to_connection = Arc::clone(&self.addr_to_connection);
            let player_to_connection = Arc::clone(&self.player_to_connection);

            tokio::spawn(async move {
                Self::tcp_acceptor(listener, tcp_tx, tls_acceptor, connections, addr_to_connection, player_to_connection).await;
            });
        }

        // Spawn UDP receiver
        let udp_socket = Arc::clone(&self.udp_socket);
        let udp_tx = self.udp_tx.clone();
        tokio::spawn(async move {
            Self::udp_receiver(udp_socket, udp_tx).await;
        });

        // Spawn UDP sender
        let udp_socket = Arc::clone(&self.udp_socket);
        let mut udp_out_rx = std::mem::replace(
            &mut self.udp_out_rx,
            mpsc::channel(1).1,
        );
        let metrics = self.metrics.clone();
        tokio::spawn(async move {
            Self::udp_sender(udp_socket, &mut udp_out_rx, metrics).await;
        });
    }

    async fn tcp_acceptor(
        listener: TcpListener,
        tcp_tx: mpsc::Sender<(ConnectionId, ClientMessage)>,
        tls_acceptor: Option<TlsAcceptor>,
        connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
        addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
        player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    ) {
        loop {
            match listener.accept().await {
                Ok((stream, addr)) => {
                    info!("New TCP connection from {}", addr);
                    let tcp_tx = tcp_tx.clone();
                    let tls_acceptor = tls_acceptor.clone();
                    let connections = Arc::clone(&connections);
                    let addr_to_connection = Arc::clone(&addr_to_connection);
                    let player_to_connection = Arc::clone(&player_to_connection);

                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_tcp_connection(
                            stream,
                            addr,
                            tcp_tx,
                            tls_acceptor,
                            connections,
                            addr_to_connection,
                            player_to_connection,
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
        tcp_tx: mpsc::Sender<(ConnectionId, ClientMessage)>,
        tls_acceptor: Option<TlsAcceptor>,
        connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
        addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
        player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    ) -> Result<(), TransportError> {
        // Generate connection ID
        let connection_id = Self::addr_to_connection_id(&addr);

        // Create per-connection send channel (BOUNDED)
        let (conn_tx, conn_rx) = mpsc::channel::<ServerMessage>(PER_CLIENT_TCP_CHANNEL_SIZE);

        // Handle TLS if available
        if let Some(acceptor) = tls_acceptor {
            match acceptor.accept(stream).await {
                Ok(tls_stream) => {
                    info!("TLS connection established for {}", addr);
                    Self::handle_stream(
                        tls_stream,
                        addr,
                        connection_id,
                        conn_tx,
                        conn_rx,
                        tcp_tx,
                        connections,
                        addr_to_connection,
                        player_to_connection,
                    )
                    .await
                }
                Err(e) => {
                    error!("TLS handshake failed for {}: {}", addr, e);
                    Err(TransportError::Io(std::io::Error::new(
                        std::io::ErrorKind::Other,
                        format!("TLS handshake failed: {}", e)
                    )))
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
                tcp_tx,
                connections,
                addr_to_connection,
                player_to_connection,
            )
            .await
        }
    }

    async fn handle_stream<S>(
        stream: S,
        addr: SocketAddr,
        connection_id: ConnectionId,
        conn_tx: mpsc::Sender<ServerMessage>,
        mut conn_rx: mpsc::Receiver<ServerMessage>,
        tcp_tx: mpsc::Sender<(ConnectionId, ClientMessage)>,
        connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
        addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
        player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    ) -> Result<(), TransportError>
    where
        S: AsyncReadExt + AsyncWriteExt + Unpin + Send + 'static,
    {
        // Split into reader and writer
        let (mut reader, mut writer) = tokio::io::split(stream);

        // Spawn writer task
        let writer_addr = addr;
        tokio::spawn(async move {
            while let Some(msg) = conn_rx.recv().await {
                match bincode::serialize(&msg) {
                    Ok(data) => {
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
                    Err(e) => {
                        error!("Failed to serialize message: {}", e);
                        break;
                    }
                }
            }
            debug!("Writer task closed for {}", writer_addr);
        });

        // Reader task (runs in this function)
        // Read with length-prefix framing
        let mut len_buf = [0u8; 4];

        loop {
            // Read length prefix
            match reader.read_exact(&mut len_buf).await {
                Ok(_) => {
                    let len = u32::from_be_bytes(len_buf) as usize;

                    // Sanity check to prevent memory exhaustion
                    if len > 1_000_000 {  // 1MB max message size
                        warn!("Message too large from {}: {} bytes", addr, len);
                        break;
                    }

                    // Read message data
                    let mut msg_buf = vec![0u8; len];
                    match reader.read_exact(&mut msg_buf).await {
                        Ok(_) => {
                            match bincode::deserialize::<ClientMessage>(&msg_buf) {
                                Ok(msg) => {
                                    // Handle authentication - register connection
                                    if let ClientMessage::Authenticate { player_name, .. } = &msg {
                                        let player_id = Uuid::new_v4();
                                        let conn_info = ConnectionInfo {
                                            player_id,
                                            player_name: player_name.clone(),
                                            connected_at: Instant::now(),
                                            last_heartbeat: Instant::now(),
                                            tcp_addr: addr,
                                            tcp_tx: conn_tx.clone(),
                                        };

                                        connections.write().await.insert(connection_id, conn_info.clone());
                                        addr_to_connection.write().await.insert(addr, connection_id);
                                        // Also track player_id -> connection_id mapping for broadcast lookups
                                        player_to_connection.write().await.insert(player_id, connection_id);
                                        info!("Player {} authenticated as {} (connection: {})", player_name, player_id, connection_id);

                                        // Send auth success response
                                        let response = ServerMessage::AuthSuccess {
                                            player_id,
                                            server_version: 1,
                                        };
                                        let _ = conn_tx.send(response).await;
                                    } else if let ClientMessage::Heartbeat { .. } = &msg {
                                        // Update last heartbeat time
                                        if let Some(conn) = connections.write().await.get_mut(&connection_id) {
                                            conn.last_heartbeat = Instant::now();
                                        }

                                        // Send heartbeat ack
                                        let response = ServerMessage::HeartbeatAck {
                                            server_tick: 0, // Will be updated later with actual tick
                                        };
                                        let _ = conn_tx.send(response).await;
                                    }

                                    if tcp_tx.send((connection_id, msg)).await.is_err() {
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
                        info!("Connection closed by client: {}", addr);
                    }
                    break;
                }
            }
        }

        // Cleanup connection
        if let Some(conn) = connections.write().await.remove(&connection_id) {
            addr_to_connection.write().await.remove(&addr);
            player_to_connection.write().await.remove(&conn.player_id);
            info!("Connection cleaned up: {} (player: {})", addr, conn.player_name);
        }

        Ok(())
    }

    async fn udp_receiver(
        socket: Arc<UdpSocket>,
        tx: mpsc::Sender<(SocketAddr, ClientMessage)>,
    ) {
        let mut buf = vec![0u8; 2048];
        loop {
            match socket.recv_from(&mut buf).await {
                Ok((n, addr)) => {
                    match bincode::deserialize::<ClientMessage>(&buf[..n]) {
                        Ok(msg) => {
                            // Try to send, but don't block if queue is full
                            if tx.send((addr, msg)).await.is_err() {
                                // Channel closed, exit
                                error!("UDP receiver channel closed");
                                break;
                            }
                        }
                        Err(e) => {
                            debug!("Failed to deserialize UDP message from {}: {}", addr, e);
                        }
                    }
                }
                Err(e) => {
                    error!("UDP receive error: {}", e);
                }
            }
        }
    }

    async fn udp_sender(
        socket: Arc<UdpSocket>,
        rx: &mut mpsc::Receiver<(SocketAddr, ServerMessage)>,
        _metrics: TransportMetrics,
    ) {
        while let Some((addr, msg)) = rx.recv().await {
            match bincode::serialize(&msg) {
                Ok(data) => {
                    if let Err(e) = socket.send_to(&data, addr).await {
                        debug!("Failed to send UDP message to {}: {}", addr, e);
                    }
                }
                Err(e) => {
                    error!("Failed to serialize UDP message: {}", e);
                }
            }
        }
    }

    pub async fn recv_tcp(&mut self) -> Option<(ConnectionId, ClientMessage)> {
        self.tcp_rx.recv().await
    }

    pub async fn recv_udp(&mut self) -> Option<(SocketAddr, ClientMessage)> {
        self.udp_rx.recv().await
    }

    pub async fn send_tcp(&self, connection_id: ConnectionId, msg: ServerMessage) -> Result<(), TransportError> {
        // Find the connection and use its dedicated channel
        if let Some(conn_info) = self.connections.read().await.get(&connection_id) {
            let priority = msg.priority();
            
            match priority {
                MessagePriority::Critical => {
                    // Critical messages must be delivered or client disconnected
                    match conn_info.tcp_tx.send(msg).await {
                        Ok(_) => Ok(()),
                        Err(_) => {
                            // Channel full or closed - this is a slow/dead client
                            warn!("Critical message could not be sent to connection {}, client should be disconnected", connection_id);
                            self.metrics.clients_disconnected_backpressure.fetch_add(1, Ordering::Relaxed);
                            Err(TransportError::QueueFull)
                        }
                    }
                }
                MessagePriority::Droppable => {
                    // Droppable messages can be dropped if queue is full
                    match conn_info.tcp_tx.try_send(msg) {
                        Ok(_) => Ok(()),
                        Err(mpsc::error::TrySendError::Full(_)) => {
                            // Queue full, drop the message and log it
                            self.metrics.tcp_messages_dropped.fetch_add(1, Ordering::Relaxed);
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

    pub async fn send_udp(&self, addr: SocketAddr, msg: ServerMessage) -> Result<(), TransportError> {
        let priority = msg.priority();
        
        match priority {
            MessagePriority::Critical => {
                // Critical messages should be sent via TCP, not UDP
                // But if we have to send via UDP, try our best
                self.udp_out_tx
                    .send((addr, msg))
                    .await
                    .map_err(|_| TransportError::ConnectionNotFound)
            }
            MessagePriority::Droppable => {
                // Droppable messages can be dropped if queue is full
                match self.udp_out_tx.try_send((addr, msg)) {
                    Ok(_) => Ok(()),
                    Err(mpsc::error::TrySendError::Full(_)) => {
                        // Queue full, drop the message and log it
                        self.metrics.udp_messages_dropped.fetch_add(1, Ordering::Relaxed);
                        let dropped = self.metrics.udp_dropped();
                        if dropped % 1000 == 1 {
                            warn!("UDP queue full for addr {}, dropped droppable message (total dropped: {})", 
                                addr, dropped);
                        }
                        Ok(()) // Not an error - dropping is expected behavior
                    }
                    Err(mpsc::error::TrySendError::Closed(_)) => {
                        Err(TransportError::ConnectionNotFound)
                    }
                }
            }
        }
    }

    pub async fn get_connection(&self, connection_id: ConnectionId) -> Option<ConnectionInfo> {
        self.connections.read().await.get(&connection_id).cloned()
    }

    pub async fn get_player_connection(&self, player_id: PlayerId) -> Option<ConnectionId> {
        self.player_to_connection.read().await.get(&player_id).copied()
    }

    pub async fn cleanup_stale_connections(&self) {
        let now = Instant::now();
        let timeout = self.heartbeat_timeout;

        let mut connections = self.connections.write().await;
        let mut to_remove = Vec::new();

        for (conn_id, info) in connections.iter() {
            if now.duration_since(info.last_heartbeat) > timeout {
                warn!("Connection {} timed out (player: {})", conn_id, info.player_name);
                to_remove.push(*conn_id);
            }
        }

        for conn_id in to_remove {
            if let Some(info) = connections.remove(&conn_id) {
                self.addr_to_connection.write().await.remove(&info.tcp_addr);
                self.player_to_connection.write().await.remove(&info.player_id);
            }
        }
    }

    pub async fn update_heartbeat(&self, connection_id: ConnectionId) {
        if let Some(info) = self.connections.write().await.get_mut(&connection_id) {
            info.last_heartbeat = Instant::now();
        }
    }

    pub async fn shutdown(&mut self) {
        info!("Initiating transport layer shutdown");

        // Send shutdown message to all connected clients
        let connections = self.connections.read().await;
        for conn_info in connections.values() {
            info!("Sending shutdown notification to player: {}", conn_info.player_name);
            let _ = conn_info.tcp_tx.send(ServerMessage::Error {
                code: 503,
                message: "Server is shutting down".to_string(),
            });
        }
        drop(connections);

        // Signal shutdown to all tasks
        let _ = self.shutdown_tx.send(());

        // Give connections time to send shutdown messages
        tokio::time::sleep(std::time::Duration::from_millis(500)).await;

        info!("Transport layer shutdown complete");
    }

    pub async fn broadcast_tcp(&self, msg: ServerMessage) {
        let connections = self.connections.read().await;
        let priority = msg.priority();
        let mut dropped_count = 0;
        
        for conn_info in connections.values() {
            match priority {
                MessagePriority::Critical => {
                    // Critical messages should be sent
                    if conn_info.tcp_tx.send(msg.clone()).await.is_err() {
                        warn!("Failed to broadcast critical message to connection");
                    }
                }
                MessagePriority::Droppable => {
                    // Try to send, but drop if queue full
                    if let Err(mpsc::error::TrySendError::Full(_)) = conn_info.tcp_tx.try_send(msg.clone()) {
                        dropped_count += 1;
                    }
                }
            }
        }
        
        if dropped_count > 0 {
            self.metrics.tcp_messages_dropped.fetch_add(dropped_count, Ordering::Relaxed);
            debug!("Broadcast dropped {} droppable messages to slow clients", dropped_count);
        }
    }

    pub fn get_connection_count(&self) -> usize {
        // This is async but we need a sync version for quick checks
        // We'll add an async version too
        0 // Placeholder, use get_connection_count_async instead
    }

    pub async fn get_connection_count_async(&self) -> usize {
        self.connections.read().await.len()
    }

    fn addr_to_connection_id(addr: &SocketAddr) -> ConnectionId {
        use std::collections::hash_map::DefaultHasher;
        use std::hash::{Hash, Hasher};

        let mut hasher = DefaultHasher::new();
        addr.hash(&mut hasher);
        hasher.finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_addr_to_connection_id() {
        let addr1: SocketAddr = "127.0.0.1:1234".parse().unwrap();
        let addr2: SocketAddr = "127.0.0.1:1234".parse().unwrap();
        let addr3: SocketAddr = "127.0.0.1:5678".parse().unwrap();

        let id1 = TransportLayer::addr_to_connection_id(&addr1);
        let id2 = TransportLayer::addr_to_connection_id(&addr2);
        let id3 = TransportLayer::addr_to_connection_id(&addr3);

        assert_eq!(id1, id2);
        assert_ne!(id1, id3);
    }
}

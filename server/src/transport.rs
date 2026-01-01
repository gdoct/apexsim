use crate::data::*;
use crate::network::{ClientMessage, ServerMessage};
use rand::RngCore;
use rustls::pki_types::CertificateDer;
use rustls::ServerConfig as TlsConfig;
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::net::SocketAddr;
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
}

#[derive(Debug, Clone)]
pub struct ConnectionInfo {
    pub player_id: PlayerId,
    pub player_name: String,
    pub connected_at: Instant,
    pub last_heartbeat: Instant,
    pub tcp_addr: SocketAddr,
    pub tcp_tx: mpsc::UnboundedSender<ServerMessage>,
    pub udp_secret: [u8; 32],
    pub current_session_id: Option<SessionId>,
    pub udp_addr: Option<SocketAddr>,
    pub last_udp_addr_change: Instant,
}

/// UDP address rebind cooldown period (30 seconds)
const UDP_REBIND_COOLDOWN_SECS: u64 = 30;

/// Mapping of session_id + udp_secret to player for UDP packet validation
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct UdpAuthKey {
    session_id: SessionId,
    udp_secret: [u8; 32],
}

pub struct TransportLayer {
    // Connection tracking
    connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
    player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
    
    // UDP authentication - maps (session_id, udp_secret) to player_id
    udp_auth_map: Arc<RwLock<HashMap<UdpAuthKey, PlayerId>>>,

    // Network sockets
    tcp_listener: Option<TcpListener>,
    udp_socket: Arc<UdpSocket>,
    tls_acceptor: Option<TlsAcceptor>,

    // Channels for communication
    tcp_rx: mpsc::UnboundedReceiver<(ConnectionId, ClientMessage)>,
    tcp_tx: mpsc::UnboundedSender<(ConnectionId, ClientMessage)>,
    udp_rx: mpsc::UnboundedReceiver<(SocketAddr, ClientMessage)>,
    udp_tx: mpsc::UnboundedSender<(SocketAddr, ClientMessage)>,

    // Outbound message queues (UDP only - TCP uses per-connection channels)
    udp_out_tx: mpsc::UnboundedSender<(SocketAddr, ServerMessage)>,
    udp_out_rx: mpsc::UnboundedReceiver<(SocketAddr, ServerMessage)>,

    // Shutdown channel
    shutdown_tx: mpsc::UnboundedSender<()>,
    #[allow(dead_code)]
    shutdown_rx: Option<mpsc::UnboundedReceiver<()>>,

    heartbeat_timeout: Duration,
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

        // Create channels
        let (tcp_tx, tcp_rx) = mpsc::unbounded_channel();
        let (udp_tx, udp_rx) = mpsc::unbounded_channel();
        let (udp_out_tx, udp_out_rx) = mpsc::unbounded_channel();
        let (shutdown_tx, shutdown_rx) = mpsc::unbounded_channel();

        Ok(Self {
            connections: Arc::new(RwLock::new(HashMap::new())),
            player_to_connection: Arc::new(RwLock::new(HashMap::new())),
            addr_to_connection: Arc::new(RwLock::new(HashMap::new())),
            udp_auth_map: Arc::new(RwLock::new(HashMap::new())),
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
        let connections = Arc::clone(&self.connections);
        let udp_auth_map = Arc::clone(&self.udp_auth_map);
        tokio::spawn(async move {
            Self::udp_receiver(udp_socket, udp_tx, connections, udp_auth_map).await;
        });

        // Spawn UDP sender
        let udp_socket = Arc::clone(&self.udp_socket);
        let mut udp_out_rx = std::mem::replace(
            &mut self.udp_out_rx,
            mpsc::unbounded_channel().1,
        );
        tokio::spawn(async move {
            Self::udp_sender(udp_socket, &mut udp_out_rx).await;
        });
    }

    async fn tcp_acceptor(
        listener: TcpListener,
        tcp_tx: mpsc::UnboundedSender<(ConnectionId, ClientMessage)>,
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
        tcp_tx: mpsc::UnboundedSender<(ConnectionId, ClientMessage)>,
        tls_acceptor: Option<TlsAcceptor>,
        connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
        addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
        player_to_connection: Arc<RwLock<HashMap<PlayerId, ConnectionId>>>,
    ) -> Result<(), TransportError> {
        // Generate connection ID
        let connection_id = Self::addr_to_connection_id(&addr);

        // Create per-connection send channel
        let (conn_tx, conn_rx) = mpsc::unbounded_channel::<ServerMessage>();

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
        conn_tx: mpsc::UnboundedSender<ServerMessage>,
        mut conn_rx: mpsc::UnboundedReceiver<ServerMessage>,
        tcp_tx: mpsc::UnboundedSender<(ConnectionId, ClientMessage)>,
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
                                        
                                        // Generate cryptographically secure random UDP secret
                                        let mut udp_secret = [0u8; 32];
                                        rand::rngs::OsRng.fill_bytes(&mut udp_secret);
                                        
                                        let conn_info = ConnectionInfo {
                                            player_id,
                                            player_name: player_name.clone(),
                                            connected_at: Instant::now(),
                                            last_heartbeat: Instant::now(),
                                            tcp_addr: addr,
                                            tcp_tx: conn_tx.clone(),
                                            udp_secret,
                                            current_session_id: None,
                                            udp_addr: None,
                                            last_udp_addr_change: Instant::now(),
                                        };

                                        connections.write().await.insert(connection_id, conn_info.clone());
                                        addr_to_connection.write().await.insert(addr, connection_id);
                                        // Also track player_id -> connection_id mapping for broadcast lookups
                                        player_to_connection.write().await.insert(player_id, connection_id);
                                        info!("Player {} authenticated as {} (connection: {})", player_name, player_id, connection_id);

                                        // Send auth success response with UDP secret
                                        let response = ServerMessage::AuthSuccess {
                                            player_id,
                                            server_version: 1,
                                            udp_secret,
                                        };
                                        let _ = conn_tx.send(response);
                                    } else if let ClientMessage::Heartbeat { .. } = &msg {
                                        // Update last heartbeat time
                                        if let Some(conn) = connections.write().await.get_mut(&connection_id) {
                                            conn.last_heartbeat = Instant::now();
                                        }

                                        // Send heartbeat ack
                                        let response = ServerMessage::HeartbeatAck {
                                            server_tick: 0, // Will be updated later with actual tick
                                        };
                                        let _ = conn_tx.send(response);
                                    }

                                    if tcp_tx.send((connection_id, msg)).is_err() {
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
        tx: mpsc::UnboundedSender<(SocketAddr, ClientMessage)>,
        connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
        udp_auth_map: Arc<RwLock<HashMap<UdpAuthKey, PlayerId>>>,
    ) {
        let mut buf = vec![0u8; 2048];
        loop {
            match socket.recv_from(&mut buf).await {
                Ok((n, addr)) => {
                    match bincode::deserialize::<ClientMessage>(&buf[..n]) {
                        Ok(msg) => {
                            // Validate UDP packets - only accept PlayerInput with valid session_id and secret
                            let is_valid = match &msg {
                                ClientMessage::PlayerInput { session_id, udp_secret, .. } => {
                                    let auth_key = UdpAuthKey {
                                        session_id: *session_id,
                                        udp_secret: *udp_secret,
                                    };
                                    
                                    // Check if this session_id + secret combination is valid
                                    let auth_map = udp_auth_map.read().await;
                                    if let Some(&player_id) = auth_map.get(&auth_key) {
                                        // Valid credentials, now check/update UDP address binding
                                        drop(auth_map);
                                        let mut connections_guard = connections.write().await;
                                        
                                        // Find the connection for this player
                                        if let Some(conn) = connections_guard.values_mut().find(|c| c.player_id == player_id) {
                                            // Check if address change
                                            if let Some(existing_addr) = conn.udp_addr {
                                                if existing_addr != addr {
                                                    // Address change detected
                                                    let elapsed = conn.last_udp_addr_change.elapsed();
                                                    if elapsed.as_secs() < UDP_REBIND_COOLDOWN_SECS {
                                                        warn!(
                                                            "UDP address rebind attempt blocked for player {} (session {}): {} -> {} (cooldown: {}s remaining)",
                                                            player_id, session_id, existing_addr, addr,
                                                            UDP_REBIND_COOLDOWN_SECS - elapsed.as_secs()
                                                        );
                                                        false
                                                    } else {
                                                        // Cooldown expired, allow rebind
                                                        info!(
                                                            "UDP address rebind allowed for player {} (session {}): {} -> {}",
                                                            player_id, session_id, existing_addr, addr
                                                        );
                                                        conn.udp_addr = Some(addr);
                                                        conn.last_udp_addr_change = Instant::now();
                                                        true
                                                    }
                                                } else {
                                                    // Same address, all good
                                                    true
                                                }
                                            } else {
                                                // First UDP packet from this client, bind address
                                                info!(
                                                    "UDP address bound for player {} (session {}): {}",
                                                    player_id, session_id, addr
                                                );
                                                conn.udp_addr = Some(addr);
                                                conn.last_udp_addr_change = Instant::now();
                                                true
                                            }
                                        } else {
                                            warn!("Valid UDP credentials but player {} not found in connections", player_id);
                                            false
                                        }
                                    } else {
                                        warn!("UDP packet from {} with invalid session_id/secret dropped", addr);
                                        false
                                    }
                                }
                                _ => {
                                    // Non-PlayerInput UDP messages are not allowed
                                    warn!("UDP packet from {} is not PlayerInput, dropped", addr);
                                    false
                                }
                            };
                            
                            if is_valid {
                                if tx.send((addr, msg)).is_err() {
                                    error!("Failed to send UDP message to handler");
                                    break;
                                }
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
        rx: &mut mpsc::UnboundedReceiver<(SocketAddr, ServerMessage)>,
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
            conn_info.tcp_tx
                .send(msg)
                .map_err(|_| TransportError::ConnectionNotFound)
        } else {
            Err(TransportError::ConnectionNotFound)
        }
    }

    pub async fn send_udp(&self, addr: SocketAddr, msg: ServerMessage) -> Result<(), TransportError> {
        self.udp_out_tx
            .send((addr, msg))
            .map_err(|_| TransportError::ConnectionNotFound)
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
        for conn_info in connections.values() {
            let _ = conn_info.tcp_tx.send(msg.clone());
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
    
    /// Register a player's UDP credentials for a session
    /// This should be called when a player joins a session
    pub async fn register_udp_session(&self, player_id: PlayerId, session_id: SessionId) -> Result<(), TransportError> {
        let mut connections = self.connections.write().await;
        
        // Find the connection for this player
        if let Some(conn) = connections.values_mut().find(|c| c.player_id == player_id) {
            // Update the session ID
            conn.current_session_id = Some(session_id);
            
            // Register the auth key
            let auth_key = UdpAuthKey {
                session_id,
                udp_secret: conn.udp_secret,
            };
            
            self.udp_auth_map.write().await.insert(auth_key, player_id);
            info!("Registered UDP auth for player {} in session {}", player_id, session_id);
            Ok(())
        } else {
            Err(TransportError::ConnectionNotFound)
        }
    }
    
    /// Unregister a player's UDP credentials for a session
    /// This should be called when a player leaves a session
    pub async fn unregister_udp_session(&self, player_id: PlayerId) -> Result<(), TransportError> {
        let mut connections = self.connections.write().await;
        
        // Find the connection for this player
        if let Some(conn) = connections.values_mut().find(|c| c.player_id == player_id) {
            if let Some(session_id) = conn.current_session_id.take() {
                // Remove the auth key
                let auth_key = UdpAuthKey {
                    session_id,
                    udp_secret: conn.udp_secret,
                };
                
                self.udp_auth_map.write().await.remove(&auth_key);
                // Clear UDP address binding
                conn.udp_addr = None;
                info!("Unregistered UDP auth for player {} from session {}", player_id, session_id);
            }
            Ok(())
        } else {
            Err(TransportError::ConnectionNotFound)
        }
    }
    
    /// Allow immediate UDP address rebind (called after re-authentication)
    pub async fn allow_udp_rebind(&self, player_id: PlayerId) {
        let mut connections = self.connections.write().await;
        if let Some(conn) = connections.values_mut().find(|c| c.player_id == player_id) {
            // Reset the cooldown timer to allow immediate rebind
            conn.last_udp_addr_change = Instant::now() - Duration::from_secs(UDP_REBIND_COOLDOWN_SECS + 1);
            info!("UDP rebind cooldown reset for player {}", player_id);
        }
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

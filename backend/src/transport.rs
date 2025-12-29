use crate::data::*;
use crate::network::{ClientMessage, ServerMessage};
use rustls::pki_types::CertificateDer;
use rustls::ServerConfig as TlsConfig;
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::{Duration, Instant};
use thiserror::Error;
use tokio::io::AsyncReadExt;
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

    // Channels for communication
    tcp_rx: mpsc::UnboundedReceiver<(ConnectionId, ClientMessage)>,
    tcp_tx: mpsc::UnboundedSender<(ConnectionId, ClientMessage)>,
    udp_rx: mpsc::UnboundedReceiver<(SocketAddr, ClientMessage)>,
    udp_tx: mpsc::UnboundedSender<(SocketAddr, ClientMessage)>,

    // Outbound message queues (UDP only - TCP uses per-connection channels)
    udp_out_tx: mpsc::UnboundedSender<(SocketAddr, ServerMessage)>,
    udp_out_rx: mpsc::UnboundedReceiver<(SocketAddr, ServerMessage)>,

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

            tokio::spawn(async move {
                Self::tcp_acceptor(listener, tcp_tx, tls_acceptor, connections, addr_to_connection).await;
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
    ) {
        loop {
            match listener.accept().await {
                Ok((stream, addr)) => {
                    info!("New TCP connection from {}", addr);
                    let tcp_tx = tcp_tx.clone();
                    let tls_acceptor = tls_acceptor.clone();
                    let connections = Arc::clone(&connections);
                    let addr_to_connection = Arc::clone(&addr_to_connection);

                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_tcp_connection(
                            stream,
                            addr,
                            tcp_tx,
                            tls_acceptor,
                            connections,
                            addr_to_connection,
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
        _tls_acceptor: Option<TlsAcceptor>,
        connections: Arc<RwLock<HashMap<ConnectionId, ConnectionInfo>>>,
        addr_to_connection: Arc<RwLock<HashMap<SocketAddr, ConnectionId>>>,
    ) -> Result<(), TransportError> {
        // Generate connection ID
        let connection_id = Self::addr_to_connection_id(&addr);

        // Create per-connection send channel
        let (conn_tx, mut conn_rx) = mpsc::unbounded_channel::<ServerMessage>();

        // Create a simple unencrypted connection for now (TLS will be added properly later)
        // Split into reader and writer
        let (reader, writer) = tokio::io::split(stream);

        // Spawn writer task
        tokio::spawn(async move {
            use tokio::io::AsyncWriteExt;
            let mut writer = writer;
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
                    }
                }
            }
            debug!("Writer task closed for {}", addr);
        });

        // Reader task (runs in this function)
        let mut reader = reader;
        let mut buf = vec![0u8; 8192];

        loop {
            match reader.read(&mut buf).await {
                Ok(0) => {
                    info!("Connection closed by client: {}", addr);
                    break;
                }
                Ok(n) => {
                    match bincode::deserialize::<ClientMessage>(&buf[..n]) {
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

                                connections.write().await.insert(connection_id, conn_info);
                                addr_to_connection.write().await.insert(addr, connection_id);
                                info!("Player {} authenticated as {}", player_name, player_id);

                                // Send auth success response
                                let response = ServerMessage::AuthSuccess {
                                    player_id,
                                    server_version: 1,
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
                    error!("Read error from {}: {}", addr, e);
                    break;
                }
            }
        }

        // Cleanup connection
        connections.write().await.remove(&connection_id);
        addr_to_connection.write().await.remove(&addr);
        info!("Connection cleaned up: {}", addr);

        Ok(())
    }

    async fn udp_receiver(
        socket: Arc<UdpSocket>,
        tx: mpsc::UnboundedSender<(SocketAddr, ClientMessage)>,
    ) {
        let mut buf = vec![0u8; 2048];
        loop {
            match socket.recv_from(&mut buf).await {
                Ok((n, addr)) => {
                    match bincode::deserialize::<ClientMessage>(&buf[..n]) {
                        Ok(msg) => {
                            if tx.send((addr, msg)).is_err() {
                                error!("Failed to send UDP message to handler");
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

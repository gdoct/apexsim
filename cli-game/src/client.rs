use crate::protocol::{ClientMessage, ServerMessage};
use anyhow::{Context, Result};
use std::net::SocketAddr;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tracing::{debug, error, info, warn};

/// Network client for connecting to ApexSim server
pub struct NetworkClient {
    server_addr: SocketAddr,
    tcp_stream: Option<TcpStream>,
    message_rx: mpsc::UnboundedReceiver<ServerMessage>,
    message_tx: mpsc::UnboundedSender<ServerMessage>,
}

impl NetworkClient {
    pub fn new(server_addr: SocketAddr) -> Self {
        let (message_tx, message_rx) = mpsc::unbounded_channel();
        Self {
            server_addr,
            tcp_stream: None,
            message_rx,
            message_tx,
        }
    }

    /// Connect to the server
    pub async fn connect(&mut self) -> Result<()> {
        info!("Connecting to server at {}...", self.server_addr);

        let stream = TcpStream::connect(self.server_addr)
            .await
            .context("Failed to connect to server")?;

        stream
            .set_nodelay(true)
            .context("Failed to set TCP_NODELAY")?;

        info!("Connected to server at {}", self.server_addr);
        self.tcp_stream = Some(stream);

        Ok(())
    }

    /// Send a message to the server
    pub async fn send(&mut self, msg: ClientMessage) -> Result<()> {
        let stream = self
            .tcp_stream
            .as_mut()
            .context("Not connected to server")?;

        // Serialize the message using bincode
        let data = bincode::serialize(&msg).context("Failed to serialize message")?;

        // Send length-prefixed message (big-endian to match server)
        let len = data.len() as u32;
        stream
            .write_all(&len.to_be_bytes())
            .await
            .context("Failed to write message length")?;
        stream
            .write_all(&data)
            .await
            .context("Failed to write message data")?;
        stream.flush().await.context("Failed to flush stream")?;

        debug!("Sent message: {:?}", msg);
        Ok(())
    }

    /// Receive a message from the server (blocking with timeout)
    pub async fn receive(&mut self) -> Result<ServerMessage> {
        let stream = self
            .tcp_stream
            .as_mut()
            .context("Not connected to server")?;

        // Read length prefix (big-endian to match server)
        let mut len_buf = [0u8; 4];
        tokio::time::timeout(Duration::from_secs(10), stream.read_exact(&mut len_buf))
            .await
            .context("Timeout reading message length")?
            .context("Failed to read message length")?;

        let len = u32::from_be_bytes(len_buf) as usize;

        if len > 1024 * 1024 {
            // 1MB limit
            anyhow::bail!("Message too large: {} bytes", len);
        }

        // Read message data
        let mut data = vec![0u8; len];
        tokio::time::timeout(Duration::from_secs(10), stream.read_exact(&mut data))
            .await
            .context("Timeout reading message data")?
            .context("Failed to read message data")?;

        // Deserialize
        let msg: ServerMessage =
            bincode::deserialize(&data).context("Failed to deserialize message")?;

        debug!("Received message: {:?}", msg);
        Ok(msg)
    }

    /// Try to receive a message without blocking
    pub async fn try_receive(&mut self, timeout: Duration) -> Result<Option<ServerMessage>> {
        let stream = self
            .tcp_stream
            .as_mut()
            .context("Not connected to server")?;

        // Read length prefix with timeout (big-endian to match server)
        let mut len_buf = [0u8; 4];
        match tokio::time::timeout(timeout, stream.read_exact(&mut len_buf)).await {
            Ok(Ok(_)) => {}
            Ok(Err(e)) => return Err(e.into()),
            Err(_) => return Ok(None), // Timeout, no message available
        }

        let len = u32::from_be_bytes(len_buf) as usize;

        if len > 1024 * 1024 {
            anyhow::bail!("Message too large: {} bytes", len);
        }

        // Read message data
        let mut data = vec![0u8; len];
        stream
            .read_exact(&mut data)
            .await
            .context("Failed to read message data")?;

        // Deserialize
        let msg: ServerMessage =
            bincode::deserialize(&data).context("Failed to deserialize message")?;

        Ok(Some(msg))
    }

    /// Disconnect from the server
    pub async fn disconnect(&mut self) -> Result<()> {
        if self.tcp_stream.is_some() {
            // Send disconnect message first
            let _ = self.send(ClientMessage::Disconnect).await;

            // Then close the stream
            if let Some(stream) = &mut self.tcp_stream {
                let _ = stream.shutdown().await;
            }
        }

        self.tcp_stream = None;
        info!("Disconnected from server");
        Ok(())
    }

    /// Check if connected
    pub fn is_connected(&self) -> bool {
        self.tcp_stream.is_some()
    }
}

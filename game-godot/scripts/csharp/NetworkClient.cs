using Godot;
using System;
using System.Net.Sockets;
using System.Threading.Tasks;
using System.Collections.Generic;

namespace ApexSim;

public partial class NetworkClient : Node
{
    [Signal]
    public delegate void ConnectedToServerEventHandler();

    [Signal]
    public delegate void DisconnectedFromServerEventHandler();

    [Signal]
    public delegate void AuthenticationSuccessEventHandler(string playerId, uint serverVersion);

    [Signal]
    public delegate void AuthenticationFailedEventHandler(string reason);

    [Signal]
    public delegate void LobbyStateReceivedEventHandler();

    [Signal]
    public delegate void SessionJoinedEventHandler(string sessionId, byte gridPosition);

    [Signal]
    public delegate void SessionLeftEventHandler();

    [Signal]
    public delegate void ErrorReceivedEventHandler(ushort code, string message);

    private TcpClient? _tcpClient;
    private NetworkStream? _stream;
    private bool _isConnected = false;
    private string _playerId = "";
    private readonly Queue<ServerMessage> _messageQueue = new();

    // Store latest lobby state for retrieval
    public LobbyStateMessage? LastLobbyState { get; private set; }

    public string ServerAddress { get; set; } = "127.0.0.1";
    public int ServerPort { get; set; } = 9000;
    public string PlayerName { get; set; } = "Player";
    public string AuthToken { get; set; } = "dev-token";
    public new bool IsConnected => _isConnected;
    public string PlayerId => _playerId;

    public async void ConnectToServer()
    {
        try
        {
            GD.Print($"Connecting to {ServerAddress}:{ServerPort}...");
            _tcpClient = new TcpClient();
            await _tcpClient.ConnectAsync(ServerAddress, ServerPort);
            _stream = _tcpClient.GetStream();
            _tcpClient.NoDelay = true;
            _isConnected = true;

            GD.Print("Connected to server!");
            EmitSignal(SignalName.ConnectedToServer);

            // Auto-authenticate
            await AuthenticateAsync(PlayerName, AuthToken);

            // Start receiving messages
            _ = ReceiveMessagesAsync();
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Connection failed: {ex.Message}");
            _isConnected = false;
            EmitSignal(SignalName.DisconnectedFromServer);
        }
    }

    public async void DisconnectFromServer()
    {
        if (_isConnected)
        {
            await SendMessageAsync(new DisconnectMessage());
        }

        _stream?.Close();
        _tcpClient?.Close();
        _isConnected = false;
        _playerId = "";
        EmitSignal(SignalName.DisconnectedFromServer);
    }

    public async Task AuthenticateAsync(string name, string token)
    {
        PlayerName = name;
        AuthToken = token;
        await SendMessageAsync(new AuthenticateMessage
        {
            Token = token,
            PlayerName = name
        });
    }

    public async Task RequestLobbyStateAsync()
    {
        await SendMessageAsync(new RequestLobbyStateMessage());
    }

    public async Task SelectCarAsync(string carId)
    {
        await SendMessageAsync(new SelectCarMessage { CarConfigId = carId });
    }

    public async Task CreateSessionAsync(string trackId, byte maxPlayers, byte aiCount, byte lapLimit)
    {
        await SendMessageAsync(new CreateSessionMessage
        {
            TrackConfigId = trackId,
            MaxPlayers = maxPlayers,
            AiCount = aiCount,
            LapLimit = lapLimit
        });
    }

    public async Task JoinSessionAsync(string sessionId)
    {
        await SendMessageAsync(new JoinSessionMessage { SessionId = sessionId });
    }

    public async Task LeaveSessionAsync()
    {
        await SendMessageAsync(new LeaveSessionMessage());
    }

    public async Task StartSessionAsync()
    {
        await SendMessageAsync(new StartSessionMessage());
    }

    public async Task SendHeartbeatAsync(uint clientTick)
    {
        await SendMessageAsync(new HeartbeatMessage { ClientTick = clientTick });
    }

    private async Task SendMessageAsync(ClientMessage message)
    {
        if (_stream == null || !_isConnected)
        {
            GD.PrintErr("Not connected to server");
            return;
        }

        try
        {
            // Serialize message with bincode
            var writer = new BincodeWriter();
            message.Serialize(writer);
            var data = writer.ToArray();

            // Send length prefix (4 bytes, big-endian)
            var length = data.Length;
            var lengthBytes = new byte[4];
            lengthBytes[0] = (byte)((length >> 24) & 0xFF);
            lengthBytes[1] = (byte)((length >> 16) & 0xFF);
            lengthBytes[2] = (byte)((length >> 8) & 0xFF);
            lengthBytes[3] = (byte)(length & 0xFF);

            await _stream.WriteAsync(lengthBytes, 0, 4);
            await _stream.WriteAsync(data, 0, data.Length);
            await _stream.FlushAsync();

            GD.Print($"Sent: {message.GetType().Name}");
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Failed to send message: {ex.Message}");
            _isConnected = false;
            EmitSignal(SignalName.DisconnectedFromServer);
        }
    }

    private async Task ReceiveMessagesAsync()
    {
        var lengthBuffer = new byte[4];

        while (_isConnected && _stream != null)
        {
            try
            {
                // Read length prefix (4 bytes, big-endian)
                var bytesRead = await _stream.ReadAsync(lengthBuffer, 0, 4);
                if (bytesRead != 4)
                {
                    GD.PrintErr("Connection closed by server");
                    break;
                }

                var length = (lengthBuffer[0] << 24) | (lengthBuffer[1] << 16) |
                            (lengthBuffer[2] << 8) | lengthBuffer[3];

                if (length > 1024 * 1024) // 1MB limit
                {
                    GD.PrintErr($"Message too large: {length} bytes");
                    break;
                }

                // Read message data
                var dataBuffer = new byte[length];
                var totalRead = 0;
                while (totalRead < length)
                {
                    bytesRead = await _stream.ReadAsync(dataBuffer, totalRead, length - totalRead);
                    if (bytesRead == 0)
                    {
                        GD.PrintErr("Connection closed while reading message");
                        break;
                    }
                    totalRead += bytesRead;
                }

                if (totalRead != length)
                {
                    GD.PrintErr("Incomplete message received");
                    break;
                }

                // Deserialize with bincode
                var reader = new BincodeReader(dataBuffer);
                ServerMessage message;
                try
                {
                    message = ServerMessage.Deserialize(reader);
                }
                catch (Exception deserEx)
                {
                    GD.PrintErr($"Deserialization error: {deserEx.Message}");
                    GD.PrintErr($"Stack trace: {deserEx.StackTrace}");
                    throw;
                }

                // Queue message for processing on main thread
                lock (_messageQueue)
                {
                    _messageQueue.Enqueue(message);
                }
            }
            catch (Exception ex)
            {
                GD.PrintErr($"Error receiving message: {ex.Message}");
                break;
            }
        }

        _isConnected = false;
        EmitSignal(SignalName.DisconnectedFromServer);
    }

    public override void _Process(double delta)
    {
        // Process queued messages on main thread
        lock (_messageQueue)
        {
            while (_messageQueue.Count > 0)
            {
                var message = _messageQueue.Dequeue();
                HandleMessage(message);
            }
        }
    }

    private void HandleMessage(ServerMessage message)
    {
        GD.Print($"Received: {message.GetType().Name}");

        switch (message)
        {
            case AuthSuccessMessage authSuccess:
                _playerId = authSuccess.PlayerId;
                EmitSignal(SignalName.AuthenticationSuccess, authSuccess.PlayerId, authSuccess.ServerVersion);
                // Auto-request lobby state after auth
                _ = RequestLobbyStateAsync();
                break;

            case AuthFailureMessage authFailure:
                EmitSignal(SignalName.AuthenticationFailed, authFailure.Reason);
                break;

            case LobbyStateMessage lobbyState:
                LastLobbyState = lobbyState;
                EmitSignal(SignalName.LobbyStateReceived);
                break;

            case SessionJoinedMessage sessionJoined:
                EmitSignal(SignalName.SessionJoined, sessionJoined.SessionId, sessionJoined.YourGridPosition);
                break;

            case SessionLeftMessage:
                EmitSignal(SignalName.SessionLeft);
                break;

            case ErrorMessage error:
                EmitSignal(SignalName.ErrorReceived, error.Code, error.Message);
                break;

            case HeartbeatAckMessage:
                // Silently handle heartbeats
                break;

            default:
                GD.Print($"Unhandled message type: {message.GetType().Name}");
                break;
        }
    }

    public override void _ExitTree()
    {
        DisconnectFromServer();
    }
}

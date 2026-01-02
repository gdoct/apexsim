using Godot;
using System;
using System.Net.Sockets;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Linq;
using MessagePack;
using MessagePack.Resolvers;

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
    public delegate void SessionStartingEventHandler(byte countdownSeconds);

    [Signal]
    public delegate void ErrorReceivedEventHandler(ushort code, string message);

    private TcpClient? _tcpClient;
    private NetworkStream? _stream;
    private bool _isConnected = false;
    private string _playerId = "";
    private readonly Queue<ServerMessage> _messageQueue = new();
    private static readonly MessagePackSerializerOptions MsgPackOptions =
        MessagePackSerializerOptions.Standard.WithResolver(ContractlessStandardResolver.Instance);

    // Heartbeat management
    private double _heartbeatTimer = 0.0;
    private const double HeartbeatInterval = 2.0; // Send heartbeat every 2 seconds
    private uint _clientTick = 0;

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
            _heartbeatTimer = 0.0;
            _clientTick = 0;

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

    public async Task CreateSessionAsync(string trackId, byte maxPlayers, byte aiCount, byte lapLimit, SessionKind sessionKind = SessionKind.Multiplayer)
    {
        await SendMessageAsync(new CreateSessionMessage
        {
            TrackConfigId = trackId,
            MaxPlayers = maxPlayers,
            AiCount = aiCount,
            LapLimit = lapLimit,
            SessionKind = sessionKind
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
            var data = SerializeClientMessage(message);

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

            // GD.Print($"Sent: {message.GetType().Name}");
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

                var message = ParseServerMessage(dataBuffer);

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

        // Send periodic heartbeats to keep connection alive
        if (_isConnected)
        {
            _heartbeatTimer += delta;
            if (_heartbeatTimer >= HeartbeatInterval)
            {
                _heartbeatTimer = 0.0;
                _clientTick++;
                _ = SendHeartbeatAsync(_clientTick);
            }
        }
    }

    private void HandleMessage(ServerMessage message)
    {
        // GD.Print($"Received: {message.GetType().Name}");

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

            case SessionStartingMessage sessionStarting:
                GD.Print($"SessionStarting received! Countdown: {sessionStarting.CountdownSeconds}s");
                EmitSignal(SignalName.SessionStarting, sessionStarting.CountdownSeconds);
                break;

            case ErrorMessage error:
                EmitSignal(SignalName.ErrorReceived, error.Code, error.Message);
                break;

            case HeartbeatAckMessage:
                // Silently handle heartbeats
                break;

            case TelemetryMessage:
                // Silently handle telemetry (will be processed by game view when needed)
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


    private byte[] SerializeClientMessage(ClientMessage message)
    {
        string type;
        Dictionary<string, object?>? payload = null;

        switch (message)
        {
            case AuthenticateMessage auth:
                type = "Authenticate";
                payload = new Dictionary<string, object?>
                {
                    ["token"] = auth.Token,
                    ["player_name"] = auth.PlayerName
                };
                break;
            case HeartbeatMessage hb:
                type = "Heartbeat";
                payload = new Dictionary<string, object?> { ["client_tick"] = hb.ClientTick };
                break;
            case SelectCarMessage selectCar:
                type = "SelectCar";
                payload = new Dictionary<string, object?> { ["car_config_id"] = AsUuidBytes(selectCar.CarConfigId) };
                break;
            case RequestLobbyStateMessage:
                type = "RequestLobbyState";
                break;
            case CreateSessionMessage createSession:
                type = "CreateSession";
                payload = new Dictionary<string, object?>
                {
                    ["track_config_id"] = AsUuidBytes(createSession.TrackConfigId),
                    ["max_players"] = createSession.MaxPlayers,
                    ["ai_count"] = createSession.AiCount,
                    ["lap_limit"] = createSession.LapLimit,
                    ["session_kind"] = (byte)createSession.SessionKind
                };
                break;
            case JoinSessionMessage join:
                type = "JoinSession";
                payload = new Dictionary<string, object?> { ["session_id"] = AsUuidBytes(join.SessionId) };
                break;
            case LeaveSessionMessage:
                type = "LeaveSession";
                break;
            case StartSessionMessage:
                type = "StartSession";
                break;
            case DisconnectMessage:
                type = "Disconnect";
                break;
            default:
                throw new Exception($"Unsupported client message type: {message.GetType().Name}");
        }

        var envelope = new Dictionary<string, object?> { ["type"] = type };
        if (payload != null)
        {
            envelope["data"] = payload;
        }

        return MessagePackSerializer.Serialize(envelope, MsgPackOptions);
    }

    private ServerMessage ParseServerMessage(byte[] data)
    {
        var envelope = MessagePackSerializer.Deserialize<Dictionary<string, object?>>(data, MsgPackOptions);

        if (!envelope.TryGetValue("type", out var typeObj) || typeObj == null)
        {
            throw new Exception("Missing type field in server message");
        }

        var messageType = typeObj.ToString();
        envelope.TryGetValue("data", out var dataObj);

        //GD.Print($"Parsing message type: {messageType}");

        return messageType switch
        {
            "AuthSuccess" => BuildAuthSuccess(dataObj),
            "AuthFailure" => BuildAuthFailure(dataObj),
            "HeartbeatAck" => BuildHeartbeatAck(dataObj),
            "LobbyState" => BuildLobbyState(dataObj),
            "SessionJoined" => BuildSessionJoined(dataObj),
            "SessionLeft" => new SessionLeftMessage(),
            "SessionStarting" => BuildSessionStarting(dataObj),
            "Error" => BuildError(dataObj),
            "PlayerDisconnected" => BuildPlayerDisconnected(dataObj),
            "Telemetry" => new TelemetryMessage(),
            _ => throw new Exception($"Unknown server message type: {messageType}")
        };
    }

    private static AuthSuccessMessage BuildAuthSuccess(object? data)
    {
        var map = ToStringMap(data);
        return new AuthSuccessMessage
        {
            PlayerId = ReadUuid(map, "player_id"),
            ServerVersion = (uint)ReadUInt(map, "server_version")
        };
    }

    private static AuthFailureMessage BuildAuthFailure(object? data)
    {
        var map = ToStringMap(data);
        return new AuthFailureMessage { Reason = ReadString(map, "reason") };
    }

    private static HeartbeatAckMessage BuildHeartbeatAck(object? data)
    {
        var map = ToStringMap(data);
        return new HeartbeatAckMessage { ServerTick = (uint)ReadUInt(map, "server_tick") };
    }

    private static LobbyStateMessage BuildLobbyState(object? data)
    {
        var map = ToStringMap(data);

        var players = map.TryGetValue("players_in_lobby", out var playersObj)
            ? ToList(playersObj).Select(BuildLobbyPlayer).ToArray()
            : Array.Empty<LobbyPlayer>();

        var sessions = map.TryGetValue("available_sessions", out var sessionsObj)
            ? ToList(sessionsObj).Select(BuildSessionSummary).ToArray()
            : Array.Empty<SessionSummary>();

        var cars = map.TryGetValue("car_configs", out var carsObj)
            ? ToList(carsObj).Select(BuildCarConfig).ToArray()
            : Array.Empty<CarConfigSummary>();

        var tracks = map.TryGetValue("track_configs", out var tracksObj)
            ? ToList(tracksObj).Select(BuildTrackConfig).ToArray()
            : Array.Empty<TrackConfigSummary>();

        return new LobbyStateMessage
        {
            PlayersInLobby = players,
            AvailableSessions = sessions,
            CarConfigs = cars,
            TrackConfigs = tracks
        };
    }

    private static SessionJoinedMessage BuildSessionJoined(object? data)
    {
        var map = ToStringMap(data);
        return new SessionJoinedMessage
        {
            SessionId = ReadUuid(map, "session_id"),
            YourGridPosition = (byte)ReadUInt(map, "your_grid_position")
        };
    }

    private static SessionStartingMessage BuildSessionStarting(object? data)
    {
        var map = ToStringMap(data);
        return new SessionStartingMessage
        {
            CountdownSeconds = (byte)ReadUInt(map, "countdown_seconds")
        };
    }

    private static ErrorMessage BuildError(object? data)
    {
        var map = ToStringMap(data);
        return new ErrorMessage
        {
            Code = (ushort)ReadUInt(map, "code"),
            Message = ReadString(map, "message")
        };
    }

    private static PlayerDisconnectedMessage BuildPlayerDisconnected(object? data)
    {
        var map = ToStringMap(data);
        return new PlayerDisconnectedMessage
        {
            PlayerId = ReadUuid(map, "player_id")
        };
    }

    private static LobbyPlayer BuildLobbyPlayer(object? obj)
    {
        var map = ToStringMap(obj);
        return new LobbyPlayer
        {
            Id = ReadUuid(map, "id"),
            Name = ReadString(map, "name"),
            SelectedCar = ReadOptionalUuid(map, "selected_car"),
            InSession = ReadOptionalUuid(map, "in_session")
        };
    }

    private static SessionSummary BuildSessionSummary(object? obj)
    {
        var map = ToStringMap(obj);
        return new SessionSummary
        {
            Id = ReadUuid(map, "id"),
            TrackName = ReadString(map, "track_name"),
            TrackFile = ReadString(map, "track_file"),
            HostName = ReadString(map, "host_name"),
            PlayerCount = (byte)ReadUInt(map, "player_count"),
            MaxPlayers = (byte)ReadUInt(map, "max_players"),
            State = (SessionState)ReadUInt(map, "state")
        };
    }

    private static CarConfigSummary BuildCarConfig(object? obj)
    {
        var map = ToStringMap(obj);
        return new CarConfigSummary
        {
            Id = ReadUuid(map, "id"),
            Name = ReadString(map, "name")
        };
    }

    private static TrackConfigSummary BuildTrackConfig(object? obj)
    {
        var map = ToStringMap(obj);
        return new TrackConfigSummary
        {
            Id = ReadUuid(map, "id"),
            Name = ReadString(map, "name")
        };
    }

    private static Dictionary<string, object?> ToStringMap(object? data)
    {
        if (data == null)
        {
            return new Dictionary<string, object?>();
        }

        if (data is Dictionary<string, object?> dict)
        {
            return dict;
        }

        if (data is IDictionary<object, object?> genericDict)
        {
            var converted = new Dictionary<string, object?>();
            foreach (var kvp in genericDict)
            {
                converted[kvp.Key.ToString() ?? ""] = kvp.Value;
            }
            return converted;
        }

        throw new Exception("Expected map in server message payload");
    }

    private static IList<object?> ToList(object? data)
    {
        if (data is IList<object?> list)
        {
            return list;
        }

        if (data is object?[] arr)
        {
            return arr;
        }

        if (data is IEnumerable<object?> enumerable)
        {
            return enumerable.ToList();
        }

        throw new Exception("Expected list in server message payload");
    }

    private static string ReadString(Dictionary<string, object?> map, string key)
    {
        if (!map.TryGetValue(key, out var value) || value == null)
        {
            throw new Exception($"Missing field '{key}' in server message");
        }
        return value.ToString() ?? string.Empty;
    }

    private static string ReadUuid(Dictionary<string, object?> map, string key)
    {
        if (!map.TryGetValue(key, out var value) || value == null)
        {
            throw new Exception($"Missing field '{key}' in server message");
        }
        return ToUuidString(value);
    }

    private static string? ReadOptionalUuid(Dictionary<string, object?> map, string key)
    {
        if (!map.TryGetValue(key, out var value) || value == null)
        {
            return null;
        }
        return ToUuidString(value);
    }

    private static ulong ReadUInt(Dictionary<string, object?> map, string key)
    {
        if (!map.TryGetValue(key, out var value) || value == null)
        {
            throw new Exception($"Missing numeric field '{key}' in server message");
        }

        return value switch
        {
            byte b => b,
            sbyte sb => (ulong)sb,
            short s => (ulong)s,
            ushort us => us,
            int i => (ulong)i,
            uint ui => ui,
            long l => (ulong)l,
            ulong ul => ul,
            float f => (ulong)f,
            double d => (ulong)d,
            _ => throw new Exception($"Unsupported numeric type for '{key}': {value.GetType().FullName} (value: {value})")
        };
    }

    private static byte[] AsUuidBytes(string value)
    {
        var guid = Guid.Parse(value);
        return guid.ToByteArray();
    }

    private static string ToUuidString(object value)
    {
        switch (value)
        {
            case byte[] bytes when bytes.Length == 16:
                return new Guid(bytes).ToString();
            case IReadOnlyList<byte> byteList when byteList.Count == 16:
                return new Guid(byteList.ToArray()).ToString();
            default:
                return value.ToString() ?? string.Empty;
        }
    }
}
using System;

namespace ApexSim;

// Session States
public enum SessionState : byte
{
    Lobby = 0,
    Countdown = 1,
    Racing = 2,
    Finished = 3
}

// Session Types / Kinds
public enum SessionKind : byte
{
    Multiplayer = 0,
    Practice = 1,
    Sandbox = 2
}

// Game Modes
public enum GameMode : byte
{
    Lobby = 0,
    Sandbox = 1,
    Countdown = 2,
    DemoLap = 3,
    FreePractice = 4,
    Replay = 5,
    Qualification = 6,
    Race = 7
}

// Client Messages
public abstract class ClientMessage { }

public class AuthenticateMessage : ClientMessage
{
    public string Token { get; set; } = "";
    public string PlayerName { get; set; } = "";
}

public class HeartbeatMessage : ClientMessage
{
    public uint ClientTick { get; set; }
}

public class SelectCarMessage : ClientMessage
{
    public string CarConfigId { get; set; } = "";
}

public class RequestLobbyStateMessage : ClientMessage { }

public class CreateSessionMessage : ClientMessage
{
    public string TrackConfigId { get; set; } = "";
    public byte MaxPlayers { get; set; }
    public byte AiCount { get; set; }
    public byte LapLimit { get; set; }
    public SessionKind SessionKind { get; set; } = SessionKind.Multiplayer;
}

public class JoinSessionMessage : ClientMessage
{
    public string SessionId { get; set; } = "";
}

public class LeaveSessionMessage : ClientMessage { }

public class StartSessionMessage : ClientMessage { }

public class SetGameModeMessage : ClientMessage
{
    public GameMode Mode { get; set; }
}

public class StartCountdownMessage : ClientMessage
{
    public ushort CountdownSeconds { get; set; }
    public GameMode NextMode { get; set; }
}

public class DisconnectMessage : ClientMessage { }

// Server Messages
public abstract class ServerMessage { }

// Concrete server message types
public class AuthSuccessMessage : ServerMessage
{
    public string PlayerId { get; set; } = "";
    public uint ServerVersion { get; set; }
}

public class AuthFailureMessage : ServerMessage
{
    public string Reason { get; set; } = "";
}

public class HeartbeatAckMessage : ServerMessage
{
    public uint ServerTick { get; set; }
}

public class LobbyStateMessage : ServerMessage
{
    public LobbyPlayer[] PlayersInLobby { get; set; } = Array.Empty<LobbyPlayer>();
    public SessionSummary[] AvailableSessions { get; set; } = Array.Empty<SessionSummary>();
    public CarConfigSummary[] CarConfigs { get; set; } = Array.Empty<CarConfigSummary>();
    public TrackConfigSummary[] TrackConfigs { get; set; } = Array.Empty<TrackConfigSummary>();
}

public class SessionJoinedMessage : ServerMessage
{
    public string SessionId { get; set; } = "";
    public byte YourGridPosition { get; set; }
}

public class SessionLeftMessage : ServerMessage
{
}

public class SessionStartingMessage : ServerMessage
{
    public byte CountdownSeconds { get; set; }
}

public class ErrorMessage : ServerMessage
{
    public ushort Code { get; set; }
    public string Message { get; set; } = "";
}

public class PlayerDisconnectedMessage : ServerMessage
{
    public string PlayerId { get; set; } = "";
}

public class GameModeChangedMessage : ServerMessage
{
    public GameMode Mode { get; set; }
}

public class TelemetryMessage : ServerMessage
{
}

// Data structures
public class LobbyPlayer
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string? SelectedCar { get; set; }
    public string? InSession { get; set; }
}

public class SessionSummary
{
    public string Id { get; set; } = "";
    public string TrackName { get; set; } = "";
    public string TrackFile { get; set; } = "";
    public string HostName { get; set; } = "";
    public byte PlayerCount { get; set; }
    public byte MaxPlayers { get; set; }
    public SessionState State { get; set; }
}

public class CarConfigSummary
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string ModelPath { get; set; } = "";
    public float MassKg { get; set; } = 0;
    public float MaxEngineForceN { get; set; } = 0;
}

public class TrackConfigSummary
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
}

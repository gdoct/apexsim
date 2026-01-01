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

// Client Messages
public abstract class ClientMessage
{
    public abstract void Serialize(BincodeWriter writer);
}

public class AuthenticateMessage : ClientMessage
{
    public string Token { get; set; } = "";
    public string PlayerName { get; set; } = "";

    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(0); // Authenticate variant
        writer.WriteString(Token);
        writer.WriteString(PlayerName);
    }
}

public class HeartbeatMessage : ClientMessage
{
    public uint ClientTick { get; set; }

    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(1); // Heartbeat variant
        writer.WriteU32(ClientTick);
    }
}

public class SelectCarMessage : ClientMessage
{
    public string CarConfigId { get; set; } = "";

    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(2); // SelectCar variant
        writer.WriteUuid(CarConfigId);
    }
}

public class RequestLobbyStateMessage : ClientMessage
{
    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(3); // RequestLobbyState variant
    }
}

public class CreateSessionMessage : ClientMessage
{
    public string TrackConfigId { get; set; } = "";
    public byte MaxPlayers { get; set; }
    public byte AiCount { get; set; }
    public byte LapLimit { get; set; }

    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(4); // CreateSession variant
        writer.WriteUuid(TrackConfigId);
        writer.WriteU8(MaxPlayers);
        writer.WriteU8(AiCount);
        writer.WriteU8(LapLimit);
    }
}

public class JoinSessionMessage : ClientMessage
{
    public string SessionId { get; set; } = "";

    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(5); // JoinSession variant
        writer.WriteUuid(SessionId);
    }
}

public class LeaveSessionMessage : ClientMessage
{
    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(7); // LeaveSession variant
    }
}

public class StartSessionMessage : ClientMessage
{
    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(8); // StartSession variant
    }
}

public class DisconnectMessage : ClientMessage
{
    public override void Serialize(BincodeWriter writer)
    {
        writer.WriteVariantIndex(9); // Disconnect variant
    }
}

// Server Messages
public abstract class ServerMessage
{
    public static ServerMessage Deserialize(BincodeReader reader)
    {
        var variant = reader.ReadVariantIndex();
        return variant switch
        {
            0 => DeserializeAuthSuccess(reader),
            1 => DeserializeAuthFailure(reader),
            2 => DeserializeHeartbeatAck(reader),
            3 => DeserializeLobbyState(reader),
            4 => DeserializeSessionJoined(reader),
            5 => new SessionLeftMessage(),
            6 => DeserializeSessionStarting(reader),
            7 => DeserializeError(reader),
            8 => DeserializePlayerDisconnected(reader),
            9 => DeserializeTelemetry(reader),
            _ => throw new Exception($"Unknown server message variant: {variant}")
        };
    }

    private static AuthSuccessMessage DeserializeAuthSuccess(BincodeReader reader)
    {
        return new AuthSuccessMessage
        {
            PlayerId = reader.ReadUuid(),
            ServerVersion = reader.ReadU32()
        };
    }

    private static AuthFailureMessage DeserializeAuthFailure(BincodeReader reader)
    {
        return new AuthFailureMessage
        {
            Reason = reader.ReadString()
        };
    }

    private static HeartbeatAckMessage DeserializeHeartbeatAck(BincodeReader reader)
    {
        return new HeartbeatAckMessage
        {
            ServerTick = reader.ReadU32()
        };
    }

    private static LobbyStateMessage DeserializeLobbyState(BincodeReader reader)
    {
        return new LobbyStateMessage
        {
            PlayersInLobby = reader.ReadVec(() => DeserializeLobbyPlayer(reader)),
            AvailableSessions = reader.ReadVec(() => DeserializeSessionSummary(reader)),
            CarConfigs = reader.ReadVec(() => DeserializeCarConfig(reader)),
            TrackConfigs = reader.ReadVec(() => DeserializeTrackConfig(reader))
        };
    }

    private static LobbyPlayer DeserializeLobbyPlayer(BincodeReader reader)
    {
        Godot.GD.Print($"DeserializeLobbyPlayer start: pos={reader.Position}/{reader.Length}");
        var id = reader.ReadUuid();
        Godot.GD.Print($"After Id: pos={reader.Position}/{reader.Length}");

        var name = reader.ReadString();
        Godot.GD.Print($"After Name ({name}): pos={reader.Position}/{reader.Length}");

        var selectedCar = reader.ReadOptionUuid();
        Godot.GD.Print($"After SelectedCar: pos={reader.Position}/{reader.Length}");

        var inSession = reader.ReadOptionUuid();
        Godot.GD.Print($"After InSession: pos={reader.Position}/{reader.Length}");

        return new LobbyPlayer
        {
            Id = id,
            Name = name,
            SelectedCar = selectedCar,
            InSession = inSession
        };
    }

    private static SessionSummary DeserializeSessionSummary(BincodeReader reader)
    {
        return new SessionSummary
        {
            Id = reader.ReadUuid(),
            TrackName = reader.ReadString(),
            HostName = reader.ReadString(),
            PlayerCount = reader.ReadU8(),
            MaxPlayers = reader.ReadU8(),
            State = (SessionState)reader.ReadU32()
        };
    }

    private static CarConfigSummary DeserializeCarConfig(BincodeReader reader)
    {
        return new CarConfigSummary
        {
            Id = reader.ReadUuid(),
            Name = reader.ReadString()
        };
    }

    private static TrackConfigSummary DeserializeTrackConfig(BincodeReader reader)
    {
        return new TrackConfigSummary
        {
            Id = reader.ReadUuid(),
            Name = reader.ReadString()
        };
    }

    private static SessionJoinedMessage DeserializeSessionJoined(BincodeReader reader)
    {
        return new SessionJoinedMessage
        {
            SessionId = reader.ReadUuid(),
            YourGridPosition = reader.ReadU8()
        };
    }

    private static SessionStartingMessage DeserializeSessionStarting(BincodeReader reader)
    {
        return new SessionStartingMessage
        {
            CountdownSeconds = reader.ReadU8()
        };
    }

    private static ErrorMessage DeserializeError(BincodeReader reader)
    {
        return new ErrorMessage
        {
            Code = reader.ReadU16(),
            Message = reader.ReadString()
        };
    }

    private static PlayerDisconnectedMessage DeserializePlayerDisconnected(BincodeReader reader)
    {
        return new PlayerDisconnectedMessage
        {
            PlayerId = reader.ReadUuid()
        };
    }

    private static TelemetryMessage DeserializeTelemetry(BincodeReader reader)
    {
        // For now, skip telemetry parsing - it's complex and not needed for menus
        return new TelemetryMessage();
    }
}

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
    public string HostName { get; set; } = "";
    public byte PlayerCount { get; set; }
    public byte MaxPlayers { get; set; }
    public SessionState State { get; set; }
}

public class CarConfigSummary
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
}

public class TrackConfigSummary
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
}

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "ApexProtocolTypes.generated.h"

/**
 * ApexSim wire protocol v2 — mirrors `server/src/network.rs` and `server/src/data.rs`.
 *
 * Framing: [4-byte big-endian length][MessagePack payload], `to_vec_named`.
 *
 * Both ClientMessage and ServerMessage are Rust enums declared
 * `#[serde(tag = "type", content = "data")]` — ADJACENTLY TAGGED. Every message
 * is therefore a map `{"type": "<Variant>", "data": <payload>}`, and a unit
 * variant is a ONE-KEY map with no "data" key at all. Emitting `"data": {}` for
 * a unit variant fails `from_slice` on the server, which charges 10 rate-limit
 * violations per attempt (200 => forced disconnect).
 *
 * ==== THE CASE ASYMMETRY — the single most error-prone part of this port ====
 *
 * The enums carry no `rename_all`, so fields declared INLINE on a variant stay
 * snake_case:
 *     Authenticate { token, player_name, protocol_version }
 *     AuthFailure { reason } / HeartbeatAck { server_tick } / Error { code, message }
 *
 * But the standalone payload STRUCTS carry `#[serde(rename_all = "PascalCase")]`,
 * so their fields are PascalCase:
 *     AuthSuccessData { PlayerId, ServerVersion, ProtocolVersion, UdpToken, UdpPort }
 *     LobbyStateData  { PlayersInLobby, AvailableSessions, CarConfigs, TrackConfigs }
 *     SessionJoinedData { SessionId, YourGridPosition }
 *
 * And one exception breaks even that: TrackPoint (network.rs:318-322) has NO
 * rename_all, so its keys are lowercase "x"/"y" while nested inside a
 * PascalCase parent.
 *
 * Because the parsers skip unknown keys (for forward compatibility), getting a
 * key's case wrong does not error — it silently yields an empty list. That is
 * why ProtocolCodecTests pins every message against a golden byte blob.
 */

/** Wire protocol version. The server rejects anything but this (network.rs:32). */
inline constexpr uint8 APEXSIM_PROTOCOL_VERSION = 2;

/** Mirrors `SessionState` (data.rs:888). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexSessionState : uint8
{
	Lobby     = 0,
	Countdown = 1,
	Racing    = 2,
	Finished  = 3,
};

/** Mirrors `SessionKind` (data.rs:897). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexSessionKind : uint8
{
	Multiplayer = 0,
	Practice    = 1,
	Sandbox     = 2,
};

/** Mirrors `GameMode` (data.rs:906). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexGameMode : uint8
{
	Lobby         = 0,
	Sandbox       = 1,
	Countdown     = 2,
	DemoLap       = 3,
	FreePractice  = 4,
	Replay        = 5,
	Qualification = 6,
	Race          = 7,
};

/** Client-side connection lifecycle. Not a protocol type. */
UENUM(BlueprintType)
enum class EApexConnectionState : uint8
{
	Disconnected,
	Connecting,
	Authenticating,
	Authenticated,
	/** Auth was rejected or the network thread could not start; nothing retries. */
	Failed,
	/**
	 * The server could not be reached or dropped the connection, and the
	 * subsystem is retrying on a backoff. Cleared by a successful connect,
	 * an AuthFailure (-> Failed) or an explicit Disconnect().
	 */
	Reconnecting,
};

/**
 * IDs are kept as FString, deliberately not FGuid: Rust emits lowercase
 * hyphenated UUIDs, while FGuid::ToString() defaults to uppercase without
 * hyphens. Round-tripping through FGuid would silently corrupt every ID we
 * send back. Compare with ESearchCase::IgnoreCase throughout.
 */

/** `LobbyPlayer` (network.rs:242) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLobbyPlayer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Name;

	/** Empty when the player has not picked a car (nil on the wire). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString SelectedCar;

	/** Empty when the player is not in a session (nil on the wire). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString InSession;

	bool HasSelectedCar() const { return !SelectedCar.IsEmpty(); }
};

/** `SessionSummary` (network.rs:266) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSessionSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString TrackName;

	/** Track file relative to the content folder, e.g. "tracks/real/Austin.yaml". */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString TrackFile;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString HostName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 PlayerCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 MaxPlayers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	EApexSessionState State = EApexSessionState::Lobby;

	bool IsJoinable() const { return State == EApexSessionState::Lobby && PlayerCount < MaxPlayers; }
};

/** `CarConfigSummary` (network.rs:285) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarConfigSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Name;

	/**
	 * Informational only — DO NOT resolve this to a file. The server builds it
	 * as `res://content/cars/{uuid}/{model}` (broadcast.rs:150) but the folders
	 * on disk are named `redhorse-rb20` etc, so the path never exists. Meshes
	 * are resolved through DT_CarCatalog keyed by Id instead.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString ModelPath;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float MassKg = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float MaxEngineForceN = 0.0f;
};

/** `TrackConfigSummary` (network.rs:326) — PascalCase keys, but see FApexTrackPoint. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTrackConfigSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Name;

	/**
	 * Every 10th centerline point (broadcast.rs:162). Only populated when the
	 * cvar `apexsim.net.ParseCenterline` is on; the menu shell has no use for
	 * it and parsing ~14k points twice a minute is not free.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FVector2D> Centerline;
};

/** `LobbyStateData` (network.rs:174) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLobbyState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexLobbyPlayer> PlayersInLobby;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexSessionSummary> AvailableSessions;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexCarConfigSummary> CarConfigs;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexTrackConfigSummary> TrackConfigs;
};

/** `AuthSuccessData` (network.rs:131) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexAuthSuccess
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	int64 ServerVersion = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	int32 ProtocolVersion = 0;

	/** One-time token for the UDP handshake. Unused by the menu shell. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	FString UdpToken;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	int32 UdpPort = 0;
};

/**
 * One car in a `SessionRoster` (network.rs:457) — PascalCase keys, TCP.
 *
 * Compact telemetry identifies cars by a session-scoped index rather than a
 * UUID, and this is the only thing that maps that index back to a player.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexRosterEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsAi = false;
};

/** `SessionRosterData` (network.rs:470) — PascalCase keys, TCP. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSessionRoster
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexRosterEntry> Entries;
};

/**
 * One car from `CompactCarState` (network.rs:388).
 *
 * Only the fields the client actually uses are kept; the rest are still read
 * positionally because skipping is not optional in a positional encoding —
 * every field must be consumed in order to stay aligned.
 *
 * Coordinates are the server's: right-handed, metres, +X along the track, +Y
 * left, angles counter-clockwise from +X.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarTelemetry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	/** Server-space position, in metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float YawRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float PitchRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RollRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SpeedMps = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Throttle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Brake = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Steering = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Gear = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float EngineRpm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CurrentLap = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TrackProgress = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CurrentLapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsOnTrack = true;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsColliding = false;
};

/** `CompactTelemetry` (network.rs:415) — positional encoding, UDP. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTelemetryFrame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int64 ServerTick = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	EApexSessionState SessionState = EApexSessionState::Lobby;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	EApexGameMode GameMode = EApexGameMode::Lobby;

	/** -1 when the server sent nil. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CountdownMs = -1;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexCarTelemetry> Cars;
};

/** The player's control inputs, sent over UDP at frame rate. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexPlayerInput
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float Throttle = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float Brake = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float Steering = 0.0f;

	/** Negative leaves the gear alone (the protocol's `None`). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 Gear = -128;

	bool HasGear() const { return Gear != -128; }
};

/** The kind of a decoded server message. */
enum class EApexServerMessageType : uint8
{
	Unknown,
	AuthSuccess,
	AuthFailure,
	HeartbeatAck,
	LobbyState,
	SessionJoined,
	SessionLeft,
	SessionStarting,
	GameModeChanged,
	CountdownUpdate,
	Error,
	PlayerDisconnected,
	SessionRoster,
	UdpHandshakeAck,
	TelemetryCompact,
	/** Full named-encoding Telemetry — replays only; the wire uses the compact form. */
	IgnoredVariant,
};

/**
 * One decoded server message. A tagged union flattened into a struct: the set
 * of payloads is small and fixed, and a flat struct keeps the queue between the
 * socket thread and the game thread free of virtual dispatch.
 */
struct APEXSIMNET_API FApexServerMessage
{
	EApexServerMessageType Type = EApexServerMessageType::Unknown;

	/** The raw variant name, kept for logging unknown/ignored variants. */
	FString VariantName;

	FApexAuthSuccess AuthSuccess;
	FApexLobbyState LobbyState;
	FApexSessionRoster Roster;
	FApexTelemetryFrame Telemetry;

	/** AuthFailure::reason, or Error::message. */
	FString Reason;
	/** SessionJoined::SessionId. */
	FString SessionId;
	/** PlayerDisconnected::PlayerId. */
	FString PlayerId;

	int32 GridPosition = 0;
	int32 CountdownSeconds = 0;
	int64 ServerTick = 0;
	int32 ErrorCode = 0;
	EApexGameMode GameMode = EApexGameMode::Lobby;
};

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
 *     SessionJoinedData { SessionId, YourGridPosition, SessionKind }
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
	/**
	 * An AI-only race this client watches behind its menu: unlisted and
	 * unjoinable. Created by UApexDemoModeSubsystem; the net subsystem keeps
	 * it out of every session delegate (see IsInDemoSession).
	 */
	Demo        = 3,
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

/** What the driver does at a point of the racing line. The wire value is a u8. */
UENUM(BlueprintType)
enum class EApexLinePhase : uint8
{
	/** Flat out, or accelerating as hard as the car can. */
	Throttle = 0,
	/** At the grip limit through a corner, or lifting for a kink. */
	Partial  = 1,
	Brake    = 2,
};

/**
 * `RacingLineData` (network.rs) — PascalCase keys, TCP, sent once right after
 * SessionJoined.
 *
 * The line the joining player's car should take: a closed loop of evenly
 * spaced points, each tagged with what to do there. The server builds it from
 * the track's raceline and the car's grip, power and brakes, so the braking
 * points are that car's. On the wire the positions are parallel X/Y/Z arrays;
 * they are zipped into points here.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexRacingLineData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	/** Distance between consecutive points, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SpacingM = 0.0f;

	/** Server-frame positions, metres. The last point joins back to the first. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FVector> Points;

	/** One per point. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<EApexLinePhase> Phases;

	bool IsValid() const { return Points.Num() >= 3 && Phases.Num() == Points.Num(); }
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

/** What is under one tyre (`feedback::ContactSurface`). Ordered by roughness. */
enum class EApexContactSurface : uint8
{
	Road = 0,
	/** Within the curb band past the road edge. */
	Curb = 1,
	/** Past the curbs: grass, gravel, the verge. */
	Off  = 2,
};

/** One wheel's share of FApexDriverFeedback. Transients are the peak since the previous message. */
struct FApexWheelFeedback
{
	/** Longitudinal slip in multiples of the tyre's peak slip ratio, negative braking. Past ±1 the wheel is locking or spinning. */
	float SlipRatio = 0.0f;

	/** Slip angle in multiples of the tyre's peak slip angle. Past ±1 the tyre is sliding. */
	float SlipAngle = 0.0f;

	/** The roughest surface the tyre touched since the previous message. */
	EApexContactSurface Surface = EApexContactSurface::Road;

	/** Suspension compression speed, m/s (positive compressing), largest magnitude: bumps and landings. */
	float SuspensionMps = 0.0f;
};

/**
 * `DriverFeedback` (server/src/feedback.rs) - positional encoding, UDP only,
 * sent to a car's own driver with every telemetry frame. It carries what the
 * driver should feel, whatever the device: a wheel's constant force comes
 * from SteerTorque, and a pad's rumble from the slip, surface and hits.
 */
struct APEXSIMNET_API FApexDriverFeedback
{
	static constexpr int32 FrontLeft = 0;
	static constexpr int32 FrontRight = 1;
	static constexpr int32 RearLeft = 2;
	static constexpr int32 RearRight = 3;

	/** Tick of the newest sample; 0 until a message arrives. */
	int64 ServerTick = 0;

	/**
	 * Steering-column torque for every physics tick since the previous
	 * message, oldest first (4 at the default 240 Hz sim and 60 Hz telemetry).
	 * SERVER sign: positive turns the wheel LEFT. 1.0 is the car's reference,
	 * the front axle at its static grip limit; downforce takes it past 1.
	 */
	TArray<float, TInlineAllocator<8>> SteerTorque;

	/** FL, FR, RL, RR. */
	FApexWheelFeedback Wheels[4];

	/** ABS held a wheel back from locking during the interval. */
	bool bAbsActive = false;

	/** Traction control cut drive to a wheel during the interval. */
	bool bTcActive = false;

	/** Closing speed of the hardest contact with another car in the interval, m/s; 0 when none. */
	float ImpactMps = 0.0f;

	/**
	 * Folds the next message in, as if the server had sent one message for
	 * both intervals: samples appended, peaks kept, the roughest surface.
	 * Used when several arrive between two game frames, so a kerb strike in
	 * the first is not lost to the second.
	 */
	void Absorb(const FApexDriverFeedback& Next)
	{
		auto Peak = [](float Held, float New) { return FMath::Abs(New) > FMath::Abs(Held) ? New : Held; };
		ServerTick = Next.ServerTick;
		SteerTorque.Append(Next.SteerTorque);
		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			FApexWheelFeedback& Mine = Wheels[Wheel];
			const FApexWheelFeedback& Theirs = Next.Wheels[Wheel];
			Mine.SlipRatio = Peak(Mine.SlipRatio, Theirs.SlipRatio);
			Mine.SlipAngle = Peak(Mine.SlipAngle, Theirs.SlipAngle);
			Mine.SuspensionMps = Peak(Mine.SuspensionMps, Theirs.SuspensionMps);
			Mine.Surface = FMath::Max(Mine.Surface, Theirs.Surface);
		}
		bAbsActive |= Next.bAbsActive;
		bTcActive |= Next.bTcActive;
		ImpactMps = FMath::Max(ImpactMps, Next.ImpactMps);
	}
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
	RacingLine,
	UdpHandshakeAck,
	TelemetryCompact,
	DriverFeedback,
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
	FApexRacingLineData RacingLine;
	FApexTelemetryFrame Telemetry;
	FApexDriverFeedback DriverFeedback;

	/** AuthFailure::reason, or Error::message. */
	FString Reason;
	/** SessionJoined::SessionId. */
	FString SessionId;
	/** PlayerDisconnected::PlayerId. */
	FString PlayerId;

	int32 GridPosition = 0;
	/** SessionJoined::SessionKind; Multiplayer from a server that predates the field. */
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;
	int32 CountdownSeconds = 0;
	int64 ServerTick = 0;
	int32 ErrorCode = 0;
	EApexGameMode GameMode = EApexGameMode::Lobby;
};

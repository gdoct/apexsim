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
 *     SessionJoinedData { SessionId, YourGridPosition, SessionKind, AllowedAssists, Conditions }
 *
 * AllowedAssists is a struct with NO rename_all, so its own keys are
 * snake_case ("abs", "racing_line") even under SessionJoinedData's PascalCase
 * "AllowedAssists" key — the TrackPoint situation again. SessionConditions
 * ("weather", "time_of_day_minutes") is the same under "Conditions".
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

/** Mirrors `TractionControl` (data.rs). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexTractionControl : uint8
{
	Off  = 0,
	/** Cuts drive only when a driven wheel would spin. */
	Low  = 1,
	/** Cuts drive as soon as the tyre's combined grip runs out, keeping a margin for cornering. */
	High = 2,
};

/**
 * Mirrors `CarSetup` (car_setup.rs): the garage setup as *clicks* per knob,
 * one signed integer each, so neither the wire nor the client needs the
 * car's base figures — the server turns "+2" into newtons per metre against
 * the car.toml it loaded. Every knob has a fixed click range and a fixed
 * effect per click (ApexCarSetup::Knob); the server clamps whatever it is
 * sent, and all-zero is the car as filed. Encoded inline on SetCarSetup, so
 * the keys are snake_case; negative clicks are msgpack negative fixints.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarSetup
{
	GENERATED_BODY()

	static constexpr int32 KnobCount = 14;

	/** Clicks per knob, in ApexCarSetup::EKnob order. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Setup")
	TArray<int32> Clicks;

	FApexCarSetup()
	{
		Clicks.Init(0, KnobCount);
	}

	int32 GetClick(int32 Knob) const
	{
		return Clicks.IsValidIndex(Knob) ? Clicks[Knob] : 0;
	}

	/** Pins the value into the knob's range; returns whether anything changed. */
	bool SetClick(int32 Knob, int32 Value);

	/** Every knob pinned into range, as the server will do to it anyway. */
	void Clamp();

	/** True when every knob is at the file's value. */
	bool IsStock() const;

	/** How many knobs are away from the file's value. */
	int32 CountChanged() const;

	bool operator==(const FApexCarSetup& Other) const { return Clicks == Other.Clicks; }
	bool operator!=(const FApexCarSetup& Other) const { return !(*this == Other); }
};

namespace ApexCarSetup
{
	/** One knob of the setup: its wire key, click range and what a click does. */
	struct FKnob
	{
		/** The serde field name on the wire. */
		const ANSICHAR* Key;
		int32 Min;
		int32 Max;
		/** The size of one click for the read-out: "+2  (+8%)". */
		float PerClick;
		/** Unit of PerClick: "%", " kPa", " rpm". */
		const TCHAR* Unit;
	};

	/** The knobs in wire order — the same order as the server's `KNOBS`. */
	APEXSIMNET_API const FKnob& Knob(int32 Index);

	/** The read-out for a knob at a click count, e.g. "+2  (+8%)" or "0". */
	APEXSIMNET_API FString Describe(int32 Index, int32 Clicks);

	enum EKnob : int32
	{
		TyrePressureFront = 0,
		TyrePressureRear,
		RevLimiter,
		EngineBraking,
		FinalDrive,
		GearSpread,
		TorqueMap,
		BrakeBias,
		SpringFront,
		SpringRear,
		DamperFront,
		DamperRear,
		AntiRollFront,
		AntiRollRear,
	};
	static_assert(AntiRollRear + 1 == FApexCarSetup::KnobCount, "knob table and enum disagree");
}

/**
 * Mirrors `AllowedAssists` (data.rs): which driving aids a session's host lets
 * its drivers use, fixed when the session is created. Sent inside
 * CreateSession and echoed in SessionJoined; the server forces a disallowed
 * aid off for every driver, so the client only has to show the lock. Every
 * field is true from a server that predates it.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexAllowedAssists
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bAbs = true;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bTractionControl = true;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bAutoGearbox = true;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bSteeringAssist = true;

	/** The server sends no RacingLine at all when this is off. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bRacingLine = true;

	bool AllowsEverything() const
	{
		return bAbs && bTractionControl && bAutoGearbox && bSteeringAssist && bRacingLine;
	}

	/** How many of the five are locked. */
	int32 CountLocked() const
	{
		return (bAbs ? 0 : 1) + (bTractionControl ? 0 : 1) + (bAutoGearbox ? 0 : 1)
			+ (bSteeringAssist ? 0 : 1) + (bRacingLine ? 0 : 1);
	}

	bool operator==(const FApexAllowedAssists& Other) const
	{
		return bAbs == Other.bAbs && bTractionControl == Other.bTractionControl
			&& bAutoGearbox == Other.bAutoGearbox && bSteeringAssist == Other.bSteeringAssist
			&& bRacingLine == Other.bRacingLine;
	}
	bool operator!=(const FApexAllowedAssists& Other) const { return !(*this == Other); }
};

/** Mirrors `Weather` (data.rs). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexWeather : uint8
{
	Sunny     = 0,
	Cloudy    = 1,
	Overcast  = 2,
	LightRain = 3,
	HeavyRain = 4,
};

/**
 * Mirrors `SessionConditions` (data.rs): the weather and the clock over a
 * session, chosen by its host on create, echoed in SessionJoined and listed in
 * every SessionSummary. The server bakes the weather's grip into the session's
 * track; the client only has to draw it. A sunny 13:00 from a server that
 * predates the field.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSessionConditions
{
	GENERATED_BODY()

	static constexpr int32 MinutesPerDay = 24 * 60;
	static constexpr int32 DefaultTimeOfDayMinutes = 13 * 60;
	static constexpr int32 WeatherCount = 5;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	EApexWeather Weather = EApexWeather::Sunny;

	/** Local time of day, minutes after midnight, 0..1439. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 TimeOfDayMinutes = DefaultTimeOfDayMinutes;

	/** The clock wrapped onto one day, as the server does it. */
	FApexSessionConditions Clamped() const
	{
		FApexSessionConditions Out = *this;
		Out.TimeOfDayMinutes = ((TimeOfDayMinutes % MinutesPerDay) + MinutesPerDay) % MinutesPerDay;
		return Out;
	}

	bool IsWet() const { return Weather == EApexWeather::LightRain || Weather == EApexWeather::HeavyRain; }
	bool IsDefault() const { return Weather == EApexWeather::Sunny && TimeOfDayMinutes == DefaultTimeOfDayMinutes; }

	/** Hours since midnight, fractional. */
	float Hours() const { return static_cast<float>(Clamped().TimeOfDayMinutes) / 60.0f; }

	/** "21:30". */
	FString ClockText() const
	{
		const int32 Minutes = Clamped().TimeOfDayMinutes;
		return FString::Printf(TEXT("%02d:%02d"), Minutes / 60, Minutes % 60);
	}

	/** "Light rain". */
	static FString WeatherLabel(EApexWeather InWeather)
	{
		switch (InWeather)
		{
		case EApexWeather::Sunny:     return TEXT("Sunny");
		case EApexWeather::Cloudy:    return TEXT("Cloudy");
		case EApexWeather::Overcast:  return TEXT("Overcast");
		case EApexWeather::LightRain: return TEXT("Light rain");
		case EApexWeather::HeavyRain: return TEXT("Heavy rain");
		default:                      return TEXT("Sunny");
		}
	}

	/** "Light rain · 21:30". */
	FString Describe() const
	{
		return FString::Printf(TEXT("%s · %s"), *WeatherLabel(Weather), *ClockText());
	}

	bool operator==(const FApexSessionConditions& Other) const
	{
		return Weather == Other.Weather && TimeOfDayMinutes == Other.TimeOfDayMinutes;
	}
	bool operator!=(const FApexSessionConditions& Other) const { return !(*this == Other); }
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
	/**
	 * Time attack: every driver starts in the garage (tuning, not simulated),
	 * goes out onto a run-up before the line for flying laps and can come
	 * back in at any time (HotlapRelocate). Cars in the garage are neither
	 * ticked nor collided with, so several drivers can hotlap side by side.
	 */
	Hotlap        = 8,
};

/** Mirrors `HotlapDestination` (data.rs): where a hotlap driver asks to be put. */
UENUM(BlueprintType)
enum class EApexHotlapDestination : uint8
{
	/** Back to the garage: parked, frozen, out of everyone's way. */
	Garage = 0,
	/** Onto the track, on the run-up before the start line. */
	Track  = 1,
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

	/** The track config the session runs on; joins the track summaries and DT_TrackCatalog. Empty from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString TrackId;

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

	/** The session's weather and clock; a sunny afternoon from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FApexSessionConditions Conditions;

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
	 * on disk are named `fugazzi-sf26` etc, so the path never exists. Meshes
	 * are resolved through DT_CarCatalog keyed by Id instead.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString ModelPath;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float MassKg = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float MaxEngineForceN = 0.0f;

	/**
	 * Checksum of the car.toml the server loaded (content_crc.rs); compared
	 * with the DT_CarCatalog row's SourceCrc when the car is used. 0 from a
	 * server that predates it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int64 ContentCrc = 0;
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

	/**
	 * Checksum of the track file the server loaded (content_crc.rs); compared
	 * with the DT_TrackCatalog row's SourceCrc when the level is streamed. 0
	 * from a server that predates it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int64 ContentCrc = 0;
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

	/** The car this entry drives (the catalog's key); empty from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString CarConfigId;

	/**
	 * The livery it wears: 0 the car as authored, 1.. the catalog row's
	 * `Liveries` (the car.toml's `[[livery]]` tables). 0 from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Livery = 0;
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
 * `TrackSectorsData` (network.rs) — where the session's sector lines are.
 *
 * Sent once, with the racing line. Sector 1 always begins at the start line,
 * so `BoundariesM` holds the other two; a car's sector is read straight off
 * the station telemetry already carries.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTrackSectors
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	/** Lap length, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TrackLengthM = 0.0f;

	/** Station of each boundary past the line, ascending, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<float> BoundariesM;

	bool IsValid() const { return TrackLengthM > 1.0f && BoundariesM.Num() > 0; }

	/** Which sector (0-based) a car standing at `StationM` is driving. */
	int32 SectorAt(float StationM) const
	{
		int32 Sector = 0;
		for (int32 Index = 0; Index < BoundariesM.Num(); ++Index)
		{
			if (StationM >= BoundariesM[Index])
			{
				Sector = Index + 1;
			}
		}
		return Sector;
	}

	/** Sectors in a lap: one more than the boundaries past the line. */
	int32 SectorCount() const { return BoundariesM.Num() + 1; }
};

/**
 * `LapTimingData` (network.rs) — one car crossed a timing line.
 *
 * Timed on the server at the full tick rate; a client timing splits off 60 Hz
 * telemetry snapshots would be tens of milliseconds out.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLapTiming
{
	GENERATED_BODY()

	/** Index into the current session roster. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Lap = 0;

	/** The sector just completed, 0-based. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Sector = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SectorTimeMs = 0;

	/** The lap time when this sector closed the lap, 0 otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsLapEnd = false;

	/** The lap was inside track limits. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPersonalBestLap = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bSessionBestLap = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPersonalBestSector = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bSessionBestSector = false;
};

/**
 * `LapRecordData` (network.rs) — the driver's stored best on this track in
 * this car, sent on joining and again whenever they beat it.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLapRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString TrackId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString CarConfigId;

	/** Their best legal lap ever here in this car; 0 when they have none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> SplitsMs;

	/** This message announces a record just beaten. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsNew = false;

	/** The fastest lap anyone has set here in this car; 0 when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 TrackRecordMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString TrackRecordHolder;

	/** The record lap has a stored trace, so a ghost can be driven from it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bHasGhost = false;
};

/** One moment of a recorded lap, in the server frame (metres, radians). */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexGhostSample
{
	GENERATED_BODY()

	/** Milliseconds since the lap started. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 TimeMs = 0;

	/** Server frame: metres, +X along the track, +Y left. */
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

	/** -1..1, positive to the left, as the server holds it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Steering = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Gear = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float EngineRpm = 0.0f;
};

/**
 * `GhostLapData` (network.rs) — the trace of the driver's record lap on the
 * session's track in their car, asked for with RequestGhost. The wire
 * carries it as struct-of-arrays; here it is one sample per moment, which
 * is what a puppet car reads.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexGhostLap
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString TrackId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString CarConfigId;

	/** The lap the trace was recorded on; 0 when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SampleHz = 20.0f;

	/** Ascending in TimeMs, from the line to the line. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexGhostSample> Samples;

	bool IsValid() const { return LapTimeMs > 0 && Samples.Num() >= 2; }

	/**
	 * The lap's pose at `TimeMs`, blended between the samples either side
	 * (the yaw the short way round). Before the first sample it is the
	 * first; past the last it is the last, and the call returns false so a
	 * ghost that has finished its lap can be hidden.
	 */
	bool SampleAt(float TimeMs, FApexGhostSample& Out) const;
};

/**
 * One car's timing sheet, built from the `LapTiming` messages it has sent.
 *
 * The server is the stopwatch; this is only the tally, so the HUD and the
 * standings can read a car's sectors without recomputing anything.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarTiming
{
	GENERATED_BODY()

	/** Splits of the lap in progress; 0 where the sector is not done yet. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> CurrentSplitsMs;

	/** Splits of the last completed lap. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> LastSplitsMs;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LastLapMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLastLapValid = false;

	/** Best legal lap this session, 0 when the car has none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 BestLapMs = 0;

	/** The splits that best lap was made of. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> BestLapSplitsMs;

	/** Best time in each sector this session, whatever lap it came from. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> BestSplitsMs;

	/** The sector the car was last seen to complete, -1 before the first. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LastSector = -1;

	/**
	 * The three best sectors added up — the lap the driver has already shown
	 * they can do. 0 until every sector has a time.
	 */
	int32 OptimalLapMs() const
	{
		int32 Total = 0;
		for (int32 Split : BestSplitsMs)
		{
			if (Split <= 0)
			{
				return 0;
			}
			Total += Split;
		}
		return BestSplitsMs.Num() > 0 ? Total : 0;
	}
};

/**
 * Every car's timing sheet for the session, plus the session bests.
 *
 * Fed one `LapTiming` message at a time; nothing here is derived from
 * telemetry, so a dropped frame costs nothing.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTimingBoard
{
	GENERATED_BODY()

	/** By car index, as the roster numbers them. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TMap<int32, FApexCarTiming> Cars;

	/** The fastest legal lap anyone has set, and who set it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SessionBestLapMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SessionBestLapCarIndex = -1;

	/** The fastest each sector has been driven by anyone. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> SessionBestSplitsMs;

	/** How many sectors a lap has here; 0 until the track sectors arrive. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SectorCount = 0;

	const FApexCarTiming* Find(int32 CarIndex) const { return Cars.Find(CarIndex); }

	void Reset(int32 InSectorCount = 0)
	{
		Cars.Reset();
		SessionBestLapMs = 0;
		SessionBestLapCarIndex = -1;
		SectorCount = InSectorCount;
		SessionBestSplitsMs.Init(0, FMath::Max(0, InSectorCount));
	}

	/** Take one crossing. Safe to call before the sector count is known. */
	void Apply(const FApexLapTiming& Timing)
	{
		if (Timing.Sector < 0)
		{
			return;
		}
		if (SectorCount <= Timing.Sector)
		{
			SectorCount = Timing.Sector + 1;
		}
		auto Fit = [this](TArray<int32>& Splits)
		{
			if (Splits.Num() < SectorCount)
			{
				Splits.SetNumZeroed(SectorCount);
			}
		};
		Fit(SessionBestSplitsMs);

		FApexCarTiming& Car = Cars.FindOrAdd(Timing.CarIndex);
		Fit(Car.CurrentSplitsMs);
		Fit(Car.LastSplitsMs);
		Fit(Car.BestSplitsMs);
		Fit(Car.BestLapSplitsMs);

		Car.CurrentSplitsMs[Timing.Sector] = Timing.SectorTimeMs;
		Car.LastSector = Timing.Sector;

		// A sector best is only a best when the lap it is on is legal, which
		// is the server's call: it sets the flag, the client only files it.
		if (Timing.bPersonalBestSector)
		{
			Car.BestSplitsMs[Timing.Sector] = Timing.SectorTimeMs;
		}
		if (Timing.bSessionBestSector)
		{
			SessionBestSplitsMs[Timing.Sector] = Timing.SectorTimeMs;
		}

		if (!Timing.bIsLapEnd)
		{
			return;
		}
		Car.LastSplitsMs = Car.CurrentSplitsMs;
		Car.LastLapMs = Timing.LapTimeMs;
		Car.bLastLapValid = Timing.bValid;
		if (Timing.bPersonalBestLap)
		{
			Car.BestLapMs = Timing.LapTimeMs;
			Car.BestLapSplitsMs = Car.CurrentSplitsMs;
		}
		if (Timing.bSessionBestLap)
		{
			SessionBestLapMs = Timing.LapTimeMs;
			SessionBestLapCarIndex = Timing.CarIndex;
		}
		Car.CurrentSplitsMs.Init(0, SectorCount);
	}
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

	/**
	 * Classified position once the car has completed the race distance, 1 for
	 * the winner; 0 while it is still racing (the server sends nil). Set in
	 * crossing order and never changed afterwards.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 FinishPosition = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CurrentLapTimeMs = 0;

	/** The car's last completed lap, 0 when it has not finished one. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LastLapTimeMs = 0;

	/** Its best lap *inside track limits* this session, 0 when it has none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 BestLapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsOnTrack = true;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsColliding = false;

	/** The lap in progress has been struck for leaving the track. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLapInvalid = false;

	/** The lap this car just completed was struck. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLastLapInvalid = false;

	/**
	 * The car is parked in the garage of a hotlap session: not simulated,
	 * not to be drawn on the track. `lap_flags` bit 2.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bInGarage = false;

	/** The car may open its DRS flap where it is. `lap_flags` bit 3. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bDrsAllowed = false;

	/** The DRS flap is open. `lap_flags` bit 4. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bDrsOpen = false;

	/**
	 * The headlights are on: the driver's switch, or the session's sky when
	 * the switch is untouched (server `headlights.rs`). `lap_flags` bit 5.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bHeadlights = false;

	/** The driver is flashing the headlights. `lap_flags` bit 6. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bHeadlightFlash = false;
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
	 * The hardest jolt a hit put through the steering column in the interval,
	 * in SteerTorque's units and SERVER sign (positive turns the wheel left);
	 * 0 when none, and from a server that predates the field.
	 */
	float SteerKick = 0.0f;

	/**
	 * The steering input the newest SteerTorque sample was worked out at,
	 * -1..1, SERVER sign (positive left). With SteerStiffness it lets a wheel
	 * correct that sample for where the rim is now.
	 */
	float SteerInput = 0.0f;

	/**
	 * How the torque changes per unit of steering input around SteerInput,
	 * in SteerTorque's units. The same in either sign convention (both the
	 * torque and the input flip). Negative while the fronts grip, positive
	 * past the aligning crest; 0 from a server that predates the field, which
	 * turns the correction off.
	 */
	float SteerStiffness = 0.0f;

	/** Front axle load over its static share at the newest sample: above 1 under braking and with downforce. */
	float FrontLoad = 1.0f;

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
		SteerKick = Peak(SteerKick, Next.SteerKick);
		// These belong with the newest torque sample.
		SteerInput = Next.SteerInput;
		SteerStiffness = Next.SteerStiffness;
		FrontLoad = Next.FrontLoad;
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

	/**
	 * The DRS button is held. The server decides whether the flap opens
	 * (only in a zone the car earned; `FApexCarTelemetry::bDrsAllowed`).
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bDrs = false;

	/**
	 * The headlight switch: -1 leaves the lights to the session's sky (sent
	 * as nil; on at dusk, at night and in the rain), 0 off, 1 on.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 Headlights = -1;

	/** The flash button is held: full beam, whatever the switch says. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bFlash = false;

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
	TrackSectors,
	LapTiming,
	LapRecord,
	GhostLap,
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
	FApexTrackSectors TrackSectors;
	FApexLapTiming LapTiming;
	FApexLapRecord LapRecord;
	FApexGhostLap GhostLap;
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
	/** SessionJoined::AllowedAssists; everything allowed from a server that predates the field. */
	FApexAllowedAssists AllowedAssists;
	/** SessionJoined::Conditions; a sunny afternoon from a server that predates the field. */
	FApexSessionConditions Conditions;
	int32 CountdownSeconds = 0;
	int64 ServerTick = 0;
	int32 ErrorCode = 0;
	EApexGameMode GameMode = EApexGameMode::Lobby;
};

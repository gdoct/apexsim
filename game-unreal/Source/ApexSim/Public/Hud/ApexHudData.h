#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Hud/ApexHudValue.h"

/**
 * Everything the game publishes for the HUD, by name.
 *
 * The HUD components under content/hud read nothing else: a data point is the
 * whole interface between the game and a component, so a modder can lay out
 * any of it without touching C++. Scalars are `Values` (`car.speed_kph`,
 * `lap.last_s`, ...); things there are several of are `Lists` of records
 * (`standings`, `sectors`, `tyres`, `damage`), which a component walks with
 * a `repeat`.
 *
 * Every name is always present, with a `null` value when the game cannot
 * fill it (an older server, no car yet), so the set of names is the catalogue
 * and `apexsim.hud.Data` can list it. docs/game/hud-modding.md documents each one;
 * `ApexSim.Hud.Data.Documented` fails when a name is missing from it.
 */
struct APEXSIM_API FApexHudData
{
	TMap<FName, FApexHudValue> Values;
	TMap<FName, TArray<FApexHudRecord>> Lists;

	void Set(const TCHAR* Name, const FApexHudValue& Value) { Values.FindOrAdd(FName(Name)) = Value; }
	void Set(const TCHAR* Name, bool bValue) { Set(Name, FApexHudValue::Of(bValue)); }
	void Set(const TCHAR* Name, double Value) { Set(Name, FApexHudValue::Of(Value)); }
	void Set(const TCHAR* Name, float Value) { Set(Name, FApexHudValue::Of(Value)); }
	void Set(const TCHAR* Name, int32 Value) { Set(Name, FApexHudValue::Of(Value)); }
	void Set(const TCHAR* Name, const FString& Value) { Set(Name, FApexHudValue::Of(Value)); }
	void Set(const TCHAR* Name, const TCHAR* Value) { Set(Name, FApexHudValue::Of(Value)); }
	/** The name, with no value: what the game cannot fill this frame. */
	void SetNone(const TCHAR* Name) { Values.FindOrAdd(FName(Name)) = FApexHudValue(); }

	const FApexHudValue* Find(FName Name) const { return Values.Find(Name); }
	const TArray<FApexHudRecord>* FindList(FName Name) const { return Lists.Find(Name); }
};

/**
 * What the data is built from: the game's state, gathered by
 * UApexHudDataSubsystem from the net, flow and settings subsystems, or made
 * up by a test.
 */
struct APEXSIM_API FApexHudInputs
{
	const FApexTelemetryFrame* Frame = nullptr;
	/**
	 * The car the HUD is about: the player's own, or the car being watched
	 * while spectating (every `car.*`, `lap.*`, `tyre.*`... is that car's).
	 */
	int32 LocalCarIndex = -1;
	const FApexSessionRoster* Roster = nullptr;
	const FApexTimingBoard* Timing = nullptr;
	const FApexTrackSectors* Sectors = nullptr;

	/** The circuit's length from its catalog row; 0 when unknown (gaps and the delta then stay empty). */
	float TrackLengthM = 0.0f;
	/** The race distance in laps; 0 when there is none (a hotlap, practice). */
	int32 LapLimit = 0;
	EApexGameMode GameMode = EApexGameMode::Lobby;

	FString TrackName;
	FString CarName;
	FString ModeName;
	FApexSessionConditions Conditions;
	bool bHasConditions = false;
	/** Heartbeat round trip, ms; negative before the first. */
	int32 PingMs = -1;

	/** Settings. */
	bool bImperial = false;
	bool bFullDetail = true;
	/** The session's damage rule (SessionJoined), the same for every car. */
	EApexDamageLevel DamageLevel = EApexDamageLevel::Full;

	/** The local car's catalog row: its tyres' working window and its rev range (0 when unknown). */
	float TyreOptimalC = 90.0f;
	float TyreWindowC = 10.0f;
	float RedlineRpm = 0.0f;
	float LimiterRpm = 0.0f;

	/** Seconds since the race view opened, for blinking and fading. */
	double TimeSeconds = 0.0;

	/** The last input came from a gamepad (the hints show its buttons). */
	bool bGamepad = false;

	/** Watching a race rather than driving in it (docs/game/spectator.md, "Watching a race"). */
	bool bSpectating = false;
	/** What is being watched: `showcase`, `file`, `demo` or `live`. */
	FString SpectateSource;
	/** The watch camera's name, `TV`, `CHASE`, `ONBOARD`. */
	FString SpectateCamera;
	/** The TV director picks the car. */
	bool bSpectateAuto = false;
	/** The timing tower's column (ApexSpectate::TowerModeKey). */
	FString SpectateTowerMode = TEXT("interval");

	/** The race on screen is a watched hotlap (SessionKind::HotlapWatch): one AI car lapping alone. */
	bool bHotlapWatch = false;
	/** The track's corners (`TrackCorners`), for `corner.*`; null or not valid when none were sent. */
	const FApexTrackCorners* Corners = nullptr;

	/** A saved replay is playing (UApexReplayRecorder), and where its transport stands. */
	bool bReplay = false;
	double ReplaySeconds = 0.0;
	double ReplayDurationSeconds = 0.0;
	float ReplayRate = 1.0f;
	bool bReplayPaused = false;
	bool bReplayEnded = false;

	/** Each car's model by car index, from the roster and the car catalog; null when not known. */
	const TMap<int32, FString>* CarNames = nullptr;

	/**
	 * The local car's compound names from the setup sheet
	 * (FApexCarSetupSheet::Compounds), in the order telemetry indexes them;
	 * null or empty when the server named none, when the five defaults apply.
	 */
	const TArray<FString>* CompoundNames = nullptr;

	/** Each car's latest pit stop as its crew started it (`PitService`), by car index. */
	const TMap<int32, FApexPitService>* PitServices = nullptr;
};

/**
 * Where a pit stop stands: the crew's plan (`PitService`) read against the
 * seconds of service telemetry says are left. Pure, so the HUD's pit panel
 * can be tested without a server.
 */
struct APEXSIM_API FApexPitStopProgress
{
	/** The parts of a stop, in the order the crew does them. */
	enum EPart : int32 { Tyres = 0, Fuel = 1, Repair = 2, PartCount = 3 };

	/** The plan had any work in it. */
	bool bValid = false;
	float TotalS = 0.0f;
	float ElapsedS = 0.0f;
	/** ElapsedS over TotalS, 0 to 1. */
	float Progress = 0.0f;

	/** The part being worked on; INDEX_NONE when the plan is empty. Done, it is the last part. */
	int32 Phase = INDEX_NONE;
	/** `tyres`, `fuel`, `repair`, or empty. */
	FString PhaseKey;
	/** "CHANGING TYRES · SOFT", "REFUELLING · +25.0 L", "REPAIRING · 50%". */
	FString PhaseLabel;
	float PhaseLeftS = 0.0f;
	float PhaseProgress = 0.0f;

	/** Each part's seconds, its share of the stop (for sizing a bar's segments) and how much of it is done, 0 to 1. */
	float PartSeconds[PartCount] = {0.0f, 0.0f, 0.0f};
	float PartShare[PartCount] = {0.0f, 0.0f, 0.0f};
	float PartFill[PartCount] = {0.0f, 0.0f, 0.0f};
};


/**
 * What the data needs to remember between frames: the reference lap behind
 * the delta, what a lap of fuel costs, which damage zone was just hit.
 * Reset when a race view opens.
 */
struct APEXSIM_API FApexHudMemory
{
	/** Elapsed lap time sampled against the fraction of the lap, for the lap in progress. */
	TArray<TPair<float, float>> LapSamples;
	/** The same curve for the quickest legal lap watched so far; empty until one completes. */
	TArray<TPair<float, float>> ReferenceLap;
	float ReferenceLapSeconds = 0.0f;
	int32 LastSeenLap = 0;

	/** Highest RPM seen: the scale when the car's rev range is not known. */
	float ObservedMaxRpm = 8000.0f;

	int32 FuelLap = -1;
	float FuelAtLapStart = -1.0f;
	float FuelPerLap = -1.0f;

	/** The battery at the line, for what a lap gains or spends. A lap the HUD
	 * joined part-way, or one with a stop in it (the battery is refilled),
	 * is not whole and measures nothing. */
	int32 ErsCar = -1;
	int32 ErsLap = -1;
	float ErsAtLapStart = -1.0f;
	bool bErsLapWhole = false;
	bool bErsLastLapKnown = false;
	float ErsLastLapNet = 0.0f;

	float LastDamagePct[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	double DamageFlashUntil[5] = {0.0, 0.0, 0.0, 0.0, 0.0};

	/**
	 * What every car has done since the HUD first saw it, for the timing
	 * tower: the stops it made and the lap its tyres went on. Kept when the
	 * HUD moves to another car.
	 */
	struct FCarHistory
	{
		int32 PitStops = 0;
		/** The lap counter when the set on the car was fitted. */
		int32 TyresFromLap = 1;
		int32 Compound = -1;
		bool bWasServicing = false;
	};
	TMap<int32, FCarHistory> Cars;

	/**
	 * The running order as last taken, with each car's race distance (m) and
	 * speed (m/s) then: the places and gaps are read from it, so two cars
	 * side by side do not swap places on screen every frame.
	 */
	struct FStanding
	{
		int32 CarIndex = -1;
		float Progress = 0.0f;
		float Speed = 0.0f;
	};
	TArray<FStanding> Standings;
	/** The HUD clock when Standings was taken; negative before the first. */
	double StandingsTakenAt = -1.0;

	void Reset() { *this = FApexHudMemory(); }

	/**
	 * The HUD moved to another car: its delta, fuel, rev scale and damage
	 * flashes were the other car's. The field's history stays.
	 */
	void ResetForNewCar()
	{
		TMap<int32, FCarHistory> Kept = MoveTemp(Cars);
		*this = FApexHudMemory();
		Cars = MoveTemp(Kept);
	}

	/** Note this frame's pit stops and tyre changes, car by car. */
	void SampleField(const FApexTelemetryFrame& Frame);

	/**
	 * Feed one telemetry frame's local car into the delta's reference. Driven
	 * by the stream rather than the frame rate: a lap rolling over is an event
	 * on the stream, and sampling it at frame rate would miss the frame the
	 * counter moved on a slow client.
	 */
	void SampleLap(const FApexCarTelemetry& Local, float TrackLengthM);

	/** Reference-lap time at a fraction of the lap, or -1 with no reference. */
	float ReferenceTimeAt(float Fraction) const;
};

/**
 * A made-up race for the HUD editor to show when there is no real one: ten
 * cars mid-race, the player fourth on a hybrid with warm tyres, a little
 * damage and a delta, and a pit stop under way, so every component has
 * something to draw.
 */
struct APEXSIM_API FApexHudPreview
{
	FApexTelemetryFrame Frame;
	FApexSessionRoster Roster;
	FApexTimingBoard Timing;
	FApexTrackSectors Sectors;
	/** A circuit-shaped loop for the minimap. */
	TArray<FVector2D> Outline;
	/** The player's stop in progress, so the pit panel has something to place. */
	TMap<int32, FApexPitService> PitServices;
	FApexHudMemory Memory;
	/** Points into this struct, so it must not be copied once made. */
	FApexHudInputs Inputs;

	FApexHudPreview();
	FApexHudPreview(const FApexHudPreview&) = delete;
	FApexHudPreview& operator=(const FApexHudPreview&) = delete;
};

namespace ApexHudData
{
	/** Fills Out from In, every name every time (see FApexHudData). */
	APEXSIM_API void Build(const FApexHudInputs& In, FApexHudMemory& Memory, FApexHudData& Out);

	/** The lists Build fills, and the fields of each list's records. */
	APEXSIM_API const TMap<FName, TArray<FName>>& ListFields();

	/** Every scalar name, from a build with no game state: the catalogue. */
	APEXSIM_API TArray<FName> ScalarNames();

	/**
	 * A stop's progress from its plan and the seconds of service left. The
	 * parts run tyres, fuel, repairs; a part of no seconds is skipped.
	 */
	APEXSIM_API FApexPitStopProgress PitStopProgress(const FApexPitService& Stop, float SecondsLeft,
		const TArray<FString>* CompoundNames = nullptr);
}

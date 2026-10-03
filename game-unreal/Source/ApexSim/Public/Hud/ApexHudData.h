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
 * and `apexsim.hud.Data` can list it. docs/HUD_MODDING.md documents each one;
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

	/** Watching a race rather than driving in it (docs/SPECTATOR.md, "Watching a race"). */
	bool bSpectating = false;
	/** What is being watched: `showcase`, `file`, `demo` or `live`. */
	FString SpectateSource;
	/** The watch camera's name, `TV`, `CHASE`, `ONBOARD`. */
	FString SpectateCamera;
	/** The TV director picks the car. */
	bool bSpectateAuto = false;
	/** The timing tower's column (ApexSpectate::TowerModeKey). */
	FString SpectateTowerMode = TEXT("interval");

	/** A saved replay is playing (UApexReplayRecorder), and where its transport stands. */
	bool bReplay = false;
	double ReplaySeconds = 0.0;
	double ReplayDurationSeconds = 0.0;
	float ReplayRate = 1.0f;
	bool bReplayPaused = false;
	bool bReplayEnded = false;

	/** Each car's model by car index, from the roster and the car catalog; null when not known. */
	const TMap<int32, FString>* CarNames = nullptr;
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
 * damage and a delta, so every component has something to draw.
 */
struct APEXSIM_API FApexHudPreview
{
	FApexTelemetryFrame Frame;
	FApexSessionRoster Roster;
	FApexTimingBoard Timing;
	FApexTrackSectors Sectors;
	/** A circuit-shaped loop for the minimap. */
	TArray<FVector2D> Outline;
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
}

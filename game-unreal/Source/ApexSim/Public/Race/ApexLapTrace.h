#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * A driven lap sampled by distance, for telemetry logging and lap
 * comparison (docs/game/client.md, "Lap traces"): what the car did at each
 * metre of the lap, from the telemetry the client already has for its own
 * car. Pure data and maths; UApexLapTraceSubsystem records and stores them,
 * UApexLapTraceWidget draws two against each other.
 */
struct APEXSIM_API FApexTraceSample
{
	/** Metres from the line (the telemetry's station). */
	float DistanceM = 0.0f;
	/** Seconds into the lap. */
	float TimeS = 0.0f;
	float SpeedKph = 0.0f;
	float Throttle = 0.0f;
	float Brake = 0.0f;
	/** -1 full left to 1 full right. */
	float Steering = 0.0f;
	int32 Gear = 0;
	float Rpm = 0.0f;
	/** Tread, FL FR RL RR; -1 unknown. */
	float TyreC[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
	/** Server frame, metres. */
	float X = 0.0f;
	float Y = 0.0f;
};

struct APEXSIM_API FApexLapTrace
{
	FString TrackId;
	FString CarId;
	int32 LapNumber = 0;
	/** The lap's time as the server timed it. */
	int32 LapMs = 0;
	/** Inside track limits. */
	bool bValid = false;
	FDateTime RecordedAt;
	/** In distance order. */
	TArray<FApexTraceSample> Samples;

	bool IsEmpty() const { return Samples.Num() < 2; }
	float LengthM() const { return Samples.Num() > 0 ? Samples.Last().DistanceM : 0.0f; }

	/** The sample at a distance, every field interpolated between its neighbours (gear the nearer's). */
	FApexTraceSample At(float DistanceM) const;
};

namespace ApexLapTrace
{
	/** Every so many metres a sample is kept (a 5 km lap is 2500 rows). */
	inline constexpr float SampleSpacingM = 2.0f;

	/**
	 * The trace as CSV: a `#` header line with the track, car, lap, time and
	 * validity, then a row per sample (distance_m, time_s, speed_kph,
	 * throttle, brake, steering, gear, rpm, tyre_fl_c ... tyre_rr_c, x, y).
	 */
	APEXSIM_API FString ToCsv(const FApexLapTrace& Trace);
	/** Reads what ToCsv wrote; false for anything else. */
	APEXSIM_API bool FromCsv(const FString& Text, FApexLapTrace& Out);

	/**
	 * The time Lap loses (+) or gains (-) against Reference at each StepM of
	 * distance, from the start to the shorter trace's end: X distance, Y
	 * seconds.
	 */
	APEXSIM_API TArray<FVector2f> DeltaByDistance(const FApexLapTrace& Reference, const FApexLapTrace& Lap, float StepM);

	/** `<yyyymmdd-hhmmss>_lap<N>_<m-ss-mmm>[_invalid].csv`. */
	APEXSIM_API FString FileName(const FApexLapTrace& Trace);
}

/**
 * Turns the local car's telemetry, frame by frame, into whole laps. A lap
 * is kept only when it was seen from the line (its first sample within
 * 30 m and 2 s of it) to the next crossing with no hole of more than 100 m:
 * an out-lap from the pits, a lap joined part-way, a recovery or a rejoin
 * gives none.
 */
class APEXSIM_API FApexLapRecorder
{
public:
	void Reset();

	/**
	 * One frame of the car. True when the car has just crossed the line
	 * having driven the lap before it whole: that lap is in OutLap.
	 */
	bool Add(const FApexCarTelemetry& Car, FApexLapTrace& OutLap);

private:
	FApexLapTrace Current;
	int32 CurrentLap = -1;
	bool bWhole = false;
	float LastDistanceM = -1.0f;
};

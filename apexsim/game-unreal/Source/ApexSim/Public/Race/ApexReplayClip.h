#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSpectatorStream.h"

class FApexStreamFile;

/**
 * A race clip played back from disk (`-ApexReplay=<file>.apxs`, or the
 * older `<file>.clip.json`).
 *
 * The server's `apexsim-replay` tool simulates an AI race, finds the moment
 * worth filming and cuts it into a spectator stream (`server/src/spectator.rs`,
 * docs/game/spectator.md), whose frames are read into the same table the JSON
 * clip (`replay_tools::ClipFile`) filled: every car per frame as a
 * positional row in roster order, in the server frame:
 *
 *   [x, y, z, yaw, pitch, roll, speed_mps, throttle, brake, steering, gear,
 *    rpm, lap, station_m, on_track (0/1), finish_position (0 = none)]
 *
 * Pure data and maths: no world, no net. `SampleAt` is what a fixed-timestep
 * frame dump plays by, so a clip renders the same every time.
 */
class APEXSIM_API FApexReplayClip
{
public:
	/** What the file says about each car, in roster order. */
	struct FCarInfo
	{
		int32 Index = 0;
		FString Name;
		FString CarConfigId;
		int32 Livery = 0;
	};

	/** Values per car per frame; the array layout above. */
	static constexpr int32 FieldsPerCar = 16;

	/** Parse a clip; false with a reason when it is not one. */
	bool LoadFromString(const FString& Json, FString& OutError);
	/** A stream's frames (its parts put together), roster, path and sky. */
	bool LoadFromStream(const FApexStreamFile& File, FString& OutError);
	/** `.apxs` by its magic, else the JSON clip. */
	bool LoadFromFile(const FString& Path, FString& OutError);

	bool IsValid() const { return Ticks.Num() > 0 && Cars.Num() > 0; }

	/** Seconds from the first frame to the last. */
	double GetDurationSeconds() const;

	/**
	 * The field at `Seconds` after the first frame: positions and the
	 * continuous values blended between the frames either side (angles the
	 * short way round, the lap station across the line), the discrete ones
	 * (gear, state) from the earlier frame, the lap from whichever side of the
	 * line the blended station is on. Clamped to the clip. A car the clip
	 * has no row for in a frame comes back with `bInGarage` set: not drawn.
	 */
	void SampleAt(double Seconds, FApexTelemetryFrame& OutFrame) const;

	/** A roster the race director can spawn the field from. */
	FApexSessionRoster MakeRoster() const;

	/**
	 * The sky as the stream carried it `Seconds` into the clip (the rain
	 * arriving, the clock moving); `bValid` is false from a clip with none,
	 * and the header's fixed conditions are the sky then.
	 */
	FApexSkyNow SkyAt(double Seconds) const;

	/** The tick `Seconds` into the clip. */
	int64 TickAt(double Seconds) const { return Ticks.Num() > 0 ? Ticks[0] + static_cast<int64>(FMath::Max(Seconds, 0.0) * TickRate) : 0; }

	/** The road-state slices the stream carried, in file order (empty from a clip with none). */
	const TArray<FApexStreamRoad>& GetRoads() const { return Roads; }

	FApexSessionConditions GetConditions() const { return Conditions; }
	const FString& GetTrackStem() const { return TrackStem; }
	const FString& GetTrackId() const { return TrackId; }
	const FString& GetTrackName() const { return TrackName; }
	float GetTrackLengthM() const { return TrackLengthM; }
	const TArray<FVector2D>& GetCenterline() const { return Centerline; }
	const TArray<FCarInfo>& GetCars() const { return Cars; }
	int32 NumFrames() const { return Ticks.Num(); }

	/**
	 * The car furthest round the race at `Seconds` (laps, then station), or
	 * INDEX_NONE: who a clip follows when it is told to follow the leader.
	 */
	int32 LeaderAt(double Seconds) const;

	/**
	 * The car nearest a point (server frame, metres) at `Seconds`: who a
	 * fixed camera should be watching as the field goes by it.
	 */
	int32 NearestCarTo(const FVector& ServerMetres, double Seconds) const;

private:
	/** Every value zero: the car was not in that frame. */
	static bool IsEmptyRow(const float* Row);
	/** Frame index at or before `Seconds`, and how far to the next (0..1). */
	void Locate(double Seconds, int32& OutIndex, double& OutAlpha) const;
	const float* CarValues(int32 Frame, int32 Car) const
	{
		return Values.GetData() + (static_cast<int64>(Frame) * Cars.Num() + Car) * FieldsPerCar;
	}

	FString TrackName;
	FString TrackStem;
	FString TrackId;
	float TrackLengthM = 0.0f;
	int32 TickRate = 420;
	FApexSessionConditions Conditions;
	TArray<FCarInfo> Cars;
	TArray<FVector2D> Centerline;

	TArray<int64> Ticks;
	TArray<uint8> States;
	TArray<int32> CountdownMs;
	/** Frames x cars x FieldsPerCar, flat. */
	TArray<float> Values;
	/** The sky of each frame, parallel to Ticks (empty when no frame had one). */
	TArray<FApexSkyNow> Skies;
	TArray<FApexStreamRoad> Roads;
};

#pragma once

#include "CoreMinimal.h"

namespace ApexTv
{
	struct FPath;
}

/**
 * Marshals and the recovery vehicle at a stopped car, drawn by the client
 * from telemetry alone (docs/game/cameras.md, "Marshals and recovery"):
 * a car stopped off the road, out of the race on it, or stranded on it
 * for a while gets two marshals waving yellow at the verge a little up
 * the road, then a recovery tractor that drives up to it and stays until
 * the car is towed (it vanishes to its pit box), drives on or the race
 * ends. Pure logic; AApexRaceDirector draws it.
 */
namespace ApexMarshals
{
	/** One car as the telemetry has it this frame, Unreal space. */
	struct FCarSample
	{
		int32 CarIndex = INDEX_NONE;
		FVector Location = FVector::ZeroVector;
		float YawDeg = 0.0f;
		float SpeedMps = 0.0f;
		/** On the road (not past the kerbs). */
		bool bOnTrack = true;
		/** In the pit lane or its garage. */
		bool bInPits = false;
		/** A damage zone at 100%: out of the race, to be towed. */
		bool bOut = false;
	};

	/** Slower than this a car counts as stopped. */
	inline constexpr float StoppedMps = 1.5f;
	/** Stopped this long off the road, or out, before the marshals come out. */
	inline constexpr float OffRoadAfterS = 2.0f;
	/** Stopped this long on the road (a stall, a spin left facing the wall). */
	inline constexpr float OnRoadAfterS = 6.0f;
	/**
	 * From the marshals coming out to the recovery vehicle setting off, and
	 * its drive up to the car: it is there before the server tows a car out
	 * of the race (`TOW_AFTER_S`, 10 s after it went out).
	 */
	inline constexpr float TruckAfterS = 1.5f;
	inline constexpr float TruckDriveS = 5.0f;
	/** How far back up the road it sets off from, and where it stops behind the car. */
	inline constexpr double TruckFromCm = 4500.0;
	inline constexpr double TruckStopCm = 650.0;
	/** The car's own side of the road, this far further out than the car, for the truck. */
	inline constexpr double TruckBesideCm = 250.0;
	/** The marshals: this far up the road from the car, on its side, at least this far out. */
	inline constexpr double PostUpCm = 1200.0;
	inline constexpr double PostOutCm = 350.0;
	inline constexpr double PostMinLateralCm = 700.0;
	/** Seconds everything stays after the car has gone (towed, or driven off). */
	inline constexpr float LingerS = 5.0f;
	/** A car this far from where it stopped has been moved (towed to its box). */
	inline constexpr double MovedAwayCm = 3000.0;
	/** Faster than this a stopped car is driving on. */
	inline constexpr float DrivingOnMps = 4.0f;
	/** At most this many scenes at once; a new one takes the oldest's place. */
	inline constexpr int32 MaxIncidents = 3;

	struct APEXSIM_API FIncident
	{
		int32 CarIndex = INDEX_NONE;
		/** Where the car stopped. */
		FVector CarLocation = FVector::ZeroVector;
		/** Down the road at the car (unit, level), and the car's side of the road (unit, level). */
		FVector Along = FVector::ForwardVector;
		FVector Outward = FVector::RightVector;
		/** Where the marshals stand; they face the oncoming cars (-Along). */
		FVector Post = FVector::ZeroVector;
		/** The recovery vehicle's path: from up the road to just behind the car. */
		FVector TruckFrom = FVector::ZeroVector;
		FVector TruckTo = FVector::ZeroVector;
		/** Seconds since the marshals came out. */
		float Age = 0.0f;
		/** Seconds since the car went (towed or drove on); negative while it is there. */
		float GoneFor = -1.0f;

		bool HasTruck() const { return Age >= TruckAfterS; }
		/** The recovery vehicle now, and its heading. */
		FVector TruckLocation() const;
		float TruckYawDeg() const;
		/** Whether the marshals still wave (until the car has gone). */
		bool IsWaving() const { return GoneFor < 0.0f; }
	};

	class APEXSIM_API FTracker
	{
	public:
		void Reset();

		/**
		 * Advance a frame. `bRacing`: the race is on (a grid, a finished race
		 * or another session state clears everything). `Path` (optional) is
		 * the circuit's centerline, which puts the marshals at the verge.
		 */
		void Tick(TConstArrayView<FCarSample> Cars, bool bRacing, float DeltaSeconds, const ApexTv::FPath* Path);

		const TArray<FIncident>& GetIncidents() const { return Incidents; }

	private:
		FIncident Open(const FCarSample& Car, const ApexTv::FPath* Path) const;

		struct FCarMemory
		{
			/** Seconds it has been stopped (outside the pits). */
			float StillFor = 0.0f;
			FVector Last = FVector::ZeroVector;
			/** Its scene was dropped or has run its course: none again until it moves. */
			bool bDismissed = false;
		};
		TMap<int32, FCarMemory> Memory;
		TArray<FIncident> Incidents;
	};
}

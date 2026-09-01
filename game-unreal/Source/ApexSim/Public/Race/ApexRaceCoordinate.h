#pragma once

#include "CoreMinimal.h"

/**
 * The one place the server's coordinate system meets Unreal's.
 *
 * Server (see CLAUDE.md): right-handed, metres, origin at the start/finish
 * line, +X along the track direction, +Y to the left of the track, yaw
 * counter-clockwise from +X.
 *
 * Unreal: left-handed, centimetres, +X forward, +Y to the RIGHT, yaw clockwise
 * when viewed from above.
 *
 * So the conversion is a metres-to-centimetres scale plus a handedness flip on
 * Y, and the flip means every angle changes sign too. Getting this wrong is
 * subtle rather than obvious — cars drive mirrored, and corners bend the wrong
 * way — so it lives in one header that everything else goes through.
 */
namespace ApexRace
{
	/** Unreal works in centimetres; the server works in metres. */
	inline constexpr double MetresToCentimetres = 100.0;

	/** Server position (metres, +Y left) -> Unreal world position (cm, +Y right). */
	inline FVector ServerToUnrealPosition(const FVector& ServerMetres)
	{
		return FVector(
			ServerMetres.X * MetresToCentimetres,
			-ServerMetres.Y * MetresToCentimetres,
			ServerMetres.Z * MetresToCentimetres);
	}

	/**
	 * Server orientation (radians, counter-clockwise) -> Unreal rotator.
	 *
	 * Yaw and roll negate with the Y flip; pitch is about the (unflipped) Y axis
	 * in both systems and keeps its sign.
	 */
	inline FRotator ServerToUnrealRotation(float YawRad, float PitchRad, float RollRad)
	{
		return FRotator(
			FMath::RadiansToDegrees(PitchRad),
			-FMath::RadiansToDegrees(YawRad),
			-FMath::RadiansToDegrees(RollRad));
	}

	/** Metres per second -> kilometres per hour, for anything user-facing. */
	inline float MpsToKph(float Mps)
	{
		return Mps * 3.6f;
	}

	/**
	 * Race distance in metres, for ordering cars on track.
	 *
	 * The wire's `TrackProgress` is the centerline station in METRES — the
	 * server writes `distance_from_start_m` straight into it (physics.rs) —
	 * not a fraction of the lap. Lap numbers are 1-based, and stay 0 until
	 * the car has crossed the start line and cleared 10% of the lap.
	 *
	 * The grid sits BEHIND the start/finish line, so a car that has not
	 * started yet carries a station just short of the full track length.
	 * Counted forward that would rank the whole field nearly a lap ahead,
	 * so on lap 0 a station in the back half of the track reads as distance
	 * still to cover. Every transition the server emits then stays
	 * continuous: grid to line (negative to zero), lap 0 to 1 at the 10%
	 * mark (identical value either side), and each later wrap (the station
	 * resets to zero exactly as the lap counter steps).
	 */
	inline float RaceDistanceM(int32 CurrentLap, float StationM, float TrackLengthM)
	{
		const float Station = FMath::Max(StationM, 0.0f);
		if (CurrentLap <= 0 && Station > TrackLengthM * 0.5f)
		{
			return Station - TrackLengthM;
		}
		return FMath::Max(0, CurrentLap - 1) * TrackLengthM + Station;
	}
}

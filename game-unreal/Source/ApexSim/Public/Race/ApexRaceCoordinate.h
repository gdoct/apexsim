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

	/** Unreal world position (cm, +Y right) -> server position (metres, +Y left). */
	inline FVector UnrealToServerPosition(const FVector& UnrealCm)
	{
		return FVector(
			UnrealCm.X / MetresToCentimetres,
			-UnrealCm.Y / MetresToCentimetres,
			UnrealCm.Z / MetresToCentimetres);
	}

	/**
	 * A view direction given the server's way -> Unreal rotator, no roll.
	 *
	 * Degrees, yaw counter-clockwise from +X as the server measures it, and
	 * pitch positive UP. That is a camera's convention rather than the car
	 * body's, so it is kept apart from `ServerToUnrealRotation`: the yaw
	 * negates with the Y flip and the pitch is Unreal's own.
	 */
	inline FRotator ServerViewToUnrealRotation(double YawDeg, double PitchDeg)
	{
		return FRotator(PitchDeg, -YawDeg, 0.0);
	}

	/** Inverse of `ServerViewToUnrealRotation`, for logging a pose in the server frame. */
	inline void UnrealRotationToServerView(const FRotator& Rotation, double& OutYawDeg, double& OutPitchDeg)
	{
		OutYawDeg = FRotator::NormalizeAxis(-Rotation.Yaw);
		OutPitchDeg = FRotator::NormalizeAxis(Rotation.Pitch);
	}

	/**
	 * Unreal rotator that looks from one server-frame point (metres) at
	 * another, with no roll. Coincident points give a zero rotator.
	 */
	inline FRotator ServerLookAtUnrealRotation(const FVector& FromMetres, const FVector& TargetMetres)
	{
		const FVector Direction = ServerToUnrealPosition(TargetMetres) - ServerToUnrealPosition(FromMetres);
		if (Direction.IsNearlyZero())
		{
			return FRotator::ZeroRotator;
		}
		FRotator Rotation = Direction.Rotation();
		Rotation.Roll = 0.0;
		return Rotation;
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
	 * not a fraction of the lap. Lap numbers are 1-based and stay 0 until the
	 * car starts its first lap: a car on the grid behind the line starts it
	 * as it crosses the line, pole (on the line) with the green light, and a
	 * car placed past the line on clearing 10% of the lap.
	 *
	 * The grid sits BEHIND the start/finish line, so a car that has not
	 * started yet carries a station just short of the full track length.
	 * Counted forward that would rank the whole field nearly a lap ahead,
	 * so on lap 0 a station in the back half of the track reads as distance
	 * still to cover. Every transition the server emits then stays
	 * continuous: grid to line (negative to zero, the lap stepping 0 to 1),
	 * lap 0 to 1 at the 10% mark (identical value either side), and each
	 * later wrap (the station resets to zero exactly as the lap counter steps).
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

	/**
	 * Race order: whether car A is ahead of car B. A car that has taken the
	 * flag (`FinishPosition` > 0) is ahead of every car still racing, and
	 * finishers are in their classified order. That matters once the winner
	 * is in: a finished car drives on, and its lap count or station says
	 * nothing about the result. Everyone else is ordered by race distance.
	 */
	inline bool RanksAhead(int32 FinishA, float DistanceA, int32 FinishB, float DistanceB)
	{
		const bool bFinishedA = FinishA > 0;
		const bool bFinishedB = FinishB > 0;
		if (bFinishedA != bFinishedB)
		{
			return bFinishedA;
		}
		if (bFinishedA)
		{
			return FinishA < FinishB;
		}
		return DistanceA > DistanceB;
	}

	/** The lap to show a driver: never 0 on the grid, never past the race distance. */
	inline int32 DisplayLap(int32 CurrentLap, int32 LapLimit)
	{
		const int32 Lap = FMath::Max(1, CurrentLap);
		return LapLimit > 0 ? FMath::Min(Lap, LapLimit) : Lap;
	}
}

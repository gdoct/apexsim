#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Race/ApexTvDirector.h"

/**
 * The camera behind the session lobby: the circuit with nobody on it, filmed
 * the way coverage opens before a race. There are no cars to follow, so the
 * shots are about the place: a helicopter circling a corner, a drone gliding
 * down the road ahead of where a car would be, and a long lens trackside on
 * the outside of a bend panning through it. Each holds for a few seconds and
 * cuts to another part of the lap.
 *
 * Pure logic, like ApexTvDirector: Unreal world space (cm, +Z up), with the
 * ground and line-of-sight queries passed in. `ApexSim.LobbyCam.*` drives it
 * on a synthetic circuit.
 */
namespace ApexLobbyCam
{
	enum class EShot : uint8
	{
		None,
		/** High and wide, circling a corner. */
		Orbit,
		/** Over the outside of the road, flying along it and looking down the lap. */
		Glide,
		/** Low on the outside of a bend, panning from its entry to its exit. */
		Pan,
		Count
	};

	APEXSIM_API const TCHAR* ShotName(EShot Shot);

	class APEXSIM_API FFlyover
	{
	public:
		/** Forget the path and the shot; the seed picks the order of the shots. */
		void Reset(int32 Seed);

		/** The road's centerline, a closed loop, with its real heights (the built track's). */
		void SetPath(TConstArrayView<FVector> Points);
		bool HasPath() const { return Path.IsValid(); }
		const ApexTv::FPath& GetPath() const { return Path; }

		/** Height of the road at a station, cm: the path's own, between its points. */
		double RoadZ(double StationCm) const;

		/** Place the camera for this frame. False without a path. */
		bool Tick(float DeltaSeconds, const ApexTv::FWorldQueries& World, ApexTv::FPose& OutPose);

		/** Cut to another shot on the next Tick. */
		void RequestCut() { bCutRequested = true; }

		EShot GetShot() const { return Shot.Kind; }
		float GetShotAge() const { return ShotAge; }
		int32 GetCutCount() const { return CutCount; }
		/** The station the shot is about: the corner circled, the bend panned, where the glide began. */
		double GetShotStationCm() const { return Shot.StationCm; }

	private:
		struct FShot
		{
			EShot Kind = EShot::None;
			double StationCm = 0.0;
			/** +1 puts the camera on the road's right (Unreal +Y of the tangent), -1 on its left. */
			double Side = 1.0;
			double HeightCm = 0.0;
			/** Off the road (Glide, Pan) or from the corner (Orbit). */
			double DistanceCm = 0.0;
			/** Glide: along the road, cm/s. Orbit: degrees/s. Pan: half the sweep, cm. */
			double Rate = 0.0;
			/** Orbit: the bearing it starts at, degrees. Glide: how far ahead it looks, cm. */
			double Start = 0.0;
			float FovDeg = 50.0f;
			float Seconds = 9.0f;
		};

		/** Pick, place and check the next shot. */
		void Cut(const ApexTv::FWorldQueries& World);
		/** A shot of this kind round a corner chosen at random, not the part of the lap just shown. */
		FShot Plan(EShot Kind);
		/** Where the camera is and what it looks at, `Age` seconds into a shot. */
		void Place(const FShot& InShot, float Age, const ApexTv::FWorldQueries& World, FVector& OutEye, FVector& OutLook) const;
		double GroundBelow(const FVector& At, double Fallback, const ApexTv::FWorldQueries& World) const;
		FVector RoadPoint(double StationCm, FVector& OutTangent) const;

		ApexTv::FPath Path;
		/** Cumulative plan distance and height of each path point, for RoadZ. */
		TArray<double> HeightStations;
		TArray<double> Heights;

		FRandomStream Rng;
		FShot Shot;
		float ShotAge = 0.0f;
		int32 CutCount = 0;
		bool bCutRequested = false;
		FRotator Rotation = FRotator::ZeroRotator;
		/** The camera's height, eased over steps in the ground. */
		double EyeZ = 0.0;
	};
}

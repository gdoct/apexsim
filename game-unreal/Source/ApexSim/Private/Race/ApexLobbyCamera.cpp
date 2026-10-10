#include "Race/ApexLobbyCamera.h"

#include "Algo/BinarySearch.h"

namespace ApexLobbyCam
{
	namespace LobbyCamDetail
	{
		constexpr double Metre = 100.0;
		/** How far round the lap the next shot must be from the last, as a share of the lap. */
		constexpr double MinShotSpacing = 0.15;
		/** Clearance kept over whatever is below the camera, cm. */
		constexpr double MinClearanceCm = 200.0;
		/** Ground traces start this far over the road. */
		constexpr double TraceHeadroomCm = 50000.0;

		/** Plan distance between two stations on a loop, the short way round. */
		double LoopGap(double A, double B, double Length)
		{
			const double Gap = FMath::Abs(FMath::Fmod(A - B, Length));
			return FMath::Min(Gap, Length - Gap);
		}
	}

	const TCHAR* ShotName(EShot Shot)
	{
		switch (Shot)
		{
		case EShot::Orbit: return TEXT("orbit");
		case EShot::Glide: return TEXT("glide");
		case EShot::Pan:   return TEXT("pan");
		default:           return TEXT("none");
		}
	}

	void FFlyover::Reset(int32 Seed)
	{
		Path = ApexTv::FPath();
		HeightStations.Reset();
		Heights.Reset();
		Rng.Initialize(Seed);
		Shot = FShot();
		ShotAge = 0.0f;
		CutCount = 0;
		bCutRequested = false;
		Rotation = FRotator::ZeroRotator;
		EyeZ = 0.0;
	}

	void FFlyover::SetPath(TConstArrayView<FVector> Points)
	{
		// The same points FPath keeps (no repeats, no closing point), so the
		// heights line up with its stations.
		TArray<FVector> Kept;
		Kept.Reserve(Points.Num());
		for (const FVector& Point : Points)
		{
			if (Kept.Num() == 0 || FVector::DistSquared2D(Kept.Last(), Point) > 1.0)
			{
				Kept.Add(Point);
			}
		}
		if (Kept.Num() > 1 && FVector::DistSquared2D(Kept[0], Kept.Last()) < 1.0)
		{
			Kept.Pop();
		}
		HeightStations.Reset(Kept.Num());
		Heights.Reset(Kept.Num());
		double Station = 0.0;
		for (int32 Index = 0; Index < Kept.Num(); ++Index)
		{
			if (Index > 0)
			{
				Station += FVector::Dist2D(Kept[Index - 1], Kept[Index]);
			}
			HeightStations.Add(Station);
			Heights.Add(Kept[Index].Z);
		}
		Path.Build(MoveTemp(Kept));
		Shot = FShot();
		ShotAge = 0.0f;
	}

	double FFlyover::RoadZ(double StationCm) const
	{
		if (!Path.IsValid() || Heights.Num() != Path.Points.Num())
		{
			return 0.0;
		}
		const double Station = FMath::Fmod(FMath::Fmod(StationCm, Path.LengthCm) + Path.LengthCm, Path.LengthCm);
		const int32 Upper = Algo::UpperBound(HeightStations, Station);
		const int32 Low = FMath::Clamp(Upper - 1, 0, Heights.Num() - 1);
		const int32 High = (Low + 1) % Heights.Num();
		const double Next = High == 0 ? Path.LengthCm : HeightStations[High];
		const double Span = Next - HeightStations[Low];
		const double T = Span > 0.0 ? FMath::Clamp((Station - HeightStations[Low]) / Span, 0.0, 1.0) : 0.0;
		return FMath::Lerp(Heights[Low], Heights[High], T);
	}

	FVector FFlyover::RoadPoint(double StationCm, FVector& OutTangent) const
	{
		FVector Point;
		Path.Sample(StationCm, Point, OutTangent);
		Point.Z = RoadZ(StationCm);
		return Point;
	}

	double FFlyover::GroundBelow(const FVector& At, double Fallback, const ApexTv::FWorldQueries& World) const
	{
		double Z = 0.0;
		if (World.GroundZ && World.GroundZ(FVector(At.X, At.Y, At.Z + LobbyCamDetail::TraceHeadroomCm), Z))
		{
			return Z;
		}
		return Fallback;
	}

	FFlyover::FShot FFlyover::Plan(EShot Kind)
	{
		using namespace LobbyCamDetail;
		auto Range = [this](double Low, double High) { return Low + (High - Low) * Rng.GetFraction(); };

		// The tightest of a few corners round the lap, away from the last shot:
		// corners are what a circuit is remembered by.
		double Best = Rng.GetFraction() * Path.LengthCm;
		double BestTurn = -1.0;
		for (int32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			const double Candidate = Rng.GetFraction() * Path.LengthCm;
			if (Shot.Kind != EShot::None && LoopGap(Candidate, Shot.StationCm, Path.LengthCm) < MinShotSpacing * Path.LengthCm)
			{
				continue;
			}
			const double Turn = FMath::Abs(Path.TurnDeg(Candidate, 50.0 * Metre));
			if (Turn > BestTurn)
			{
				Best = Candidate;
				BestTurn = Turn;
			}
		}
		const double TurnDeg = Path.TurnDeg(Best, 50.0 * Metre);
		// The outside of the bend, where the cameras stand at a real circuit.
		const double OutsideSide = FMath::Abs(TurnDeg) < 10.0 ? (Rng.GetFraction() < 0.5 ? -1.0 : 1.0)
			: TurnDeg > 0.0                                   ? -1.0
															  : 1.0;

		FShot Next;
		Next.Kind = Kind;
		Next.StationCm = Best;
		Next.Side = OutsideSide;
		switch (Kind)
		{
		case EShot::Orbit:
		{
			Next.DistanceCm = Range(140.0, 220.0) * Metre;
			Next.HeightCm = Range(65.0, 105.0) * Metre;
			Next.Rate = (Rng.GetFraction() < 0.5 ? -1.0 : 1.0) * Range(2.5, 4.0);
			FVector Tangent;
			RoadPoint(Best, Tangent);
			const FVector Outside = FVector(-Tangent.Y, Tangent.X, 0.0) * OutsideSide;
			Next.Start = FMath::RadiansToDegrees(FMath::Atan2(Outside.Y, Outside.X)) + Range(-60.0, 60.0);
			Next.FovDeg = static_cast<float>(Range(45.0, 55.0));
			Next.Seconds = static_cast<float>(Range(10.0, 13.0));
			break;
		}
		case EShot::Glide:
			Next.Rate = Range(14.0, 20.0) * Metre;
			Next.Seconds = static_cast<float>(Range(8.0, 11.0));
			// Into the corner: half the flight before it.
			Next.StationCm = Best - Next.Rate * Next.Seconds * 0.5;
			Next.DistanceCm = Range(15.0, 30.0) * Metre;
			Next.HeightCm = Range(20.0, 35.0) * Metre;
			Next.Start = Range(100.0, 150.0) * Metre;
			Next.FovDeg = static_cast<float>(Range(60.0, 70.0));
			break;
		case EShot::Pan:
		default:
			Next.DistanceCm = Range(20.0, 32.0) * Metre;
			Next.HeightCm = Range(2.5, 4.5) * Metre;
			Next.Rate = Range(50.0, 80.0) * Metre;
			Next.FovDeg = static_cast<float>(Range(32.0, 42.0));
			Next.Seconds = static_cast<float>(Range(7.0, 9.0));
			break;
		}
		return Next;
	}

	void FFlyover::Place(const FShot& InShot, float Age, const ApexTv::FWorldQueries& World, FVector& OutEye, FVector& OutLook) const
	{
		using namespace LobbyCamDetail;
		FVector Tangent;
		switch (InShot.Kind)
		{
		case EShot::Orbit:
		{
			const FVector Corner = RoadPoint(InShot.StationCm, Tangent);
			const double Bearing = FMath::DegreesToRadians(InShot.Start + InShot.Rate * Age);
			OutEye = Corner + FVector(FMath::Cos(Bearing), FMath::Sin(Bearing), 0.0) * InShot.DistanceCm;
			OutEye.Z = FMath::Max(Corner.Z + InShot.HeightCm, GroundBelow(OutEye, Corner.Z, World) + 0.6 * InShot.HeightCm);
			OutLook = Corner;
			break;
		}
		case EShot::Glide:
		{
			const double Station = InShot.StationCm + InShot.Rate * Age;
			const FVector Road = RoadPoint(Station, Tangent);
			OutEye = Road + FVector(-Tangent.Y, Tangent.X, 0.0) * InShot.Side * InShot.DistanceCm;
			OutEye.Z = FMath::Max(Road.Z + InShot.HeightCm, GroundBelow(OutEye, Road.Z, World) + 0.6 * InShot.HeightCm);
			FVector Ahead;
			OutLook = RoadPoint(Station + InShot.Start, Ahead);
			break;
		}
		case EShot::Pan:
		default:
		{
			const FVector Bend = RoadPoint(InShot.StationCm, Tangent);
			OutEye = Bend + FVector(-Tangent.Y, Tangent.X, 0.0) * InShot.Side * InShot.DistanceCm;
			OutEye.Z = FMath::Max(GroundBelow(OutEye, Bend.Z, World) + InShot.HeightCm, Bend.Z + 1.5 * Metre);
			// Entry to exit, easing in and out like an operator's head.
			const double Progress = static_cast<double>(Age) / FMath::Max(static_cast<double>(InShot.Seconds), 0.1);
			const double Sweep = FMath::Lerp(-1.0, 1.0, FMath::SmoothStep(0.0, 1.0, Progress));
			FVector Along;
			OutLook = RoadPoint(InShot.StationCm + Sweep * InShot.Rate, Along) + FVector(0.0, 0.0, 1.0 * Metre);
			break;
		}
		}
	}

	void FFlyover::Cut(const ApexTv::FWorldQueries& World)
	{
		// An establishing orbit first, then never the same kind twice running.
		TArray<EShot, TInlineAllocator<3>> Kinds;
		if (Shot.Kind == EShot::None)
		{
			Kinds.Add(EShot::Orbit);
		}
		else
		{
			for (EShot Kind : { EShot::Orbit, EShot::Glide, EShot::Pan })
			{
				if (Kind != Shot.Kind)
				{
					Kinds.Add(Kind);
				}
			}
			if (Rng.GetFraction() < 0.5)
			{
				Kinds.Swap(0, 1);
			}
		}

		// A shot whose subject is hidden behind a stand or a hill is no shot;
		// try other corners, then the other kinds.
		FShot Chosen;
		bool bFound = false;
		for (EShot Kind : Kinds)
		{
			for (int32 Attempt = 0; Attempt < 6 && !bFound; ++Attempt)
			{
				const FShot Candidate = Plan(Kind);
				Chosen = Candidate;
				bFound = true;
				if (!World.IsClear)
				{
					break;
				}
				for (const float Share : { 0.0f, 0.5f, 1.0f })
				{
					FVector Eye;
					FVector Look;
					Place(Candidate, Candidate.Seconds * Share, World, Eye, Look);
					if (!World.IsClear(Eye, Look + FVector(0.0, 0.0, 100.0)))
					{
						bFound = false;
						break;
					}
				}
			}
			if (bFound)
			{
				break;
			}
		}
		if (!bFound)
		{
			// Nothing is clear from anywhere: the high orbit sees over most of it.
			Chosen = Plan(EShot::Orbit);
		}
		Shot = Chosen;
		ShotAge = 0.0f;
		bCutRequested = false;
		++CutCount;
	}

	bool FFlyover::Tick(float DeltaSeconds, const ApexTv::FWorldQueries& World, ApexTv::FPose& OutPose)
	{
		if (!Path.IsValid())
		{
			return false;
		}
		DeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		ShotAge += DeltaSeconds;
		const bool bCut = Shot.Kind == EShot::None || bCutRequested || ShotAge >= Shot.Seconds;
		if (bCut)
		{
			Cut(World);
		}

		FVector Eye;
		FVector Look;
		Place(Shot, ShotAge, World, Eye, Look);
		// The ground under a moving camera steps (a barrier, a bank); the
		// camera eases over it rather than hopping, but never into it.
		const double Floor = GroundBelow(Eye, Eye.Z, World) + LobbyCamDetail::MinClearanceCm;
		EyeZ = bCut ? Eye.Z : FMath::FInterpTo(EyeZ, Eye.Z, DeltaSeconds, 2.0);
		Eye.Z = FMath::Max(EyeZ, Floor);

		const FRotator Wanted = (Look - Eye).Rotation();
		Rotation = bCut ? Wanted : FMath::RInterpTo(Rotation, Wanted, DeltaSeconds, 4.0f);

		OutPose.Location = Eye;
		OutPose.Rotation = Rotation;
		OutPose.FovDeg = Shot.FovDeg;
		OutPose.FocusDistanceCm = static_cast<float>(FVector::Dist(Eye, Look));
		OutPose.Aperture = 0.0f;
		return true;
	}
}

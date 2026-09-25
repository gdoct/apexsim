#include "Race/ApexTvDirector.h"

#include "Race/ApexRaceCoordinate.h"

namespace ApexTv
{
	namespace
	{
		constexpr double Metre = 100.0;

		/** Unreal's right of a heading in plan: +Y for a car facing +X. */
		FVector RightOf(const FVector& Tangent)
		{
			return FVector(-Tangent.Y, Tangent.X, 0.0).GetSafeNormal();
		}

		FVector Heading(double YawDeg)
		{
			return FRotator(0.0, YawDeg, 0.0).Vector();
		}

		double SmoothStep(double X)
		{
			X = FMath::Clamp(X, 0.0, 1.0);
			return X * X * (3.0 - 2.0 * X);
		}

		/** Ease `Current` toward `Target` degrees along the short way round. */
		double EaseYaw(double Current, double Target, float DeltaSeconds, double Rate)
		{
			const double Delta = FRotator::NormalizeAxis(Target - Current);
			const double Alpha = 1.0 - FMath::Exp(-Rate * DeltaSeconds);
			return FRotator::NormalizeAxis(Current + Delta * Alpha);
		}

		bool IsOnboard(EShot Shot)
		{
			return Shot == EShot::Onboard || Shot == EShot::Nose;
		}

		/** Race order, leader first. */
		TArray<const FCar*> RaceOrder(TConstArrayView<FCar> Cars)
		{
			TArray<const FCar*> Order;
			for (const FCar& Car : Cars)
			{
				Order.Add(&Car);
			}
			Order.StableSort([](const FCar& A, const FCar& B) { return A.RaceDistanceM > B.RaceDistanceM; });
			return Order;
		}
	}

	const TCHAR* ShotName(EShot Shot)
	{
		switch (Shot)
		{
		case EShot::Grid:       return TEXT("grid");
		case EShot::Trackside:  return TEXT("trackside");
		case EShot::Helicopter: return TEXT("helicopter");
		case EShot::Tracking:   return TEXT("tracking");
		case EShot::Chase:      return TEXT("chase");
		case EShot::Onboard:    return TEXT("onboard");
		case EShot::Nose:       return TEXT("nose");
		case EShot::Reverse:    return TEXT("reverse");
		default:                return TEXT("none");
		}
	}

	FVector FCar::Centre() const
	{
		return Location + Rotation.RotateVector(Body.GetCenter());
	}

	// --- Path --------------------------------------------------------------------

	void FPath::BuildFromServerCenterline(const TArray<FVector2D>& CenterlineMetres)
	{
		TArray<FVector> InPoints;
		InPoints.Reserve(CenterlineMetres.Num());
		for (const FVector2D& Point : CenterlineMetres)
		{
			InPoints.Add(ApexRace::ServerToUnrealPosition(FVector(Point.X, Point.Y, 0.0)));
		}
		Build(MoveTemp(InPoints));
	}

	void FPath::Build(TArray<FVector> InPoints)
	{
		Points.Reset();
		Stations.Reset();
		LengthCm = 0.0;
		for (FVector& Point : InPoints)
		{
			Point.Z = 0.0;
			// A repeated point has no tangent.
			if (Points.Num() == 0 || FVector::DistSquared2D(Points.Last(), Point) > 1.0)
			{
				Points.Add(Point);
			}
		}
		// A loop written with its first point repeated at the end.
		if (Points.Num() > 1 && FVector::DistSquared2D(Points[0], Points.Last()) < 1.0)
		{
			Points.Pop();
		}
		if (Points.Num() < 3)
		{
			Points.Reset();
			return;
		}
		Stations.SetNum(Points.Num());
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Stations[Index] = LengthCm;
			LengthCm += FVector::Dist2D(Points[Index], Points[(Index + 1) % Points.Num()]);
		}
	}

	double FPath::StationAt(const FVector& Location) const
	{
		if (!IsValid())
		{
			return 0.0;
		}
		double BestDistSq = TNumericLimits<double>::Max();
		double BestStation = 0.0;
		const FVector Flat(Location.X, Location.Y, 0.0);
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			const FVector& A = Points[Index];
			const FVector& B = Points[(Index + 1) % Points.Num()];
			const FVector Segment = B - A;
			const double LengthSq = Segment.SizeSquared2D();
			const double T = LengthSq > 0.0 ? FMath::Clamp(FVector::DotProduct(Flat - A, Segment) / LengthSq, 0.0, 1.0) : 0.0;
			const double DistSq = FVector::DistSquared2D(A + Segment * T, Flat);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestStation = Stations[Index] + T * FMath::Sqrt(LengthSq);
			}
		}
		return BestStation;
	}

	void FPath::Sample(double StationCm, FVector& OutPoint, FVector& OutTangent) const
	{
		if (!IsValid())
		{
			OutPoint = FVector::ZeroVector;
			OutTangent = FVector::ForwardVector;
			return;
		}
		const double Station = FMath::Fmod(FMath::Fmod(StationCm, LengthCm) + LengthCm, LengthCm);
		// Last point whose station is at or before the one asked for.
		int32 Low = 0;
		int32 High = Stations.Num() - 1;
		while (Low < High)
		{
			const int32 Mid = (Low + High + 1) / 2;
			if (Stations[Mid] <= Station)
			{
				Low = Mid;
			}
			else
			{
				High = Mid - 1;
			}
		}
		const FVector& A = Points[Low];
		const FVector& B = Points[(Low + 1) % Points.Num()];
		const double SegmentLength = FVector::Dist2D(A, B);
		const double T = SegmentLength > 0.0 ? (Station - Stations[Low]) / SegmentLength : 0.0;
		OutPoint = FMath::Lerp(A, B, FMath::Clamp(T, 0.0, 1.0));
		OutTangent = (B - A).GetSafeNormal2D();
		if (OutTangent.IsNearlyZero())
		{
			OutTangent = FVector::ForwardVector;
		}
	}

	double FPath::TurnDeg(double StationCm, double HalfSpanCm) const
	{
		FVector Point;
		FVector Before;
		FVector After;
		Sample(StationCm - HalfSpanCm, Point, Before);
		Sample(StationCm + HalfSpanCm, Point, After);
		return FRotator::NormalizeAxis(After.Rotation().Yaw - Before.Rotation().Yaw);
	}

	// --- Free functions ----------------------------------------------------------

	float FramingFovDeg(float SubjectSizeCm, float DistanceCm, float FrameFraction, float MinDeg, float MaxDeg)
	{
		const float Width = FMath::Max(SubjectSizeCm, 1.0f) / FMath::Max(FrameFraction, 0.01f);
		const float Fov = 2.0f * FMath::RadiansToDegrees(FMath::Atan2(Width * 0.5f, FMath::Max(DistanceCm, 1.0f)));
		return FMath::Clamp(Fov, MinDeg, MaxDeg);
	}

	int32 PickTarget(TConstArrayView<FCar> Cars, int32 CurrentCarIndex, int32 ShotsOnCurrent,
		const TSet<int32>& CarsInTrouble, FRandomStream& Rng)
	{
		if (Cars.Num() == 0)
		{
			return INDEX_NONE;
		}
		const TArray<const FCar*> Order = RaceOrder(Cars);

		int32 Best = INDEX_NONE;
		double BestScore = -TNumericLimits<double>::Max();
		for (int32 Position = 0; Position < Order.Num(); ++Position)
		{
			const FCar& Car = *Order[Position];
			double Gap = TNumericLimits<double>::Max();
			if (Position > 0)
			{
				Gap = FMath::Min(Gap, static_cast<double>(Order[Position - 1]->RaceDistanceM - Car.RaceDistanceM));
			}
			if (Position + 1 < Order.Num())
			{
				Gap = FMath::Min(Gap, static_cast<double>(Car.RaceDistanceM - Order[Position + 1]->RaceDistanceM));
			}

			// A car within a few lengths of another is a race; one 35 m clear is not.
			double Score = FMath::Clamp(1.0 - Gap / 35.0, 0.0, 1.0);
			Score += Position == 0 ? 0.35 : Position <= 2 ? 0.15 : 0.0;
			if (CarsInTrouble.Contains(Car.CarIndex) && !(Car.CarIndex == CurrentCarIndex && ShotsOnCurrent >= 2))
			{
				// The news, whoever else is on screen: for a couple of shots.
				Score += 2.5;
			}
			if (Car.CarIndex == CurrentCarIndex)
			{
				// Stay with a car for a few shots, then look elsewhere.
				Score += ShotsOnCurrent < 3 ? 0.5 : ShotsOnCurrent >= 4 ? -0.6 : 0.0;
			}
			Score += Rng.FRandRange(0.0f, 0.3f);

			if (Score > BestScore)
			{
				BestScore = Score;
				Best = static_cast<int32>(Order[Position] - Cars.GetData());
			}
		}
		return Best;
	}

	// --- Director ----------------------------------------------------------------

	FDirector::FDirector()
	{
		Reset(0x7E1E);
	}

	void FDirector::Reset(int32 Seed)
	{
		Rng.Initialize(Seed);
		Shot = EShot::None;
		ForcedShot = EShot::None;
		Setup = FShotSetup();
		TargetCarIndex = INDEX_NONE;
		LockedCarIndex = INDEX_NONE;
		ShotsOnTarget = 0;
		ShotAge = 0.0f;
		ShotLength = 0.0f;
		HiddenFor = 0.0f;
		CutCount = 0;
		PendingCutReason = TEXT("start");
		LastCutReason = TEXT("start");
		LastShotHeld = 0.0f;
		bCutRequested = false;
		bWasCountdown = false;
		History.Reset();
		bFresh = true;
		bApproached = false;
		SlowFor.Reset();
		InTrouble.Reset();
		bTroubleUnseen = false;
		TroubleCooldown = 0.0f;
		RacingFor = 0.0f;
	}

	void FDirector::SetPath(const TArray<FVector2D>& CenterlineMetres)
	{
		Path.BuildFromServerCenterline(CenterlineMetres);
	}

	const FCar* FDirector::FindCar(TConstArrayView<FCar> Cars, int32 CarIndex) const
	{
		for (const FCar& Car : Cars)
		{
			if (Car.CarIndex == CarIndex)
			{
				return &Car;
			}
		}
		return nullptr;
	}

	double FDirector::GroundBelow(const FVector& At, double Fallback, const FWorldQueries& World) const
	{
		double Z = 0.0;
		if (World.GroundZ && World.GroundZ(At, Z))
		{
			return Z;
		}
		return Fallback;
	}

	bool FDirector::Tick(TConstArrayView<FCar> Cars, bool bCountdown, float DeltaSeconds, float TimeSeconds,
		const FWorldQueries& World, FPose& OutPose)
	{
		if (Cars.Num() == 0)
		{
			return false;
		}
		DeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);

		TrackTrouble(Cars, bCountdown, DeltaSeconds);

		// Lights out is always a cut: off the grid, onto the start.
		if (bCountdown != bWasCountdown)
		{
			bWasCountdown = bCountdown;
			RequestCutFor(TEXT("lights"));
		}

		const FCar* Target = FindCar(Cars, TargetCarIndex);
		if (Target && !bFresh && FVector::Dist(Target->Location, PrevTargetLocation) > 30.0 * Metre)
		{
			// The car was put somewhere else (a reset, a new race): no shot survives that.
			RequestCutFor(TEXT("car moved"));
		}
		if (!Target || bCutRequested || Shot == EShot::None)
		{
			Cut(Cars, bCountdown, World);
			Target = FindCar(Cars, TargetCarIndex);
			if (!Target)
			{
				return false;
			}
		}
		else
		{
			ShotAge += DeltaSeconds;
		}

		Evaluate(*Target, Cars, DeltaSeconds, TimeSeconds, World, OutPose);
		PrevTargetLocation = Target->Location;

		// A camera that cannot see its car is not a shot. The onboard ones are
		// on the car; everything else has to look past barriers and trees.
		if (!IsOnboard(Shot) && World.IsClear && !World.IsClear(OutPose.Location, Target->Centre() + FVector(0.0, 0.0, 30.0)))
		{
			HiddenFor += DeltaSeconds;
		}
		else
		{
			HiddenFor = 0.0f;
		}

		// A pole or a trunk crossing the lens is part of the picture; a wall
		// that stays there is not.
		if (HiddenFor > BlockedCutSeconds)
		{
			RequestCutFor(TEXT("blocked"));
		}
		else if (ShotAge >= ShotLength)
		{
			RequestCutFor(TEXT("held its time"));
		}
		else if (IsShotSpent(*Target, OutPose))
		{
			RequestCutFor(TEXT("done"));
		}
		return true;
	}

	void FDirector::TrackTrouble(TConstArrayView<FCar> Cars, bool bCountdown, float DeltaSeconds)
	{
		if (bCountdown)
		{
			RacingFor = 0.0f;
			SlowFor.Reset();
			InTrouble.Reset();
			bTroubleUnseen = false;
			return;
		}
		RacingFor += DeltaSeconds;
		TroubleCooldown = FMath::Max(0.0f, TroubleCooldown - DeltaSeconds);

		// Go to it as soon as the shot on screen has had a moment: a cut in
		// its first second would look like a mistake. One incident at a time,
		// though, or a scrappy lap is nothing but cuts.
		if (bTroubleUnseen && ShotAge > 1.0f && ForcedShot == EShot::None)
		{
			bTroubleUnseen = false;
			if (TroubleCooldown <= 0.0f)
			{
				TroubleCooldown = 12.0f;
				RequestCutFor(TEXT("car in trouble"));
			}
		}

		for (const FCar& Car : Cars)
		{
			float& Slow = SlowFor.FindOrAdd(Car.CarIndex);
			// Off the road, or all but stopped once the start has cleared: a
			// spin, the gravel, a wall. Merely slow is not news; slow corners
			// are.
			const bool bStopped = RacingFor > 12.0f && Car.SpeedCmPerS() < 5.0f * Metre;
			const bool bOff = RacingFor > 5.0f && Car.bOffTrack;
			Slow = bStopped || bOff ? Slow + DeltaSeconds : FMath::Max(0.0f, Slow - 2.0f * DeltaSeconds);

			// News for twenty seconds; a car stopped for good is not a story.
			const bool bNews = Slow > 1.0f && Slow < 20.0f;
			if (bNews && !InTrouble.Contains(Car.CarIndex))
			{
				InTrouble.Add(Car.CarIndex);
				bTroubleUnseen |= Car.CarIndex != TargetCarIndex;
			}
			else if (!bNews)
			{
				InTrouble.Remove(Car.CarIndex);
			}
		}
	}

	void FDirector::RequestCutFor(const TCHAR* Reason)
	{
		if (!bCutRequested)
		{
			PendingCutReason = Reason;
		}
		bCutRequested = true;
	}

	void FDirector::Cut(TConstArrayView<FCar> Cars, bool bCountdown, const FWorldQueries& World)
	{
		bCutRequested = false;
		LastCutReason = PendingCutReason;
		PendingCutReason = TEXT("asked");
		LastShotHeld = ShotAge;

		int32 NewTarget = INDEX_NONE;
		if (LockedCarIndex != INDEX_NONE && FindCar(Cars, LockedCarIndex))
		{
			NewTarget = LockedCarIndex;
		}
		else if (bCountdown)
		{
			// The grid is about the pole-sitter.
			NewTarget = RaceOrder(Cars)[0]->CarIndex;
		}
		else if (History.Num() > 0 && History[0] == EShot::Grid)
		{
			// Lights out: the leader, with the field behind.
			NewTarget = RaceOrder(Cars)[0]->CarIndex;
		}
		else
		{
			const int32 Picked = PickTarget(Cars, TargetCarIndex, ShotsOnTarget, InTrouble, Rng);
			NewTarget = Picked != INDEX_NONE ? Cars[Picked].CarIndex : INDEX_NONE;
		}
		if (NewTarget != TargetCarIndex)
		{
			ShotsOnTarget = 0;
		}
		TargetCarIndex = NewTarget;
		const FCar* Target = FindCar(Cars, TargetCarIndex);
		if (!Target)
		{
			return;
		}

		EShot Next = ForcedShot != EShot::None ? ForcedShot : ChooseShot(*Target, Cars, bCountdown);
		if (!SetUpShot(Next, *Target, Cars, World))
		{
			// Trackside is the one that can fail (no path, nowhere with a view);
			// a chase always has somewhere to stand.
			Next = bCountdown ? EShot::Grid : EShot::Helicopter;
			if (!SetUpShot(Next, *Target, Cars, World))
			{
				Next = EShot::Chase;
				SetUpShot(Next, *Target, Cars, World);
			}
		}

		Shot = Next;
		History.Insert(Shot, 0);
		if (History.Num() > 4)
		{
			History.Pop();
		}
		ShotAge = 0.0f;
		HiddenFor = 0.0f;
		bFresh = true;
		bApproached = false;
		++CutCount;
		++ShotsOnTarget;
	}

	EShot FDirector::ChooseShot(const FCar& Target, TConstArrayView<FCar> Cars, bool bCountdown)
	{
		if (bCountdown)
		{
			return EShot::Grid;
		}
		if (History.Num() > 0 && History[0] == EShot::Grid)
		{
			return EShot::Helicopter;
		}

		double Weights[static_cast<int32>(EShot::Count)] = {};
		auto Weight = [&Weights](EShot S) -> double& { return Weights[static_cast<int32>(S)]; };
		Weight(EShot::Trackside) = Path.IsValid() ? 3.2 : 0.0;
		Weight(EShot::Helicopter) = 1.4;
		Weight(EShot::Tracking) = 1.2;
		Weight(EShot::Chase) = 1.0;
		Weight(EShot::Onboard) = 1.0;
		Weight(EShot::Nose) = 0.5;
		Weight(EShot::Reverse) = 0.8;

		// Somebody right behind: the camera on the car ahead is made for that.
		for (const FCar& Other : Cars)
		{
			const float Behind = Target.RaceDistanceM - Other.RaceDistanceM;
			if (Other.CarIndex != Target.CarIndex && Behind > 0.0f && Behind < 30.0f)
			{
				Weight(EShot::Reverse) += 1.2;
				break;
			}
		}

		// A car barely moving is dull from on board and never reaches a trackside camera.
		if (Target.SpeedCmPerS() < 8.0 * Metre)
		{
			Weight(EShot::Onboard) *= 0.2;
			Weight(EShot::Nose) *= 0.2;
			Weight(EShot::Tracking) *= 0.2;
			Weight(EShot::Reverse) *= 0.2;
			Weight(EShot::Trackside) *= 0.4;
			Weight(EShot::Helicopter) *= 2.0;
		}

		// Never the same angle twice running, nor the one before it; the
		// onboard views read as one, and are rationed.
		for (int32 Index = 0; Index < History.Num(); ++Index)
		{
			if (Index < 2)
			{
				Weight(History[Index]) = 0.0;
			}
			if (IsOnboard(History[Index]))
			{
				Weight(EShot::Onboard) *= 0.25;
				Weight(EShot::Nose) *= 0.25;
			}
		}

		double Total = 0.0;
		for (double W : Weights)
		{
			Total += W;
		}
		if (Total <= 0.0)
		{
			return EShot::Chase;
		}
		double Roll = Rng.FRandRange(0.0f, 1.0f) * Total;
		for (int32 Index = 1; Index < static_cast<int32>(EShot::Count); ++Index)
		{
			Roll -= Weights[Index];
			if (Roll <= 0.0 && Weights[Index] > 0.0)
			{
				return static_cast<EShot>(Index);
			}
		}
		return EShot::Chase;
	}

	bool FDirector::SetUpShot(EShot InShot, const FCar& Target, TConstArrayView<FCar> Cars, const FWorldQueries& World)
	{
		FShotSetup S;
		const float Pace = FMath::Max(Settings.PaceScale, 0.1f);
		auto RandSign = [this]() { return Rng.FRand() < 0.5f ? -1.0 : 1.0; };
		auto Range = [this](double Min, double Max) { return static_cast<double>(Rng.FRandRange(static_cast<float>(Min), static_cast<float>(Max))); };

		switch (InShot)
		{
		case EShot::Grid:
		{
			const TArray<const FCar*> Order = RaceOrder(Cars);
			const FCar& Pole = *Order[0];
			S.Side = RandSign();
			if (History.Num() > 0 && History[0] == EShot::Grid)
			{
				// Second grid shot: low, ahead of the pole-sitter, slowly pushing in.
				S.Variant = 1;
				S.DistanceCm = Range(9.0, 12.0) * Metre;
				S.HeightCm = 0.8 * Metre;
				S.OrbitDeg = Range(18.0, 32.0);
				S.FrameFraction = 0.5f;
				ShotLength = 30.0f;
			}
			else
			{
				// A crane from the front of the grid to the back.
				S.Variant = 0;
				S.Anchor = Pole.Location;
				const FVector Back = Order.Last()->Location;
				S.Axis = Order.Num() > 1 && FVector::Dist2D(Pole.Location, Back) > 5.0 * Metre
					? (Pole.Location - Back).GetSafeNormal2D()
					: Heading(Pole.Rotation.Yaw);
				S.EndCm = FVector::Dist2D(Pole.Location, Back);
				S.DistanceCm = Range(10.0, 13.0) * Metre;
				S.HeightCm = Range(3.5, 5.5) * Metre;
				ShotLength = Range(5.0, 6.5);
			}
			break;
		}

		case EShot::Trackside:
		{
			if (!Path.IsValid())
			{
				return false;
			}
			const double Speed = FMath::Max(static_cast<double>(Target.SpeedCmPerS()), 15.0 * Metre);
			const double Here = Path.StationAt(Target.Location);
			double Ahead = FMath::Clamp(Speed * 3.2, 60.0 * Metre, 190.0 * Metre) + Range(-10.0, 25.0) * Metre;

			for (int32 Attempt = 0; Attempt < 3; ++Attempt, Ahead *= 0.7)
			{
				FVector Point;
				FVector Tangent;
				Path.Sample(Here + Ahead, Point, Tangent);
				// The outside of the corner, where the car comes towards the lens.
				const double Turn = Path.TurnDeg(Here + Ahead, 40.0 * Metre);
				const double Outside = Turn > 4.0 ? -1.0 : Turn < -4.0 ? 1.0 : RandSign();
				const double Lateral = Range(14.0, 26.0) * Metre;
				const double Height = Range(2.5, 6.0) * Metre;

				FVector Road = Point;
				Road.Z = GroundBelow(Point + FVector(0, 0, Target.Location.Z + 50.0 * Metre), Target.Location.Z, World) + Metre;

				// The road the car will come along, not just the spot the camera
				// faces: a tripod behind a row of trees sees the car for a moment.
				FVector Approach[3];
				for (int32 Back = 0; Back < 3; ++Back)
				{
					FVector Along;
					FVector Unused;
					Path.Sample(Here + Ahead - Back * 35.0 * Metre, Along, Unused);
					Along.Z = GroundBelow(Along + FVector(0, 0, Target.Location.Z + 50.0 * Metre), Target.Location.Z, World) + Metre;
					Approach[Back] = Along;
				}

				for (double SideTry : { Outside, -Outside })
				{
					FVector Camera = Point + RightOf(Tangent) * SideTry * Lateral;
					Camera.Z = GroundBelow(Camera + FVector(0, 0, Target.Location.Z + 50.0 * Metre), Target.Location.Z, World) + Height;
					int32 Seen = 0;
					for (const FVector& OnRoad : Approach)
					{
						Seen += !World.IsClear || World.IsClear(Camera, OnRoad);
					}
					if (Seen == 3 || (Seen == 2 && (!World.IsClear || World.IsClear(Camera, Road))))
					{
						S.Anchor = Camera;
						S.Axis = Tangent;
						S.Side = SideTry;
						S.FrameFraction = static_cast<float>(Range(0.22, 0.32));
						ShotLength = 11.0f;
						Setup = S;
						ShotLength *= Pace;
						return true;
					}
				}
			}
			return false;
		}

		case EShot::Helicopter:
		{
			const bool bOpening = History.Num() > 0 && History[0] == EShot::Grid;
			S.DistanceCm = Range(40.0, 65.0) * Metre;
			S.HeightCm = (bOpening ? Range(45.0, 60.0) : Range(28.0, 50.0)) * Metre;
			S.OrbitDeg = RandSign() * Range(35.0, 140.0);
			S.OrbitRateDegPerS = RandSign() * Range(3.0, 7.0);
			// The start wants the field in frame, not just the leader.
			S.FrameFraction = static_cast<float>(bOpening ? 0.05 : Range(0.10, 0.18));
			ShotLength = bOpening ? 8.0f : static_cast<float>(Range(6.0, 9.0));
			break;
		}

		case EShot::Tracking:
			S.Side = RandSign();
			S.DistanceCm = Range(7.0, 10.5) * Metre;
			S.HeightCm = Range(1.0, 2.2) * Metre;
			S.StartCm = Range(-12.0, -7.0) * Metre;
			S.EndCm = Range(2.0, 7.0) * Metre;
			S.FrameFraction = 0.42f;
			ShotLength = static_cast<float>(Range(4.5, 7.0));
			break;

		case EShot::Chase:
			S.DistanceCm = Range(10.0, 15.0) * Metre;
			S.HeightCm = Range(1.6, 3.0) * Metre;
			S.Side = Range(0.0, 2.5) * Metre;
			S.FrameFraction = static_cast<float>(Range(0.32, 0.42));
			ShotLength = static_cast<float>(Range(4.0, 6.5));
			break;

		case EShot::Onboard:
			ShotLength = static_cast<float>(Range(4.0, 6.0));
			break;

		case EShot::Nose:
			ShotLength = static_cast<float>(Range(3.0, 5.0));
			break;

		case EShot::Reverse:
			S.DistanceCm = Range(12.0, 18.0) * Metre;
			S.HeightCm = Range(1.2, 2.2) * Metre;
			S.Side = Range(-2.0, 2.0) * Metre;
			S.FrameFraction = static_cast<float>(Range(0.28, 0.38));
			ShotLength = static_cast<float>(Range(3.5, 6.0));
			break;

		default:
			return false;
		}

		Setup = S;
		if (InShot != EShot::Grid)
		{
			ShotLength *= Pace;
		}
		return true;
	}

	void FDirector::Evaluate(const FCar& Target, TConstArrayView<FCar> Cars, float DeltaSeconds, float TimeSeconds,
		const FWorldQueries& World, FPose& OutPose)
	{
		const FVector Centre = Target.Centre();
		const double BodyLength = FMath::Max(Target.Body.GetSize().X, 200.0);
		const FVector Velocity = Target.Velocity;
		const double Speed = Target.SpeedCmPerS();
		// Heading of motion, or of the car when it is not going anywhere.
		const double MotionYaw = Speed > 3.0 * Metre ? Velocity.Rotation().Yaw : Target.Rotation.Yaw;

		if (bFresh)
		{
			FrameYawDeg = MotionYaw;
		}

		FVector Location = CamLocation;
		FVector LookAt = Centre;
		FQuat Rigid = FQuat::Identity;
		bool bRigid = false;
		double FollowRate = 7.0;
		float Fov = 60.0f;
		float FovRate = 2.5f;
		float Aperture = 4.0f;
		float Wobble = 0.1f;
		float WobbleRate = 1.6f;
		const float Age = ShotAge;
		const float Progress = ShotLength > 0.0f ? Age / ShotLength : 0.0f;

		switch (Shot)
		{
		case EShot::Grid:
			if (Setup.Variant == 0)
			{
				const FVector Right = RightOf(Setup.Axis);
				// From a car length ahead of the pole back past the last row.
				const double Along = FMath::Lerp(10.0 * Metre, -Setup.EndCm - 5.0 * Metre, SmoothStep(Progress));
				FVector OnGrid = Setup.Anchor + Setup.Axis * Along;
				OnGrid.Z = Setup.Anchor.Z;
				Location = OnGrid + Right * Setup.Side * Setup.DistanceCm + FVector(0, 0, Setup.HeightCm);
				LookAt = OnGrid + Setup.Axis * 6.0 * Metre + FVector(0, 0, 0.6 * Metre);
				Fov = 48.0f;
				FollowRate = 5.0;
				Aperture = 2.8f;
			}
			else
			{
				const FVector Forward = Heading(Target.Rotation.Yaw);
				const FVector Toward = FRotator(0.0, Target.Rotation.Yaw + Setup.Side * Setup.OrbitDeg, 0.0).Vector();
				// A slow push in, as a camera operator walks towards the car.
				const double Distance = FMath::Max(Setup.DistanceCm - Age * 45.0, 5.0 * Metre);
				Location = Target.Location + Toward * Distance + FVector(0, 0, Setup.HeightCm);
				LookAt = Centre + Forward * 0.2 * Metre;
				Fov = FramingFovDeg(BodyLength, Distance, Setup.FrameFraction, 22.0f, 60.0f);
				FollowRate = 4.0;
				Aperture = 2.8f;
			}
			Wobble = 0.08f;
			break;

		case EShot::Trackside:
		{
			Location = Setup.Anchor;
			const double Distance = FVector::Dist(Location, Centre);
			FollowRate = 9.0;
			Fov = FramingFovDeg(BodyLength, Distance, Setup.FrameFraction, 7.0f, 55.0f);
			FovRate = 3.5f;
			Aperture = Fov < 25.0f ? 2.8f : 5.6f;
			// The long lens shows every tremor of the operator's hands.
			Wobble = 0.12f * FMath::Sqrt(60.0f / FMath::Max(Fov, 5.0f));
			if (Distance < 60.0 * Metre)
			{
				bApproached = true;
			}
			break;
		}

		case EShot::Helicopter:
		{
			FrameYawDeg = EaseYaw(FrameYawDeg, MotionYaw, DeltaSeconds, 0.8);
			Setup.OrbitDeg += Setup.OrbitRateDegPerS * DeltaSeconds;
			const FVector Out = Heading(FrameYawDeg + 180.0 + Setup.OrbitDeg);
			Location = Target.Location + Out * Setup.DistanceCm + FVector(0, 0, Setup.HeightCm);
			LookAt = Centre + Velocity * 0.15;
			FollowRate = 3.0;
			Fov = FramingFovDeg(BodyLength, FVector::Dist(Location, Centre), Setup.FrameFraction, 8.0f, 70.0f);
			FovRate = 1.5f;
			Aperture = 11.0f;
			Wobble = 0.25f;
			WobbleRate = 0.35f;
			break;
		}

		case EShot::Tracking:
		{
			FrameYawDeg = EaseYaw(FrameYawDeg, MotionYaw, DeltaSeconds, 3.5);
			const double Along = FMath::Lerp(Setup.StartCm, Setup.EndCm, SmoothStep(Progress));
			const FVector Local(Along, Setup.Side * Setup.DistanceCm, Setup.HeightCm);
			Location = Target.Location + FRotator(0.0, FrameYawDeg, 0.0).RotateVector(Local);
			FollowRate = 8.0;
			Fov = FramingFovDeg(BodyLength, FVector::Dist(Location, Centre), Setup.FrameFraction, 28.0f, 75.0f);
			Aperture = 4.0f;
			break;
		}

		case EShot::Chase:
		{
			FrameYawDeg = EaseYaw(FrameYawDeg, MotionYaw, DeltaSeconds, 2.5);
			const FVector Local(-Setup.DistanceCm, FMath::Sin(Age * 0.6) * Setup.Side, Setup.HeightCm);
			Location = Target.Location + FRotator(0.0, FrameYawDeg, 0.0).RotateVector(Local);
			LookAt = Centre + Heading(MotionYaw) * 2.0 * Metre;
			FollowRate = 7.0;
			Fov = FramingFovDeg(BodyLength, FVector::Dist(Location, Centre), Setup.FrameFraction, 22.0f, 65.0f);
			Aperture = 4.0f;
			break;
		}

		case EShot::Reverse:
		{
			FrameYawDeg = EaseYaw(FrameYawDeg, MotionYaw, DeltaSeconds, 3.0);
			const FVector Local(Setup.DistanceCm, Setup.Side, Setup.HeightCm);
			Location = Target.Location + FRotator(0.0, FrameYawDeg, 0.0).RotateVector(Local);
			FollowRate = 7.0;
			Fov = FramingFovDeg(BodyLength, FVector::Dist(Location, Centre), Setup.FrameFraction, 25.0f, 70.0f);
			Aperture = 4.0f;
			break;
		}

		case EShot::Onboard:
		case EShot::Nose:
		{
			bRigid = true;
			const bool bNose = Shot == EShot::Nose;
			const FVector Mount = bNose
				? FVector(Target.Body.Max.X - 15.0, 0.0, Target.Body.Min.Z + 0.3 * Target.Body.GetSize().Z)
				: Target.EyeLocal + FVector(-25.0, 0.0, 38.0);
			Location = Target.Location + Target.Rotation.RotateVector(Mount);
			// Road texture through the mount: more of it the faster the car goes.
			const float Buzz = 0.18f * FMath::Clamp(static_cast<float>(Speed / (80.0 * Metre)), 0.0f, 1.0f) * Settings.Handheld;
			const FRotator Shake(FMath::PerlinNoise1D(TimeSeconds * 13.0f) * Buzz, FMath::PerlinNoise1D(TimeSeconds * 11.0f + 30.0f) * Buzz * 0.5f, 0.0f);
			Rigid = Target.Rotation.Quaternion() * FRotator(bNose ? -3.0 : -5.0, 0.0, 0.0).Quaternion() * Shake.Quaternion();
			Fov = bNose ? 92.0f : 80.0f;
			Aperture = 0.0f;
			break;
		}

		default:
			Location = Target.Location + FVector(-12.0 * Metre, 0.0, 3.0 * Metre);
			break;
		}

		if (!bRigid)
		{
			// Never below the ground a camera is standing or hanging over. A hit
			// far overhead is a bridge deck or a tree, not ground to climb onto.
			const double Ground = GroundBelow(Location + FVector(0, 0, 60.0 * Metre), -TNumericLimits<double>::Max(), World);
			if (Location.Z < Ground + 60.0 && Ground - Location.Z < 4.0 * Metre)
			{
				Location.Z = Ground + 60.0;
			}
			// An operator's pan lags by about 1/rate behind the car's motion
			// across the lens; aim that far ahead so the car sits in the frame
			// rather than trailing out of it. A camera travelling with the car
			// sees little of that motion, a tripod all of it.
			const FVector CameraVelocity = !bFresh && DeltaSeconds > 0.0f ? (Location - CamLocation) / DeltaSeconds : FVector::ZeroVector;
			LookAt += (Velocity - CameraVelocity) * (1.0 / FollowRate) + Velocity * 0.05;
		}

		const FQuat Wanted = bRigid ? Rigid : FRotationMatrix::MakeFromX(LookAt - Location).ToQuat();
		if (bFresh)
		{
			CamRotation = Wanted;
			CamFov = Fov;
			bFresh = false;
		}
		else if (bRigid)
		{
			CamRotation = Wanted;
			CamFov = Fov;
		}
		else
		{
			const double Alpha = 1.0 - FMath::Exp(-FollowRate * DeltaSeconds);
			CamRotation = FQuat::Slerp(CamRotation, Wanted, Alpha).GetNormalized();
			CamFov = FMath::FInterpTo(CamFov, Fov, DeltaSeconds, FovRate);
		}
		CamLocation = Location;

		FRotator Rotation = CamRotation.Rotator();
		if (!bRigid)
		{
			const float Amount = Wobble * Settings.Handheld;
			Rotation.Pitch += FMath::PerlinNoise1D(TimeSeconds * WobbleRate + 11.0f) * Amount;
			Rotation.Yaw += FMath::PerlinNoise1D(TimeSeconds * WobbleRate * 0.83f + 47.0f) * Amount;
			Rotation.Roll = 0.0;
		}

		OutPose.Location = CamLocation;
		OutPose.Rotation = Rotation;
		OutPose.FovDeg = CamFov;
		OutPose.FocusDistanceCm = static_cast<float>(FVector::Dist(CamLocation, Centre));
		OutPose.Aperture = Aperture;
	}

	bool FDirector::IsShotSpent(const FCar& Target, const FPose& Pose) const
	{
		if (ForcedShot != EShot::None)
		{
			return false;
		}
		const double Speed = Target.SpeedCmPerS();
		switch (Shot)
		{
		case EShot::Trackside:
		{
			if (ShotAge < 1.5f)
			{
				return false;
			}
			const FVector ToCar = Target.Location - Pose.Location;
			const double Distance = ToCar.Size2D();
			const bool bLeaving = FVector::DotProduct(ToCar.GetSafeNormal2D(), Target.Velocity.GetSafeNormal2D()) > 0.0;
			// Once it has gone past, hold it for a moment and cut; a car that
			// never came (it pitted, or the camera guessed the wrong road) too.
			if (bLeaving && (bApproached || ShotAge > 4.0f) && Distance > FMath::Max(35.0 * Metre, Speed * 1.2))
			{
				return true;
			}
			return ShotAge > 9.0f && Distance > 250.0 * Metre;
		}

		case EShot::Grid:
			return false;

		default:
			// A camera moving with a car that has stopped has nothing to show.
			return ShotAge > 2.0f && Speed < 3.0 * Metre && !IsOnboard(Shot);
		}
	}
}

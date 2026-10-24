#include "School/ApexSchool.h"

#include "Guide/ApexTrackGuide.h"

namespace ApexSchoolText
{
	constexpr float MpsToKph = 3.6f;

	FString Metres(float Value, bool bMetric)
	{
		const float Abs = FMath::Abs(Value);
		return bMetric ? FString::Printf(TEXT("%.1f m"), Abs) : FString::Printf(TEXT("%.0f ft"), Abs * 3.28084f);
	}

	FString Seconds(float Value)
	{
		return FString::Printf(TEXT("%.3f s"), Value);
	}

	/** The braked corners a test can be made of: a real stop, entry before exit inside the lap. */
	bool IsBraked(const FApexGuideCorner& Corner, float LapM)
	{
		return Corner.bHasBrake && !Corner.bFlatOut && Corner.BrakeM > 1.0f && Corner.EntrySpeedKph > Corner.MinSpeedKph + 5.0f
			&& Corner.EntryM >= 0.0f && Corner.EntryM < Corner.ExitM && Corner.ExitM <= LapM;
	}
}

TArray<ApexSchool::FTest> ApexSchool::BuildTests(const FApexTrackGuide& Guide)
{
	using namespace ApexSchoolText;
	TArray<FTest> Tests;
	const float LapM = Guide.Track.LengthM;
	if (LapM <= 100.0f)
	{
		return Tests;
	}
	TArray<const FApexGuideCorner*> Braked;
	for (const FApexGuideCorner& Corner : Guide.Corners)
	{
		if (IsBraked(Corner, LapM))
		{
			Braked.Add(&Corner);
		}
	}
	Braked.Sort([](const FApexGuideCorner& A, const FApexGuideCorner& B) { return A.EntryM < B.EntryM; });

	// The brake test: the biggest stop of the lap, the board where the guide
	// car braked for it, the line where its own deceleration from there would
	// have stopped it (with some slack).
	{
		const FApexGuideCorner* Best = nullptr;
		float BestLineM = 0.0f;
		for (const FApexGuideCorner* Corner : Braked)
		{
			const float BoardM = Corner->ApexM - Corner->BrakeM;
			const float EntryMps = Corner->EntrySpeedKph / MpsToKph;
			const float MinMps = Corner->MinSpeedKph / MpsToKph;
			const float Decel = (EntryMps * EntryMps - MinMps * MinMps) / (2.0f * Corner->BrakeM);
			if (BoardM <= 0.0f || Decel <= 0.1f)
			{
				continue;
			}
			const float LineM = BoardM + EntryMps * EntryMps / (2.0f * Decel) * Rules::StopSlack;
			if (LineM >= LapM - 10.0f)
			{
				continue;
			}
			if (!Best || Corner->EntrySpeedKph - Corner->MinSpeedKph > Best->EntrySpeedKph - Best->MinSpeedKph)
			{
				Best = Corner;
				BestLineM = LineM;
			}
		}
		if (Best)
		{
			FTest& Test = Tests.AddDefaulted_GetRef();
			Test.Id = TEXT("brake");
			Test.Kind = EApexSchoolKind::Brake;
			Test.Title = TEXT("Brake test");
			Test.Place = Best->Name;
			Test.FromM = Best->ApexM - Best->BrakeM;
			Test.ToM = BestLineM;
			SetReference(Test, Best->EntrySpeedKph / MpsToKph);
		}
	}

	// The corner exit: the slowest corner, where the exit speed carries down
	// the straight after it.
	const FApexGuideCorner* Slowest = nullptr;
	for (const FApexGuideCorner* Corner : Braked)
	{
		if (Corner->ExitSpeedKph > 0.0f && (!Slowest || Corner->MinSpeedKph < Slowest->MinSpeedKph))
		{
			Slowest = Corner;
		}
	}
	if (Slowest)
	{
		FTest& Test = Tests.AddDefaulted_GetRef();
		Test.Id = TEXT("exit");
		Test.Kind = EApexSchoolKind::CornerExit;
		Test.Title = TEXT("Corner exit");
		Test.Place = Slowest->Name;
		Test.FromM = Slowest->EntryM;
		Test.ToM = Slowest->ExitM;
		SetReference(Test, Slowest->ExitSpeedKph / MpsToKph);
	}

	// The corner time: the longest braked corner (a sequence, as a rule)
	// other than the exit test's, timed entry to exit. Its reference is the
	// guide car's own time, measured from the recording afterwards.
	const FApexGuideCorner* Longest = nullptr;
	for (const FApexGuideCorner* Corner : Braked)
	{
		if ((Corner != Slowest || Braked.Num() == 1)
			&& (!Longest || Corner->ExitM - Corner->EntryM > Longest->ExitM - Longest->EntryM))
		{
			Longest = Corner;
		}
	}
	if (Longest)
	{
		FTest& Test = Tests.AddDefaulted_GetRef();
		Test.Id = TEXT("corner");
		Test.Kind = EApexSchoolKind::CornerTime;
		Test.Title = TEXT("Corner time");
		Test.Place = Longest->Name;
		Test.FromM = Longest->EntryM;
		Test.ToM = Longest->ExitM;
	}

	if (Guide.Track.LapTimeS > 0.0f)
	{
		FTest& Test = Tests.AddDefaulted_GetRef();
		Test.Id = TEXT("lap");
		Test.Kind = EApexSchoolKind::Lap;
		Test.Title = TEXT("Clean lap");
		SetReference(Test, Guide.Track.LapTimeS);
	}
	return Tests;
}

void ApexSchool::SetReference(FTest& Test, float Reference)
{
	Test.Reference = FMath::Max(0.0f, Reference);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		switch (Test.Kind)
		{
		case EApexSchoolKind::Brake:
			Test.Limits[Index] = Rules::StopToleranceM[Index];
			break;
		case EApexSchoolKind::CornerExit:
			Test.Limits[Index] = Test.Reference * Rules::ExitShare[Index];
			break;
		case EApexSchoolKind::CornerTime:
			Test.Limits[Index] = Test.Reference * Rules::CornerTimeShare[Index];
			break;
		case EApexSchoolKind::Lap:
			Test.Limits[Index] = Test.Reference * Rules::LapTimeShare[Index];
			break;
		}
	}
	if (Test.Kind == EApexSchoolKind::Brake)
	{
		Test.MinSpeedMps = Test.Reference * Rules::BoardSpeedShare;
	}
}

EApexMedal ApexSchool::MedalFor(const FTest& Test, float Value)
{
	if (!Test.IsReady())
	{
		return EApexMedal::None;
	}
	for (int32 Index = 2; Index >= 0; --Index)
	{
		bool bMade = false;
		switch (Test.Kind)
		{
		case EApexSchoolKind::Brake:
			bMade = FMath::Abs(Value) <= Test.Limits[Index];
			break;
		case EApexSchoolKind::CornerExit:
			bMade = Value >= Test.Limits[Index];
			break;
		case EApexSchoolKind::CornerTime:
		case EApexSchoolKind::Lap:
			bMade = Value > 0.0f && Value <= Test.Limits[Index];
			break;
		}
		if (bMade)
		{
			return static_cast<EApexMedal>(Index + 1);
		}
	}
	return EApexMedal::None;
}

bool ApexSchool::IsBetter(const FTest& Test, float A, float B)
{
	switch (Test.Kind)
	{
	case EApexSchoolKind::Brake:
		return FMath::Abs(A) < FMath::Abs(B);
	case EApexSchoolKind::CornerExit:
		return A > B;
	default:
		return A < B;
	}
}

FString ApexSchool::MedalKey(const FString& TrackId, const FString& CarId, const FString& TestId)
{
	return FString::Printf(TEXT("%s|%s|%s"), *TrackId, *CarId, *TestId);
}

const FApexSchoolMedal* ApexSchool::FindMedal(const TArray<FApexSchoolMedal>& Medals, const FString& Key)
{
	return Medals.FindByPredicate([&Key](const FApexSchoolMedal& Medal) { return Medal.Key.Equals(Key, ESearchCase::IgnoreCase); });
}

bool ApexSchool::FileResult(TArray<FApexSchoolMedal>& Medals, const FString& Key, const FTest& Test, const FResult& Result)
{
	FApexSchoolMedal* Entry = Medals.FindByPredicate([&Key](const FApexSchoolMedal& Medal) { return Medal.Key.Equals(Key, ESearchCase::IgnoreCase); });
	if (!Entry)
	{
		Entry = &Medals.AddDefaulted_GetRef();
		Entry->Key = Key;
	}
	++Entry->Tries;
	if (!Result.bPassed || Result.Medal == EApexMedal::None)
	{
		return false;
	}
	const bool bBetter = Entry->Medal == EApexMedal::None || Result.Medal > Entry->Medal
		|| (Result.Medal == Entry->Medal && IsBetter(Test, Result.Value, Entry->Best));
	if (bBetter)
	{
		Entry->Medal = Result.Medal;
		Entry->Best = Result.Value;
	}
	return bBetter;
}

FString ApexSchool::MedalName(EApexMedal Medal)
{
	switch (Medal)
	{
	case EApexMedal::Gold:   return TEXT("GOLD");
	case EApexMedal::Silver: return TEXT("SILVER");
	case EApexMedal::Bronze: return TEXT("BRONZE");
	default:                 return TEXT("NO MEDAL");
	}
}

FLinearColor ApexSchool::MedalColour(EApexMedal Medal)
{
	switch (Medal)
	{
	case EApexMedal::Gold:   return FLinearColor::FromSRGBColor(FColor(0xE8, 0xC0, 0x3A));
	case EApexMedal::Silver: return FLinearColor::FromSRGBColor(FColor(0xC4, 0xC8, 0xCE));
	case EApexMedal::Bronze: return FLinearColor::FromSRGBColor(FColor(0xC0, 0x7A, 0x45));
	default:                 return FLinearColor::FromSRGBColor(FColor(0x6E, 0x6E, 0x76));
	}
}

FString ApexSchool::GoalText(const FTest& Test, bool bMetric)
{
	using namespace ApexSchoolText;
	switch (Test.Kind)
	{
	case EApexSchoolKind::Brake:
		return FString::Printf(TEXT("Reach the board before %s at %s or more, then stop on the line %s after it."), *Test.Place,
			*ApexTrackGuide::SpeedText(Test.MinSpeedMps * MpsToKph, bMetric), *ApexTrackGuide::DistanceText(Test.ToM - Test.FromM, bMetric));
	case EApexSchoolKind::CornerExit:
		return FString::Printf(TEXT("Take %s inside track limits and leave it as fast as you can."), *Test.Place);
	case EApexSchoolKind::CornerTime:
		return FString::Printf(TEXT("Drive %s from its entry to its exit as quickly as you can, inside track limits."), *Test.Place);
	case EApexSchoolKind::Lap:
	default:
		return TEXT("One lap from the line, inside track limits.");
	}
}

FString ApexSchool::TargetText(const FTest& Test, EApexMedal Medal, bool bMetric)
{
	using namespace ApexSchoolText;
	const int32 Index = FMath::Clamp(static_cast<int32>(Medal) - 1, 0, 2);
	if (!Test.IsReady())
	{
		return TEXT("—");
	}
	switch (Test.Kind)
	{
	case EApexSchoolKind::Brake:
		return FString::Printf(TEXT("within %s of the line"), *Metres(Test.Limits[Index], bMetric));
	case EApexSchoolKind::CornerExit:
		return FString::Printf(TEXT("out at %s or more"), *ApexTrackGuide::SpeedText(Test.Limits[Index] * MpsToKph, bMetric));
	case EApexSchoolKind::CornerTime:
		return FString::Printf(TEXT("%s or less"), *Seconds(Test.Limits[Index]));
	case EApexSchoolKind::Lap:
	default:
		return FString::Printf(TEXT("%s or less"), *ApexTrackGuide::LapTimeText(Test.Limits[Index]));
	}
}

FString ApexSchool::FigureText(const FTest& Test, float Value, bool bMetric)
{
	using namespace ApexSchoolText;
	switch (Test.Kind)
	{
	case EApexSchoolKind::Brake:
		return FString::Printf(TEXT("%s %s the line"), *Metres(Value, bMetric), Value >= 0.0f ? TEXT("past") : TEXT("short of"));
	case EApexSchoolKind::CornerExit:
		return ApexTrackGuide::SpeedText(Value * MpsToKph, bMetric);
	case EApexSchoolKind::CornerTime:
		return Seconds(Value);
	case EApexSchoolKind::Lap:
	default:
		return ApexTrackGuide::LapTimeText(Value);
	}
}

bool ApexSchool::PoseAtStation(const TArray<FVector>& LineCm, float StationM, FVector& OutCm, FVector& OutForward)
{
	const int32 Count = LineCm.Num();
	if (Count < 2)
	{
		return false;
	}
	double LoopCm = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		LoopCm += FVector2D::Distance(FVector2D(LineCm[Index]), FVector2D(LineCm[(Index + 1) % Count]));
	}
	if (LoopCm <= 0.0)
	{
		return false;
	}
	double Want = FMath::Fmod(static_cast<double>(StationM) * 100.0, LoopCm);
	if (Want < 0.0)
	{
		Want += LoopCm;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector& A = LineCm[Index];
		const FVector& B = LineCm[(Index + 1) % Count];
		const double Span = FVector2D::Distance(FVector2D(A), FVector2D(B));
		if (Want <= Span || Index == Count - 1)
		{
			const double Alpha = Span > 0.0 ? FMath::Clamp(Want / Span, 0.0, 1.0) : 0.0;
			OutCm = FMath::Lerp(A, B, Alpha);
			OutForward = FVector(B.X - A.X, B.Y - A.Y, 0.0).GetSafeNormal();
			return true;
		}
		Want -= Span;
	}
	return false;
}

// --- The judge ----------------------------------------------------------------

void ApexSchool::FJudge::Begin(const FTest& InTest, float InLapM)
{
	Test = InTest;
	LapM = InLapM;
	Phase = EJudgePhase::Waiting;
	Result = FResult();
	AbortReason.Reset();
	bHasPrev = false;
	Prev = FSample();
	StartTimeS = 0.0;
	StartLap = 0;
	TravelledM = 0.0f;
	bStruckAtStart = false;
}

bool ApexSchool::FJudge::Crossed(float AtM, float FromStation, float ToStation, float& OutAlpha) const
{
	OutAlpha = 0.0f;
	if (LapM <= 0.0f)
	{
		return false;
	}
	float Delta = ToStation - FromStation;
	if (Delta < -LapM * 0.5f)
	{
		Delta += LapM;
	}
	else if (Delta > LapM * 0.5f)
	{
		Delta -= LapM;
	}
	if (Delta <= 0.0f)
	{
		return false;
	}
	const float Offset = FMath::Fmod(FMath::Fmod(AtM - FromStation, LapM) + LapM, LapM);
	if (Offset <= 0.0f || Offset > Delta)
	{
		return false;
	}
	OutAlpha = Offset / Delta;
	return true;
}

void ApexSchool::FJudge::Finish(bool bPassed, float Value, const FString& Why)
{
	Result.Value = Value;
	Result.Medal = bPassed ? MedalFor(Test, Value) : EApexMedal::None;
	Result.bPassed = bPassed && Result.Medal != EApexMedal::None;
	Result.Why = Result.bPassed ? FString() : Why;
	Phase = EJudgePhase::Done;
}

void ApexSchool::FJudge::Abort(const FString& Why)
{
	AbortReason = Why;
	Phase = EJudgePhase::Aborted;
}

bool ApexSchool::FJudge::Feed(const FSample& Sample)
{
	if (IsOver())
	{
		return false;
	}
	const bool bRunning = Phase == EJudgePhase::Running;
	// The car parked, moved or the clock wound back: no crossing between the two.
	if (Sample.bInGarage)
	{
		if (bRunning)
		{
			Abort(TEXT("Back in the garage"));
			return true;
		}
		bHasPrev = false;
		return false;
	}
	if (!bHasPrev)
	{
		Prev = Sample;
		bHasPrev = true;
		return false;
	}
	float Step = Sample.StationM - Prev.StationM;
	if (Step < -LapM * 0.5f)
	{
		Step += LapM;
	}
	else if (Step > LapM * 0.5f)
	{
		Step -= LapM;
	}
	if (Sample.TimeS < Prev.TimeS || FMath::Abs(Step) > Rules::JumpM)
	{
		if (bRunning)
		{
			Abort(Sample.TimeS < Prev.TimeS ? TEXT("The session went back in time") : TEXT("The car was moved"));
			return true;
		}
		Prev = Sample;
		return false;
	}

	const FSample Last = Prev;
	Prev = Sample;
	auto Lerp = [&Last, &Sample](float Alpha, double& OutTime, float& OutSpeed)
	{
		OutTime = Last.TimeS + (Sample.TimeS - Last.TimeS) * Alpha;
		OutSpeed = FMath::Lerp(Last.SpeedMps, Sample.SpeedMps, Alpha);
	};

	float Alpha = 0.0f;
	double AtTime = 0.0;
	float AtSpeed = 0.0f;
	if (Phase == EJudgePhase::Waiting)
	{
		const float StartM = Test.Kind == EApexSchoolKind::Lap ? 0.0f : Test.FromM;
		if (!Crossed(StartM, Last.StationM, Sample.StationM, Alpha))
		{
			return false;
		}
		Lerp(Alpha, AtTime, AtSpeed);
		Phase = EJudgePhase::Running;
		StartTimeS = AtTime;
		StartLap = Sample.Lap;
		TravelledM = Step * (1.0f - Alpha);
		bStruckAtStart = Test.Kind != EApexSchoolKind::Lap && Sample.bLapInvalid;
		if (Test.Kind == EApexSchoolKind::Brake)
		{
			Result.SpeedMps = AtSpeed;
			if (AtSpeed < Test.MinSpeedMps)
			{
				Finish(false, 0.0f, FString::Printf(TEXT("Too slow at the board: %.0f of %.0f km/h"),
					AtSpeed * ApexSchoolText::MpsToKph, Test.MinSpeedMps * ApexSchoolText::MpsToKph));
				return true;
			}
		}
		return false;
	}

	// Running.
	TravelledM += FMath::Max(0.0f, Step);
	switch (Test.Kind)
	{
	case EApexSchoolKind::Brake:
	{
		const float LineM = Test.ToM - Test.FromM;
		if (!bStruckAtStart && Sample.bLapInvalid)
		{
			Finish(false, 0.0f, TEXT("Off the track: the lap was struck"));
			return true;
		}
		if (Sample.SpeedMps < Rules::StoppedMps)
		{
			const float Off = TravelledM - LineM;
			Finish(true, Off, FString::Printf(TEXT("Stopped %.0f m %s the line"), FMath::Abs(Off), Off >= 0.0f ? TEXT("past") : TEXT("short of")));
			return true;
		}
		if (TravelledM > LineM + Test.Limits[0])
		{
			Result.Value = TravelledM - LineM;
			Finish(false, TravelledM - LineM, FString::Printf(TEXT("Ran through the box at %.0f km/h"), Sample.SpeedMps * ApexSchoolText::MpsToKph));
			return true;
		}
		return false;
	}

	case EApexSchoolKind::CornerExit:
	case EApexSchoolKind::CornerTime:
	{
		if (!bStruckAtStart && Sample.bLapInvalid)
		{
			Finish(false, 0.0f, TEXT("Off the track: the lap was struck"));
			return true;
		}
		if (Crossed(Test.ToM, Last.StationM, Sample.StationM, Alpha))
		{
			Lerp(Alpha, AtTime, AtSpeed);
			Result.SpeedMps = AtSpeed;
			Result.Seconds = static_cast<float>(AtTime - StartTimeS);
			const float Value = Test.Kind == EApexSchoolKind::CornerExit ? AtSpeed : Result.Seconds;
			Finish(true, Value, Test.Kind == EApexSchoolKind::CornerExit ? FString(TEXT("Too slow out of the corner")) : FString(TEXT("Too slow through the corner")));
			return true;
		}
		// Gone well past the exit without crossing it (backwards, or round the back of the paddock).
		if (TravelledM > (Test.ToM - Test.FromM) + 300.0f)
		{
			Finish(false, 0.0f, TEXT("Did not reach the corner's exit"));
			return true;
		}
		return false;
	}

	case EApexSchoolKind::Lap:
	default:
	{
		// The lap counter may tick a sample after the line: the lap started on is the one after it.
		if (TravelledM < LapM * 0.25f)
		{
			StartLap = FMath::Max(StartLap, Sample.Lap);
		}
		if (TravelledM > 50.0f && Sample.Lap == StartLap && Sample.bLapInvalid)
		{
			Finish(false, 0.0f, TEXT("Off the track: the lap was struck"));
			return true;
		}
		if (Sample.Lap > StartLap && TravelledM > LapM * 0.5f)
		{
			if (Sample.bLastLapInvalid)
			{
				Finish(false, 0.0f, TEXT("Off the track: the lap was struck"));
				return true;
			}
			float Seconds = Sample.LastLapMs > 0 ? Sample.LastLapMs / 1000.0f : 0.0f;
			if (Seconds <= 0.0f)
			{
				// No server time (yet): the clock between the two crossings.
				Crossed(0.0f, Last.StationM, Sample.StationM, Alpha);
				Lerp(Alpha, AtTime, AtSpeed);
				Seconds = static_cast<float>(AtTime - StartTimeS);
			}
			Result.Seconds = Seconds;
			Finish(true, Seconds, TEXT("Too slow"));
			return true;
		}
		return false;
	}
	}
}

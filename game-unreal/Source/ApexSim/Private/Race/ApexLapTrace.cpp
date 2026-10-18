#include "Race/ApexLapTrace.h"

#include "ApexSim.h"

namespace
{
	/** A lap starts here or not at all: within this of the line, and this soon. */
	constexpr float LapTraceStartM = 30.0f;
	constexpr float LapTraceStartS = 2.0f;
	/** A gap this long (a recovery, lost telemetry) breaks the lap. */
	constexpr float LapTraceHoleM = 100.0f;

	FApexTraceSample LapTraceLerp(const FApexTraceSample& A, const FApexTraceSample& B, float T)
	{
		FApexTraceSample Out;
		Out.DistanceM = FMath::Lerp(A.DistanceM, B.DistanceM, T);
		Out.TimeS = FMath::Lerp(A.TimeS, B.TimeS, T);
		Out.SpeedKph = FMath::Lerp(A.SpeedKph, B.SpeedKph, T);
		Out.Throttle = FMath::Lerp(A.Throttle, B.Throttle, T);
		Out.Brake = FMath::Lerp(A.Brake, B.Brake, T);
		Out.Steering = FMath::Lerp(A.Steering, B.Steering, T);
		Out.Gear = T < 0.5f ? A.Gear : B.Gear;
		Out.Rpm = FMath::Lerp(A.Rpm, B.Rpm, T);
		for (int32 Tyre = 0; Tyre < 4; ++Tyre)
		{
			Out.TyreC[Tyre] = A.TyreC[Tyre] < 0.0f || B.TyreC[Tyre] < 0.0f ? -1.0f : FMath::Lerp(A.TyreC[Tyre], B.TyreC[Tyre], T);
		}
		Out.X = FMath::Lerp(A.X, B.X, T);
		Out.Y = FMath::Lerp(A.Y, B.Y, T);
		return Out;
	}

	FString LapTraceTime(int32 Ms, const TCHAR* Separator)
	{
		return FString::Printf(TEXT("%d%s%02d%s%03d"), Ms / 60000, Separator, (Ms / 1000) % 60, Separator, Ms % 1000);
	}
}

FApexTraceSample FApexLapTrace::At(float DistanceM) const
{
	if (Samples.Num() == 0)
	{
		return FApexTraceSample();
	}
	if (DistanceM <= Samples[0].DistanceM)
	{
		return Samples[0];
	}
	if (DistanceM >= Samples.Last().DistanceM)
	{
		return Samples.Last();
	}
	// Samples are in distance order: the first one past it, by halving.
	int32 Low = 0;
	int32 High = Samples.Num() - 1;
	while (High - Low > 1)
	{
		const int32 Mid = (Low + High) / 2;
		(Samples[Mid].DistanceM <= DistanceM ? Low : High) = Mid;
	}
	const FApexTraceSample& A = Samples[Low];
	const FApexTraceSample& B = Samples[High];
	const float Span = B.DistanceM - A.DistanceM;
	return LapTraceLerp(A, B, Span > KINDA_SMALL_NUMBER ? (DistanceM - A.DistanceM) / Span : 0.0f);
}

FString ApexLapTrace::ToCsv(const FApexLapTrace& Trace)
{
	FString Out = FString::Printf(TEXT("# apexsim lap trace 1; track=%s; car=%s; lap=%d; lap_ms=%d; valid=%d; recorded=%s\n"),
		*Trace.TrackId, *Trace.CarId, Trace.LapNumber, Trace.LapMs, Trace.bValid ? 1 : 0, *Trace.RecordedAt.ToIso8601());
	Out += TEXT("distance_m,time_s,speed_kph,throttle,brake,steering,gear,rpm,tyre_fl_c,tyre_fr_c,tyre_rl_c,tyre_rr_c,x,y\n");
	for (const FApexTraceSample& S : Trace.Samples)
	{
		Out += FString::Printf(TEXT("%.1f,%.3f,%.1f,%.3f,%.3f,%.3f,%d,%.0f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f\n"),
			S.DistanceM, S.TimeS, S.SpeedKph, S.Throttle, S.Brake, S.Steering, S.Gear, S.Rpm,
			S.TyreC[0], S.TyreC[1], S.TyreC[2], S.TyreC[3], S.X, S.Y);
	}
	return Out;
}

bool ApexLapTrace::FromCsv(const FString& Text, FApexLapTrace& Out)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	if (Lines.Num() < 2 || !Lines[0].StartsWith(TEXT("# apexsim lap trace 1;")))
	{
		return false;
	}
	FApexLapTrace Trace;
	TArray<FString> Fields;
	Lines[0].ParseIntoArray(Fields, TEXT(";"));
	for (FString Field : Fields)
	{
		FString Key, Value;
		if (!Field.TrimStartAndEnd().Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		if (Key == TEXT("track")) { Trace.TrackId = Value; }
		else if (Key == TEXT("car")) { Trace.CarId = Value; }
		else if (Key == TEXT("lap")) { Trace.LapNumber = FCString::Atoi(*Value); }
		else if (Key == TEXT("lap_ms")) { Trace.LapMs = FCString::Atoi(*Value); }
		else if (Key == TEXT("valid")) { Trace.bValid = Value == TEXT("1"); }
		else if (Key == TEXT("recorded")) { FDateTime::ParseIso8601(*Value, Trace.RecordedAt); }
	}
	for (int32 Index = 2; Index < Lines.Num(); ++Index)
	{
		TArray<FString> Cells;
		Lines[Index].ParseIntoArray(Cells, TEXT(","), false);
		if (Cells.Num() < 14)
		{
			continue;
		}
		FApexTraceSample S;
		S.DistanceM = FCString::Atof(*Cells[0]);
		S.TimeS = FCString::Atof(*Cells[1]);
		S.SpeedKph = FCString::Atof(*Cells[2]);
		S.Throttle = FCString::Atof(*Cells[3]);
		S.Brake = FCString::Atof(*Cells[4]);
		S.Steering = FCString::Atof(*Cells[5]);
		S.Gear = FCString::Atoi(*Cells[6]);
		S.Rpm = FCString::Atof(*Cells[7]);
		for (int32 Tyre = 0; Tyre < 4; ++Tyre)
		{
			S.TyreC[Tyre] = FCString::Atof(*Cells[8 + Tyre]);
		}
		S.X = FCString::Atof(*Cells[12]);
		S.Y = FCString::Atof(*Cells[13]);
		Trace.Samples.Add(S);
	}
	if (Trace.IsEmpty())
	{
		return false;
	}
	Out = MoveTemp(Trace);
	return true;
}

TArray<FVector2f> ApexLapTrace::DeltaByDistance(const FApexLapTrace& Reference, const FApexLapTrace& Lap, float StepM)
{
	TArray<FVector2f> Out;
	if (Reference.IsEmpty() || Lap.IsEmpty() || StepM <= 0.0f)
	{
		return Out;
	}
	const float End = FMath::Min(Reference.LengthM(), Lap.LengthM());
	for (float Distance = 0.0f; Distance <= End; Distance += StepM)
	{
		Out.Add(FVector2f(Distance, Lap.At(Distance).TimeS - Reference.At(Distance).TimeS));
	}
	return Out;
}

FString ApexLapTrace::FileName(const FApexLapTrace& Trace)
{
	return FString::Printf(TEXT("%s_lap%d_%s%s.csv"), *Trace.RecordedAt.ToString(TEXT("%Y%m%d-%H%M%S")), Trace.LapNumber,
		*LapTraceTime(Trace.LapMs, TEXT("-")), Trace.bValid ? TEXT("") : TEXT("_invalid"));
}

void FApexLapRecorder::Reset()
{
	*this = FApexLapRecorder();
}

bool FApexLapRecorder::Add(const FApexCarTelemetry& Car, FApexLapTrace& OutLap)
{
	bool bCompleted = false;
	if (Car.CurrentLap != CurrentLap)
	{
		// Crossed the line: the lap before is done, whole or not. A lap
		// counter running back (a new session) is no crossing.
		if (CurrentLap >= 0 && Car.CurrentLap == CurrentLap + 1 && bWhole && Car.LastLapTimeMs > 0
			&& Current.Samples.Num() >= 2)
		{
			Current.LapMs = Car.LastLapTimeMs;
			Current.bValid = !Car.bLastLapInvalid;
			Current.LapNumber = CurrentLap;
			Current.RecordedAt = FDateTime::Now();
			// The line itself: the last sample's time stretched to the lap's.
			FApexTraceSample End = Current.Samples.Last();
			if (End.TimeS < Current.LapMs / 1000.0f)
			{
				End.TimeS = Current.LapMs / 1000.0f;
				End.DistanceM += SMALL_NUMBER;
				Current.Samples.Add(End);
			}
			OutLap = MoveTemp(Current);
			bCompleted = true;
		}
		else if (CurrentLap >= 1 && bWhole)
		{
			UE_LOG(LogApexSim, Log, TEXT("Lap trace: lap %d not kept at the line: counter %d -> %d, last lap %d ms, %d samples"),
				CurrentLap, CurrentLap, Car.CurrentLap, Car.LastLapTimeMs, Current.Samples.Num());
		}
		Current = FApexLapTrace();
		CurrentLap = Car.CurrentLap;
		LastDistanceM = -1.0f;
		// Timed from the line: the counter moved as the car crossed it.
		bWhole = Car.CurrentLapTimeMs / 1000.0f <= LapTraceStartS && !Car.bInGarage;
	}
	if (!bWhole)
	{
		return bCompleted;
	}
	if (Car.bInGarage || Car.IsRecovering() || (Car.bInPitLane && Car.bPitServicing))
	{
		// Parked, towed or serviced: no lap to compare.
		UE_LOG(LogApexSim, Log, TEXT("Lap trace: lap %d broken at %.0f m: %s"), CurrentLap, Car.TrackProgress,
			Car.bInGarage ? TEXT("in the garage") : Car.IsRecovering() ? TEXT("recovering") : TEXT("serviced"));
		bWhole = false;
		return bCompleted;
	}
	const float Distance = Car.TrackProgress;
	if (Current.Samples.Num() == 0 && Distance > LapTraceStartM)
	{
		// The station may wrap a frame after the counter: wait for it, a
		// moment at most (an out-lap's clock starts at the pit exit and its
		// station never comes near the line).
		bWhole = Car.CurrentLapTimeMs / 1000.0f <= LapTraceStartS;
		return bCompleted;
	}
	if (LastDistanceM >= 0.0f)
	{
		if (Distance < LapTraceStartM && LastDistanceM > Distance + LapTraceHoleM)
		{
			// Wrapped ahead of the counter, at the line: the lap is done.
			return bCompleted;
		}
		if (FMath::Abs(Distance - LastDistanceM) > LapTraceHoleM)
		{
			UE_LOG(LogApexSim, Log, TEXT("Lap trace: lap %d broken by a jump from %.0f m to %.0f m"), CurrentLap, LastDistanceM, Distance);
			bWhole = false;
			return bCompleted;
		}
		if (Distance < LastDistanceM + ApexLapTrace::SampleSpacingM)
		{
			return bCompleted;
		}
	}
	FApexTraceSample S;
	S.DistanceM = Distance;
	S.TimeS = Car.CurrentLapTimeMs / 1000.0f;
	S.SpeedKph = Car.SpeedMps * 3.6f;
	S.Throttle = FMath::Clamp(Car.Throttle, 0.0f, 1.0f);
	S.Brake = FMath::Clamp(Car.Brake, 0.0f, 1.0f);
	S.Steering = Car.Steering;
	S.Gear = Car.Gear;
	S.Rpm = Car.EngineRpm;
	for (int32 Tyre = 0; Tyre < 4; ++Tyre)
	{
		S.TyreC[Tyre] = Car.TyreTempC[Tyre];
	}
	S.X = static_cast<float>(Car.Position.X);
	S.Y = static_cast<float>(Car.Position.Y);
	Current.Samples.Add(S);
	LastDistanceM = Distance;
	return bCompleted;
}

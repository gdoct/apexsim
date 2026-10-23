#include "ApexTestCommon.h"
#include "Race/ApexLapTrace.h"
#include "Race/ApexLapTraceSubsystem.h"
#include "Engine/GameInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexLapTraceTest
{
	/**
	 * A car round a 1000 m lap at SpeedMps, a frame every 1/60 s, from
	 * StartM on lap StartLap with the lap clock at StartLapS; the station
	 * wraps WrapEarlyFrames frames before the counter moves.
	 */
	struct FDrive
	{
		float SpeedMps = 50.0f;
		float LapM = 1000.0f;
		int32 Lap = 1;
		float Station = 0.0f;
		float LapClockS = 0.0f;
		int32 LastLapMs = 0;
		int32 WrapEarlyFrames = 0;
		int32 PendingWrap = -1;

		FApexCarTelemetry Step()
		{
			const float Dt = 1.0f / 60.0f;
			Station += SpeedMps * Dt;
			LapClockS += Dt;
			if (Station >= LapM && PendingWrap < 0)
			{
				PendingWrap = WrapEarlyFrames;
				Station -= LapM;
			}
			if (PendingWrap >= 0)
			{
				if (PendingWrap == 0)
				{
					LastLapMs = FMath::RoundToInt((LapClockS - Station / SpeedMps) * 1000.0f);
					LapClockS = Station / SpeedMps;
					++Lap;
				}
				--PendingWrap;
			}
			FApexCarTelemetry Car;
			Car.CarIndex = 0;
			Car.CurrentLap = Lap;
			Car.TrackProgress = Station;
			Car.CurrentLapTimeMs = FMath::RoundToInt(LapClockS * 1000.0f);
			Car.LastLapTimeMs = LastLapMs;
			Car.SpeedMps = SpeedMps;
			Car.Throttle = 1.0f;
			Car.Gear = 4;
			return Car;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLapTraceRecorderTest, "ApexSim.LapTrace.Recorder", ApexTestFlags)

bool FApexLapTraceRecorderTest::RunTest(const FString& Parameters)
{
	using namespace ApexLapTraceTest;
	// Joined part-way round lap 1: that lap is not whole; lap 2 is.
	FDrive Drive;
	Drive.Station = 400.0f;
	Drive.LapClockS = 8.0f;
	FApexLapRecorder Recorder;
	TArray<FApexLapTrace> Laps;
	for (int32 Frame = 0; Frame < 60 * 45; ++Frame)
	{
		FApexLapTrace Lap;
		if (Recorder.Add(Drive.Step(), Lap))
		{
			Laps.Add(Lap);
		}
	}
	// 45 s at 50 m/s from 400 m: lap 1 ends at 12 s, lap 2 at 32 s.
	TestEqual(TEXT("one whole lap"), Laps.Num(), 1);
	if (Laps.Num() == 1)
	{
		const FApexLapTrace& Lap = Laps[0];
		TestEqual(TEXT("it is lap 2"), Lap.LapNumber, 2);
		TestTrue(TEXT("its time is the server's"), FMath::Abs(Lap.LapMs - 20000) <= 20);
		TestTrue(TEXT("valid"), Lap.bValid);
		TestTrue(TEXT("sampled every couple of metres"), Lap.Samples.Num() >= 380 && Lap.Samples.Num() <= 520);
		TestEqual(TEXT("half way at 10 s"), Lap.At(500.0f).TimeS, 10.0f, 0.05f);
		TestEqual(TEXT("ends at the lap's time"), Lap.Samples.Last().TimeS, 20.0f, 0.05f);
	}

	// The station wrapping three frames before the counter keeps the lap.
	FDrive Early;
	Early.WrapEarlyFrames = 3;
	FApexLapRecorder Second;
	int32 Whole = 0;
	for (int32 Frame = 0; Frame < 60 * 41; ++Frame)
	{
		FApexLapTrace Lap;
		Whole += Second.Add(Early.Step(), Lap) ? 1 : 0;
	}
	TestEqual(TEXT("a wrap ahead of the counter loses no lap"), Whole, 2);

	// A recovery's jump breaks the lap it happens on.
	FDrive Jump;
	FApexLapRecorder Third;
	int32 Kept = 0;
	for (int32 Frame = 0; Frame < 60 * 21; ++Frame)
	{
		if (Frame == 300)
		{
			Jump.Station += 300.0f;
		}
		FApexLapTrace Lap;
		Kept += Third.Add(Jump.Step(), Lap) ? 1 : 0;
	}
	TestEqual(TEXT("a jump of 300 m keeps nothing"), Kept, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLapTraceCsvTest, "ApexSim.LapTrace.CsvAndDelta", ApexTestFlags)

bool FApexLapTraceCsvTest::RunTest(const FString& Parameters)
{
	FApexLapTrace Fast;
	Fast.TrackId = TEXT("Snorisring");
	Fast.CarId = TEXT("limbotiti-caravan-gt3");
	Fast.LapNumber = 3;
	Fast.LapMs = 20000;
	Fast.bValid = true;
	Fast.RecordedAt = FDateTime(2026, 10, 17, 18, 30, 5);
	FApexLapTrace Slow = Fast;
	Slow.LapMs = 21000;
	for (int32 Step = 0; Step <= 500; ++Step)
	{
		FApexTraceSample S;
		S.DistanceM = Step * 2.0f;
		S.TimeS = S.DistanceM / 50.0f;
		S.SpeedKph = 180.0f;
		S.Gear = 4;
		S.TyreC[0] = 85.0f;
		Fast.Samples.Add(S);
		// One second lost, all of it in the second half.
		S.TimeS += S.DistanceM > 500.0f ? (S.DistanceM - 500.0f) / 500.0f : 0.0f;
		Slow.Samples.Add(S);
	}

	FApexLapTrace Read;
	TestTrue(TEXT("the CSV reads back"), ApexLapTrace::FromCsv(ApexLapTrace::ToCsv(Fast), Read));
	TestEqual(TEXT("the car"), Read.CarId, Fast.CarId);
	TestEqual(TEXT("the time"), Read.LapMs, 20000);
	TestTrue(TEXT("valid"), Read.bValid);
	TestEqual(TEXT("the samples"), Read.Samples.Num(), Fast.Samples.Num());
	TestEqual(TEXT("a tyre"), Read.Samples[10].TyreC[0], 85.0f, 0.05f);
	TestTrue(TEXT("the date"), Read.RecordedAt == Fast.RecordedAt);
	TestFalse(TEXT("anything else is no trace"), ApexLapTrace::FromCsv(TEXT("a,b,c\n1,2,3\n"), Read));
	TestEqual(TEXT("the file name"), ApexLapTrace::FileName(Fast), FString(TEXT("20261017-183005_lap3_0-20-000.csv")));

	const TArray<FVector2f> Delta = ApexLapTrace::DeltaByDistance(Fast, Slow, 10.0f);
	TestEqual(TEXT("a point every 10 m"), Delta.Num(), 101);
	TestEqual(TEXT("level at half way"), Delta[50].Y, 0.0f, 0.01f);
	TestEqual(TEXT("a second down at the line"), Delta.Last().Y, 1.0f, 0.01f);
	TestEqual(TEXT("in between, half"), Delta[75].Y, 0.5f, 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLapTraceHeldLapsTest, "ApexSim.LapTrace.HeldLaps", ApexTestFlags)

bool FApexLapTraceHeldLapsTest::RunTest(const FString& Parameters)
{
	using namespace ApexLapTraceTest;
	// Three whole laps at 50, 55 and 45 m/s: the list is newest first and
	// knows which is the best (the second, at 55 m/s).
	UGameInstance* GameInstance = NewObject<UGameInstance>();
	UApexLapTraceSubsystem* Traces = NewObject<UApexLapTraceSubsystem>(GameInstance);
	FDrive Drive;
	const float Speeds[] = { 50.0f, 55.0f, 45.0f, 45.0f };
	for (int32 Frame = 0; Frame < 60 * 80 && Traces->GetLaps().Num() < 3; ++Frame)
	{
		Drive.SpeedMps = Speeds[FMath::Clamp(Drive.Lap - 1, 0, 3)];
		FApexTelemetryFrame Telemetry;
		Telemetry.Cars.Add(Drive.Step());
		Traces->AddFrameForTest(Telemetry, 0, TEXT("Test Ring"), TEXT("Test Car"));
	}
	const TArray<FApexLapTrace>& Laps = Traces->GetLaps();
	TestEqual(TEXT("three laps held"), Laps.Num(), 3);
	if (Laps.Num() == 3)
	{
		TestEqual(TEXT("newest first"), Laps[0].LapNumber, 3);
		TestEqual(TEXT("then the one before"), Laps[2].LapNumber, 1);
		TestEqual(TEXT("the best is lap 2"), Traces->GetBestIndex(), 1);
		TestEqual(TEXT("the last is the newest"), Traces->GetLast().LapNumber, Laps[0].LapNumber);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLapTraceRecordTest, "ApexSim.LapTrace.Record", ApexTestFlags)

bool FApexLapTraceRecordTest::RunTest(const FString& Parameters)
{
	// The player's lap: a 1000 m straight along X, a sample every 2 m.
	FApexLapTrace Own;
	for (int32 i = 0; i <= 500; ++i)
	{
		FApexTraceSample S;
		S.DistanceM = i * 2.0f;
		S.X = i * 2.0f;
		Own.Samples.Add(S);
	}
	// The record: the same road a metre and a half to the left, at 50 m/s,
	// 20 samples a second.
	FApexGhostLap Ghost;
	Ghost.LapTimeMs = 20000;
	for (int32 i = 0; i <= 400; ++i)
	{
		FApexGhostSample G;
		G.TimeMs = i * 50;
		G.Position = FVector(i * 2.5, 1.5, 0.0);
		G.SpeedMps = 50.0f;
		G.Gear = 5;
		Ghost.Samples.Add(G);
	}
	const FApexLapTrace Record = ApexLapTrace::FromGhost(Ghost, &Own);
	TestTrue(TEXT("a record trace"), Record.bRecord && Record.bValid && Record.LapMs == 20000);
	TestEqual(TEXT("a sample per ghost sample"), Record.Samples.Num(), Ghost.Samples.Num());
	TestTrue(TEXT("laid on the player's distances"), FMath::Abs(Record.At(500.0f).TimeS - 10.0f) < 0.1f);
	TestEqual(TEXT("no pedals"), Record.Samples[10].Throttle, -1.0f);
	bool bRising = true;
	for (int32 i = 1; i < Record.Samples.Num(); ++i)
	{
		bRising &= Record.Samples[i].DistanceM > Record.Samples[i - 1].DistanceM;
	}
	TestTrue(TEXT("distance only grows"), bRising);
	TestTrue(TEXT("speed in km/h"), FMath::IsNearlyEqual(Record.Samples[5].SpeedKph, 180.0f));

	// With no lap of the player's, its own path length.
	const FApexLapTrace Alone = ApexLapTrace::FromGhost(Ghost, nullptr);
	TestTrue(TEXT("path length alone"), FMath::IsNearlyEqual(Alone.LengthM(), 1000.0f, 0.5f));

	// Against a lap at a steady 40 m/s: the record gains a quarter of the time.
	FApexLapTrace Slow = Own;
	for (FApexTraceSample& S : Slow.Samples)
	{
		S.TimeS = S.DistanceM / 40.0f;
	}
	const TArray<FVector2f> Delta = ApexLapTrace::DeltaByDistance(Record, Slow, 100.0f);
	TestTrue(TEXT("the slow lap loses 5 s by the end"), Delta.Num() > 0 && FMath::Abs(Delta.Last().Y - 5.0f) < 0.2f);

	// In the held list: right after the best, and never the best itself.
	UGameInstance* GameInstance = NewObject<UGameInstance>();
	UApexLapTraceSubsystem* Traces = NewObject<UApexLapTraceSubsystem>(GameInstance);
	ApexLapTraceTest::FDrive Drive;
	for (int32 Frame = 0; Frame < 60 * 80 && Traces->GetLaps().Num() < 2; ++Frame)
	{
		FApexTelemetryFrame Telemetry;
		Telemetry.Cars.Add(Drive.Step());
		Traces->AddFrameForTest(Telemetry, 0, TEXT("Test Ring"), TEXT("Test Car"));
	}
	const int32 Own2 = Traces->GetLaps().Num();
	Traces->SetRecordLap(Ghost);
	const TArray<FApexLapTrace>& Laps = Traces->GetLaps();
	TestEqual(TEXT("the record joins the list"), Laps.Num(), Own2 + 1);
	TestTrue(TEXT("right after the best"), Laps.IsValidIndex(Traces->GetBestIndex() + 1) && Laps[Traces->GetBestIndex() + 1].bRecord);
	TestFalse(TEXT("never the best"), Traces->GetBestIndex() == Laps.Num() - 1);
	Traces->SetRecordLap(Ghost);
	TestEqual(TEXT("once"), Traces->GetLaps().Num(), Own2 + 1);
	return true;
}

#endif

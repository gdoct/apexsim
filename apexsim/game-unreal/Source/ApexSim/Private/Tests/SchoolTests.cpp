#include "ApexTestCommon.h"
#include "Guide/ApexTrackGuide.h"
#include "School/ApexSchool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexSchoolTest
{
	constexpr float LapM = 4000.0f;

	FApexGuideCorner Corner(int32 Number, float EntryM, float ApexM, float ExitM, float EntryKph, float MinKph, float ExitKph, float BrakeM)
	{
		FApexGuideCorner C;
		C.Number = Number;
		C.Name = FString::Printf(TEXT("Turn %d"), Number);
		C.EntryM = EntryM;
		C.ApexM = ApexM;
		C.ExitM = ExitM;
		C.EntrySpeedKph = EntryKph;
		C.MinSpeedKph = MinKph;
		C.ExitSpeedKph = ExitKph;
		C.BrakeM = BrakeM;
		C.bHasBrake = BrakeM > 0.0f;
		C.bFlatOut = BrakeM <= 0.0f;
		return C;
	}

	FApexTrackGuide Guide()
	{
		FApexTrackGuide G;
		G.Track.LengthM = LapM;
		G.Track.LapTimeS = 90.0f;
		// Out of order on purpose; a flat kink; a hairpin (the biggest stop and
		// the slowest); a long esses; a corner across the line (never used).
		G.Corners.Add(Corner(3, 2500.0f, 2560.0f, 2800.0f, 230.0f, 140.0f, 180.0f, 90.0f));
		G.Corners.Add(Corner(1, 400.0f, 420.0f, 450.0f, 250.0f, 245.0f, 250.0f, 0.0f));
		G.Corners.Add(Corner(2, 1200.0f, 1230.0f, 1280.0f, 280.0f, 70.0f, 120.0f, 180.0f));
		G.Corners.Add(Corner(4, 3950.0f, 3990.0f, 60.0f, 200.0f, 100.0f, 150.0f, 80.0f));
		return G;
	}

	/** A car driven from `FromM` at `Speed(station travelled)` m/s, sampled at 60 Hz until `Until` says stop or `MaxS` passes. */
	struct FDrive
	{
		double TimeS = 100.0;
		float StationM = 0.0f;
		float TravelledM = 0.0f;
		int32 Lap = 1;
		bool bLapInvalid = false;
		int32 LastLapMs = 0;
		bool bLastLapInvalid = false;

		ApexSchool::FSample Sample(float SpeedMps) const
		{
			ApexSchool::FSample S;
			S.TimeS = TimeS;
			S.StationM = StationM;
			S.SpeedMps = SpeedMps;
			S.Lap = Lap;
			S.bLapInvalid = bLapInvalid;
			S.LastLapMs = LastLapMs;
			S.bLastLapInvalid = bLastLapInvalid;
			return S;
		}

		/** Step 1/60 s at `SpeedMps`; the line rolls the lap over. */
		void Step(float SpeedMps)
		{
			constexpr double Dt = 1.0 / 60.0;
			TimeS += Dt;
			const float Move = SpeedMps * static_cast<float>(Dt);
			TravelledM += Move;
			StationM += Move;
			if (StationM >= LapM)
			{
				StationM -= LapM;
				++Lap;
				bLastLapInvalid = bLapInvalid;
				bLapInvalid = false;
			}
		}
	};

	/** Feed `Drive` to `Judge` with `Speed(drive)` until the judge ends or `MaxS` passes. */
	bool Run(ApexSchool::FJudge& Judge, FDrive& Drive, TFunctionRef<float(const FDrive&)> Speed, double MaxS = 400.0)
	{
		const double Until = Drive.TimeS + MaxS;
		while (Drive.TimeS < Until)
		{
			const float V = Speed(Drive);
			if (Judge.Feed(Drive.Sample(V)))
			{
				return true;
			}
			Drive.Step(V);
		}
		return false;
	}
}

// The tests a guide makes: the biggest stop for the brake test (its board at
// the guide car's braking point, its line where that deceleration from there
// stops it, with slack), the slowest corner for the exit, the longest other
// braked corner for the corner time (no reference until measured), the lap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSchoolBuildTest, "ApexSim.School.Build", ApexTestFlags)

bool FApexSchoolBuildTest::RunTest(const FString& Parameters)
{
	using namespace ApexSchoolTest;
	const TArray<ApexSchool::FTest> Tests = ApexSchool::BuildTests(Guide());
	if (!TestEqual(TEXT("four tests"), Tests.Num(), 4))
	{
		return false;
	}
	const ApexSchool::FTest& Brake = Tests[0];
	TestEqual(TEXT("brake id"), Brake.Id, FString(TEXT("brake")));
	TestEqual(TEXT("brake at the hairpin"), Brake.Place, FString(TEXT("Turn 2")));
	TestEqual(TEXT("board at the braking point"), Brake.FromM, 1230.0f - 180.0f);
	const float Ve = 280.0f / 3.6f;
	const float Vm = 70.0f / 3.6f;
	const float Decel = (Ve * Ve - Vm * Vm) / (2.0f * 180.0f);
	TestEqual(TEXT("line where it stops"), Brake.ToM, 1050.0f + Ve * Ve / (2.0f * Decel) * ApexSchool::Rules::StopSlack, 0.01f);
	TestEqual(TEXT("least speed at the board"), Brake.MinSpeedMps, Ve * ApexSchool::Rules::BoardSpeedShare, 0.001f);
	TestEqual(TEXT("gold box"), Brake.Limits[2], 3.0f);

	const ApexSchool::FTest& Exit = Tests[1];
	TestEqual(TEXT("exit id"), Exit.Id, FString(TEXT("exit")));
	TestEqual(TEXT("exit at the slowest"), Exit.Place, FString(TEXT("Turn 2")));
	TestEqual(TEXT("exit gold"), Exit.Limits[2], 120.0f / 3.6f * 0.98f, 0.001f);

	const ApexSchool::FTest& Corner = Tests[2];
	TestEqual(TEXT("corner id"), Corner.Id, FString(TEXT("corner")));
	TestEqual(TEXT("the longest other braked corner"), Corner.Place, FString(TEXT("Turn 3")));
	TestFalse(TEXT("not measured yet"), Corner.IsReady());
	ApexSchool::FTest Measured = Corner;
	ApexSchool::SetReference(Measured, 6.0f);
	TestEqual(TEXT("corner gold"), Measured.Limits[2], 6.12f, 0.001f);
	TestEqual(TEXT("corner bronze"), Measured.Limits[0], 6.9f, 0.001f);

	const ApexSchool::FTest& Lap = Tests[3];
	TestEqual(TEXT("lap"), Lap.Id, FString(TEXT("lap")));
	TestEqual(TEXT("lap silver"), Lap.Limits[1], 94.5f, 0.001f);

	TestEqual(TEXT("brake 2 m long is gold"), ApexSchool::MedalFor(Brake, 2.0f), EApexMedal::Gold);
	TestEqual(TEXT("brake 5 m short is silver"), ApexSchool::MedalFor(Brake, -5.0f), EApexMedal::Silver);
	TestEqual(TEXT("brake 20 m is nothing"), ApexSchool::MedalFor(Brake, 20.0f), EApexMedal::None);
	TestEqual(TEXT("exit just bronze"), ApexSchool::MedalFor(Exit, 120.0f / 3.6f * 0.86f), EApexMedal::Bronze);
	TestEqual(TEXT("lap 91 s is gold"), ApexSchool::MedalFor(Lap, 91.0f), EApexMedal::Gold);
	TestEqual(TEXT("lap 0 s is nothing"), ApexSchool::MedalFor(Lap, 0.0f), EApexMedal::None);

	// A guide with no braked corner has only the lap.
	FApexTrackGuide Flat = Guide();
	Flat.Corners.SetNum(1);
	Flat.Corners[0] = ApexSchoolTest::Corner(1, 400.0f, 420.0f, 450.0f, 250.0f, 245.0f, 250.0f, 0.0f);
	TestEqual(TEXT("only the lap"), ApexSchool::BuildTests(Flat).Num(), 1);
	return true;
}

// The brake test judged: a stop inside the gold box, a car too slow at the
// board, one that runs through, one parked in the garage half way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSchoolBrakeJudgeTest, "ApexSim.School.JudgeBrake", ApexTestFlags)

bool FApexSchoolBrakeJudgeTest::RunTest(const FString& Parameters)
{
	using namespace ApexSchoolTest;
	const ApexSchool::FTest Brake = ApexSchool::BuildTests(Guide())[0];
	const float ToLine = Brake.ToM - Brake.FromM;

	// Board at 75 m/s, then a constant deceleration that stops 2 m past the line.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Brake, LapM);
		FDrive Drive;
		Drive.StationM = Brake.FromM - 400.0f;
		const float V0 = 75.0f;
		const float Decel = V0 * V0 / (2.0f * (ToLine + 2.0f));
		const float BoardTravel = 400.0f;
		const bool bEnded = Run(Judge, Drive, [&](const FDrive& D)
		{
			const float Past = D.TravelledM - BoardTravel;
			return Past <= 0.0f ? V0 : FMath::Sqrt(FMath::Max(0.0f, V0 * V0 - 2.0f * Decel * Past));
		});
		TestTrue(TEXT("judged"), bEnded && Judge.GetPhase() == ApexSchool::EJudgePhase::Done);
		TestTrue(TEXT("passed"), Judge.GetResult().bPassed);
		TestEqual(TEXT("gold"), Judge.GetResult().Medal, EApexMedal::Gold);
		TestEqual(TEXT("about 2 m past"), Judge.GetResult().Value, 2.0f, 1.0f);
		TestEqual(TEXT("board speed"), Judge.GetResult().SpeedMps, V0, 0.01f);
	}

	// Too slow at the board.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Brake, LapM);
		FDrive Drive;
		Drive.StationM = Brake.FromM - 100.0f;
		Run(Judge, Drive, [&](const FDrive&) { return Brake.MinSpeedMps * 0.8f; });
		TestEqual(TEXT("slow: done"), Judge.GetPhase(), ApexSchool::EJudgePhase::Done);
		TestFalse(TEXT("slow: failed"), Judge.GetResult().bPassed);
		TestTrue(TEXT("slow: why"), Judge.GetResult().Why.StartsWith(TEXT("Too slow at the board")));
	}

	// Through the box without stopping (what the AI stand-in does).
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Brake, LapM);
		FDrive Drive;
		Drive.StationM = Brake.FromM - 100.0f;
		Run(Judge, Drive, [&](const FDrive&) { return 75.0f; });
		TestFalse(TEXT("through: failed"), Judge.GetResult().bPassed);
		TestTrue(TEXT("through: why"), Judge.GetResult().Why.StartsWith(TEXT("Ran through the box")));
	}

	// Back to the garage after the board: given up, not judged.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Brake, LapM);
		FDrive Drive;
		Drive.StationM = Brake.FromM - 50.0f;
		for (int32 Step = 0; Step < 120; ++Step)
		{
			Judge.Feed(Drive.Sample(75.0f));
			Drive.Step(75.0f);
		}
		TestEqual(TEXT("garage: running"), Judge.GetPhase(), ApexSchool::EJudgePhase::Running);
		ApexSchool::FSample Parked = Drive.Sample(0.0f);
		Parked.bInGarage = true;
		TestTrue(TEXT("garage: ends"), Judge.Feed(Parked));
		TestEqual(TEXT("garage: aborted"), Judge.GetPhase(), ApexSchool::EJudgePhase::Aborted);
	}
	return true;
}

// The corner tests and the lap judged: through the corner timed by its two
// crossings, a struck lap failing it; the lap from the run-up before the
// line timed by the server's lap time, a struck lap failing it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSchoolCornerLapJudgeTest, "ApexSim.School.JudgeCornerLap", ApexTestFlags)

bool FApexSchoolCornerLapJudgeTest::RunTest(const FString& Parameters)
{
	using namespace ApexSchoolTest;
	const TArray<ApexSchool::FTest> Tests = ApexSchool::BuildTests(Guide());
	ApexSchool::FTest Corner = Tests[2];
	ApexSchool::SetReference(Corner, 15.0f);
	const ApexSchool::FTest Exit = Tests[1];
	const ApexSchool::FTest Lap = Tests[3];

	// 300 m of corner at 20 m/s: 15 s, gold; out at 20 m/s.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Corner, LapM);
		FDrive Drive;
		// Garage samples first: no crossing from where it stood.
		ApexSchool::FSample Garage = Drive.Sample(0.0f);
		Garage.bInGarage = true;
		Judge.Feed(Garage);
		Drive.StationM = Corner.FromM - 100.0f;
		Run(Judge, Drive, [](const FDrive&) { return 20.0f; });
		TestEqual(TEXT("corner: done"), Judge.GetPhase(), ApexSchool::EJudgePhase::Done);
		TestEqual(TEXT("corner: 15 s"), Judge.GetResult().Seconds, 15.0f, 0.02f);
		TestEqual(TEXT("corner: gold"), Judge.GetResult().Medal, EApexMedal::Gold);
		TestEqual(TEXT("corner: exit speed"), Judge.GetResult().SpeedMps, 20.0f, 0.01f);
	}

	// The exit test: out at the guide car's exit speed is gold, half of it fails.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Exit, LapM);
		FDrive Drive;
		Drive.StationM = Exit.FromM - 50.0f;
		Run(Judge, Drive, [&](const FDrive&) { return Exit.Reference; });
		TestEqual(TEXT("exit: gold"), Judge.GetResult().Medal, EApexMedal::Gold);

		Judge.Begin(Exit, LapM);
		FDrive Slow;
		Slow.StationM = Exit.FromM - 50.0f;
		Run(Judge, Slow, [&](const FDrive&) { return Exit.Reference * 0.5f; });
		TestFalse(TEXT("exit slow: failed"), Judge.GetResult().bPassed);
	}

	// Off the track in the corner.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Corner, LapM);
		FDrive Drive;
		Drive.StationM = Corner.FromM - 20.0f;
		Run(Judge, Drive, [&](const FDrive& D)
		{
			const_cast<FDrive&>(D).bLapInvalid = D.StationM > Corner.FromM + 100.0f;
			return 20.0f;
		});
		TestFalse(TEXT("off: failed"), Judge.GetResult().bPassed);
		TestTrue(TEXT("off: why"), Judge.GetResult().Why.StartsWith(TEXT("Off the track")));
	}

	// The lap: 300 m of run-up, then a lap the server times at 91.2 s.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Lap, LapM);
		FDrive Drive;
		Drive.StationM = LapM - 300.0f;
		Drive.Lap = 0;
		const bool bEnded = Run(Judge, Drive, [](const FDrive& D)
		{
			// The server's last lap shows once the lap ticks over.
			const_cast<FDrive&>(D).LastLapMs = D.Lap >= 2 ? 91200 : 0;
			return 50.0f;
		});
		TestTrue(TEXT("lap: judged"), bEnded);
		TestEqual(TEXT("lap: the server's time"), Judge.GetResult().Value, 91.2f, 0.001f);
		TestEqual(TEXT("lap: gold (91.8 s or less)"), Judge.GetResult().Medal, EApexMedal::Gold);
	}

	// A struck lap.
	{
		ApexSchool::FJudge Judge;
		Judge.Begin(Lap, LapM);
		FDrive Drive;
		Drive.StationM = LapM - 300.0f;
		Drive.Lap = 0;
		Run(Judge, Drive, [](const FDrive& D)
		{
			const_cast<FDrive&>(D).bLapInvalid = D.Lap == 1 && D.StationM > 2000.0f;
			return 50.0f;
		});
		TestFalse(TEXT("struck: failed"), Judge.GetResult().bPassed);
		TestTrue(TEXT("struck: why"), Judge.GetResult().Why.StartsWith(TEXT("Off the track")));
	}
	return true;
}

// The profile's medals: tries counted, the best kept by medal then figure;
// the cards' texts; where a station lies on the built centerline.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSchoolMedalsTest, "ApexSim.School.Medals", ApexTestFlags)

bool FApexSchoolMedalsTest::RunTest(const FString& Parameters)
{
	using namespace ApexSchoolTest;
	const ApexSchool::FTest Brake = ApexSchool::BuildTests(Guide())[0];
	const FString Key = ApexSchool::MedalKey(TEXT("track"), TEXT("car"), Brake.Id);
	TestEqual(TEXT("key"), Key, FString(TEXT("track|car|brake")));

	TArray<FApexSchoolMedal> Medals;
	ApexSchool::FResult Failed;
	TestFalse(TEXT("a failed try is no best"), ApexSchool::FileResult(Medals, Key, Brake, Failed));
	ApexSchool::FResult Silver;
	Silver.bPassed = true;
	Silver.Medal = EApexMedal::Silver;
	Silver.Value = -6.0f;
	TestTrue(TEXT("silver is a best"), ApexSchool::FileResult(Medals, Key, Brake, Silver));
	ApexSchool::FResult BetterSilver = Silver;
	BetterSilver.Value = 4.0f;
	TestTrue(TEXT("closer at silver is a best"), ApexSchool::FileResult(Medals, Key, Brake, BetterSilver));
	TestFalse(TEXT("further at silver is not"), ApexSchool::FileResult(Medals, Key, Brake, Silver));
	ApexSchool::FResult Bronze = Silver;
	Bronze.Medal = EApexMedal::Bronze;
	Bronze.Value = 0.5f;
	TestFalse(TEXT("a lower medal is not"), ApexSchool::FileResult(Medals, Key, Brake, Bronze));
	const FApexSchoolMedal* Entry = ApexSchool::FindMedal(Medals, Key);
	if (TestNotNull(TEXT("entry"), Entry))
	{
		TestEqual(TEXT("one entry"), Medals.Num(), 1);
		TestEqual(TEXT("tries"), Entry->Tries, 5);
		TestEqual(TEXT("medal"), Entry->Medal, EApexMedal::Silver);
		TestEqual(TEXT("best"), Entry->Best, 4.0f);
	}

	TestEqual(TEXT("figure past"), ApexSchool::FigureText(Brake, 1.84f, true), FString(TEXT("1.8 m past the line")));
	TestEqual(TEXT("figure short"), ApexSchool::FigureText(Brake, -3.0f, true), FString(TEXT("3.0 m short of the line")));
	TestEqual(TEXT("target"), ApexSchool::TargetText(Brake, EApexMedal::Gold, true), FString(TEXT("within 3.0 m of the line")));
	const ApexSchool::FTest Lap = ApexSchool::BuildTests(Guide())[3];
	TestEqual(TEXT("lap target"), ApexSchool::TargetText(Lap, EApexMedal::Gold, true), FString(TEXT("1:31.800 or less")));
	TestEqual(TEXT("medal name"), ApexSchool::MedalName(EApexMedal::Bronze), FString(TEXT("BRONZE")));
	TestTrue(TEXT("goal names the corner"), ApexSchool::GoalText(Brake, true).Contains(TEXT("Turn 2")));

	// A 100 m square loop (cm), counter-clockwise from the origin.
	const TArray<FVector> Square = { FVector(0, 0, 10), FVector(10000, 0, 10), FVector(10000, 10000, 20), FVector(0, 10000, 20) };
	FVector At;
	FVector Forward;
	TestTrue(TEXT("pose"), ApexSchool::PoseAtStation(Square, 150.0f, At, Forward));
	TestTrue(TEXT("150 m: half way up the second side"), At.Equals(FVector(10000, 5000, 15), 1.0));
	TestTrue(TEXT("150 m: heading +Y"), Forward.Equals(FVector(0, 1, 0), 0.001));
	TestTrue(TEXT("wraps"), ApexSchool::PoseAtStation(Square, 450.0f, At, Forward) && At.Equals(FVector(5000, 0, 10), 1.0));
	TestTrue(TEXT("closing side"), ApexSchool::PoseAtStation(Square, 350.0f, At, Forward) && At.Equals(FVector(0, 5000, 15), 1.0));
	TestFalse(TEXT("one point is no line"), ApexSchool::PoseAtStation({ FVector::ZeroVector }, 1.0f, At, Forward));
	return true;
}

#endif

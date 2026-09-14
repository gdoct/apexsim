#include "ApexTestCommon.h"
#include "Race/ApexCarMotion.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace ApexMotion;

	constexpr double TickRate = 240.0;
	/** The server's default telemetry divisor: a frame every 4 ticks. */
	constexpr int64 Spacing = 4;
	constexpr double FrameSeconds = Spacing / TickRate;

	/** A car driving +X at a steady speed, sampled at frame `n`. */
	FSnapshot StraightAt(int64 Frame, double SpeedCmPerSec, double YawDeg = 0.0)
	{
		FSnapshot S;
		S.Tick = Frame * Spacing;
		S.Location = FVector(SpeedCmPerSec * Frame * FrameSeconds, 0.0, 0.0);
		S.Rotation = FRotator(0.0, YawDeg, 0.0).Quaternion();
		S.SpeedMps = static_cast<float>(SpeedCmPerSec / 100.0);
		S.Steering = 0.0f;
		S.EngineRpm = 6000.0f;
		return S;
	}

	/**
	 * Runs the buffer on a stream of frames delivered at wall-clock times,
	 * sampling the pose at a fixed render rate, and returns the per-render-
	 * frame distance moved once the buffer has settled.
	 */
	struct FRun
	{
		FApexCarMotionBuffer Buffer;
		FSettings Settings;
		double Now = 0.0;
		TArray<FPose> Poses;
		TArray<double> Steps;
		int32 Extrapolated = 0;

		void Render(double Dt)
		{
			Now += Dt;
			FPose Pose;
			if (Buffer.Sample(Dt, Settings, Pose))
			{
				if (Poses.Num() > 0)
				{
					Steps.Add(FVector::Dist(Pose.Location, Poses.Last().Location));
				}
				Poses.Add(Pose);
				Extrapolated += Pose.bExtrapolated ? 1 : 0;
			}
		}
	};
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionSteadyTest,
	"ApexSim.Motion.SteadyStreamIsSmooth",
	ApexTestFlags)

bool FApexCarMotionSteadyTest::RunTest(const FString& Parameters)
{
	// 60Hz telemetry rendered at 144Hz: the frames and the renders never
	// line up, and some render frames get two samples while others get none.
	const double Speed = 8000.0; // 80 m/s
	const double RenderDt = 1.0 / 144.0;
	FRun Run;

	int64 Frame = 0;
	double NextFrameAt = 0.0;
	for (int32 i = 0; i < 144 * 3; ++i)
	{
		while (NextFrameAt <= Run.Now)
		{
			Run.Buffer.Push(StraightAt(Frame, Speed), Run.Now, Run.Settings);
			++Frame;
			NextFrameAt += FrameSeconds;
		}
		Run.Render(RenderDt);
	}

	TestTrue(TEXT("tick rate measured"), FMath::IsNearlyEqual(Run.Buffer.GetTicksPerSecond(), TickRate, 3.0));
	TestEqual(TEXT("frame spacing observed"), Run.Buffer.GetFrameSpacingTicks(), Spacing);

	// After the first second the step per render frame must be the car's
	// speed at the render rate, every frame: no pumping and no stutter.
	const double Expected = Speed * RenderDt;
	double MaxDeviation = 0.0;
	for (int32 i = 144; i < Run.Steps.Num(); ++i)
	{
		MaxDeviation = FMath::Max(MaxDeviation, FMath::Abs(Run.Steps[i] - Expected) / Expected);
	}
	TestTrue(FString::Printf(TEXT("steps within 5%% of steady (worst %.1f%%)"), MaxDeviation * 100.0), MaxDeviation < 0.05);
	TestEqual(TEXT("a steady stream never extrapolates"), Run.Extrapolated, 0);

	// The pose sits behind the newest sample by about the configured delay,
	// not by a distance that grows with speed.
	const double LagMs = Run.Buffer.GetLagTicks() / TickRate * 1000.0;
	TestTrue(FString::Printf(TEXT("lag near two frames (%.1f ms)"), LagMs), LagMs > 20.0 && LagMs < 50.0);
	TestTrue(TEXT("velocity reported"), FMath::IsNearlyEqual(Run.Poses.Last().Velocity.X, Speed, Speed * 0.02));

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionJitterTest,
	"ApexSim.Motion.LumpyArrivalsStaySmooth",
	ApexTestFlags)

bool FApexCarMotionJitterTest::RunTest(const FString& Parameters)
{
	// Frames arrive in pairs every other interval, as a client whose tick
	// runs a hair slower than the broadcast sees them, with 6 ms of jitter
	// on top. The ticks in the frames are still exact.
	const double Speed = 5000.0;
	const double RenderDt = 1.0 / 60.0;
	FRun Run;

	int64 Frame = 0;
	for (int32 i = 0; i < 60 * 4; ++i)
	{
		if (i % 2 == 0)
		{
			const double Jitter = ((i / 2) % 3 - 1) * 0.006;
			Run.Buffer.Push(StraightAt(Frame++, Speed), Run.Now + Jitter, Run.Settings);
			Run.Buffer.Push(StraightAt(Frame++, Speed), Run.Now + Jitter, Run.Settings);
		}
		Run.Render(RenderDt);
	}

	const double Expected = Speed * RenderDt;
	double MaxDeviation = 0.0;
	for (int32 i = 60; i < Run.Steps.Num(); ++i)
	{
		MaxDeviation = FMath::Max(MaxDeviation, FMath::Abs(Run.Steps[i] - Expected) / Expected);
	}
	TestTrue(FString::Printf(TEXT("steps within 5%% despite lumps (worst %.1f%%)"), MaxDeviation * 100.0), MaxDeviation < 0.05);
	TestEqual(TEXT("no extrapolation with two frames of delay"), Run.Extrapolated, 0);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionGapTest,
	"ApexSim.Motion.LostFramesAreDeadReckoned",
	ApexTestFlags)

bool FApexCarMotionGapTest::RunTest(const FString& Parameters)
{
	const double Speed = 6000.0;
	const double RenderDt = 1.0 / 120.0;
	FRun Run;

	// A second of clean stream, then eight frames lost (133 ms), then it resumes.
	int64 Frame = 0;
	double NextFrameAt = 0.0;
	int32 ExtrapolatedBeforeGap = 0;
	for (int32 i = 0; i < 120 * 3; ++i)
	{
		while (NextFrameAt <= Run.Now)
		{
			const bool bLost = Frame >= 60 && Frame < 68;
			if (!bLost)
			{
				Run.Buffer.Push(StraightAt(Frame, Speed), Run.Now, Run.Settings);
			}
			++Frame;
			NextFrameAt += FrameSeconds;
		}
		Run.Render(RenderDt);
		if (i == 110)
		{
			ExtrapolatedBeforeGap = Run.Extrapolated;
		}
	}

	TestEqual(TEXT("clean stream did not extrapolate"), ExtrapolatedBeforeGap, 0);
	TestTrue(TEXT("the gap was covered by dead reckoning"), Run.Extrapolated > 0);

	// The car never stops or jumps: every render step stays near the true
	// motion, through the gap and through the resync afterwards.
	const double Expected = Speed * RenderDt;
	double MaxDeviation = 0.0;
	for (int32 i = 60; i < Run.Steps.Num(); ++i)
	{
		MaxDeviation = FMath::Max(MaxDeviation, FMath::Abs(Run.Steps[i] - Expected) / Expected);
	}
	TestTrue(FString::Printf(TEXT("steps within 15%% across the gap (worst %.1f%%)"), MaxDeviation * 100.0), MaxDeviation < 0.15);
	TestTrue(TEXT("spacing estimate unaffected by the gap"), Run.Buffer.GetFrameSpacingTicks() == Spacing);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionStallTest,
	"ApexSim.Motion.StallHoldsThenResyncs",
	ApexTestFlags)

bool FApexCarMotionStallTest::RunTest(const FString& Parameters)
{
	const double Speed = 6000.0;
	FRun Run;
	Run.Settings.MaxExtrapolationSeconds = 0.25f;

	int64 Frame = 0;
	for (; Frame < 60; ++Frame)
	{
		Run.Buffer.Push(StraightAt(Frame, Speed), Frame * FrameSeconds, Run.Settings);
		Run.Render(FrameSeconds);
	}
	const FVector LastLive = Run.Poses.Last().Location;

	// Two seconds of silence: the car runs on for the limit and then holds.
	for (int32 i = 0; i < 120; ++i)
	{
		Run.Render(FrameSeconds);
	}
	const FVector Held = Run.Poses.Last().Location;
	const double RanOn = (Held - LastLive).X;
	TestTrue(FString::Printf(TEXT("ran on about the extrapolation limit (%.0f cm)"), RanOn),
		RanOn > Speed * 0.2 && RanOn < Speed * 0.35);
	TestTrue(TEXT("held still afterwards"), Run.Steps.Last() < 1.0);

	// The stream resumes where the server actually is: two seconds further
	// along. Creeping there would take seconds, so the playhead re-seats.
	Frame += 120;
	FPose Pose;
	for (int32 i = 0; i < 4; ++i, ++Frame)
	{
		Run.Buffer.Push(StraightAt(Frame, Speed), Frame * FrameSeconds, Run.Settings);
		Run.Buffer.Sample(FrameSeconds, Run.Settings, Pose);
	}
	const FVector TruePos = StraightAt(Frame - 1, Speed).Location;
	TestTrue(TEXT("re-seated on the live stream"), FVector::Dist(Pose.Location, TruePos) < Speed * 0.1);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionTeleportTest,
	"ApexSim.Motion.TeleportRestarts",
	ApexTestFlags)

bool FApexCarMotionTeleportTest::RunTest(const FString& Parameters)
{
	FRun Run;
	FPose Pose;

	// A session that has been running a while: ticks in the thousands.
	auto At = [](int64 Frame)
	{
		FSnapshot S = StraightAt(Frame, 3000.0);
		S.Tick += 100000;
		return S;
	};
	TestTrue(TEXT("first sample restarts"), Run.Buffer.Push(At(0), 0.0, Run.Settings) == EPushResult::Restarted);
	TestTrue(TEXT("sits on the only sample"), Run.Buffer.Sample(0.01, Run.Settings, Pose) && Pose.Location.IsZero());

	TestTrue(TEXT("second sample adds"), Run.Buffer.Push(At(1), FrameSeconds, Run.Settings) == EPushResult::Added);
	TestTrue(TEXT("older tick ignored"), Run.Buffer.Push(At(0), FrameSeconds, Run.Settings) == EPushResult::Ignored);
	TestTrue(TEXT("same tick ignored"), Run.Buffer.Push(At(1), FrameSeconds, Run.Settings) == EPushResult::Ignored);

	// A respawn across the map.
	FSnapshot Far = At(2);
	Far.Location = FVector(500000.0, 20000.0, 0.0);
	TestTrue(TEXT("jump restarts"), Run.Buffer.Push(Far, 2 * FrameSeconds, Run.Settings) == EPushResult::Restarted);
	TestEqual(TEXT("holds only the new place"), Run.Buffer.Num(), 1);
	Run.Buffer.Sample(0.01, Run.Settings, Pose);
	TestTrue(TEXT("shown at the new place at once"), Pose.Location.Equals(Far.Location));
	TestTrue(TEXT("rate estimate kept across a teleport"), Run.Buffer.GetFrameSpacingTicks() == Spacing);

	// A new session: the tick runs back to the start. A step back of a few
	// ticks is a reordered packet and is dropped; a second or more of ticks
	// is another clock.
	FSnapshot Reordered = Far;
	Reordered.Tick = Far.Tick - Spacing;
	TestTrue(TEXT("a step back is a reordered packet"), Run.Buffer.Push(Reordered, 3 * FrameSeconds, Run.Settings) == EPushResult::Ignored);
	FSnapshot Fresh = StraightAt(0, 0.0);
	Fresh.Tick = Far.Tick - static_cast<int64>(TickRate * 2.0);
	TestTrue(TEXT("tick far backwards restarts"), Run.Buffer.Push(Fresh, 3 * FrameSeconds, Run.Settings) == EPushResult::Restarted);
	TestEqual(TEXT("spacing forgotten with the clock"), Run.Buffer.GetFrameSpacingTicks(), static_cast<int64>(0));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionYawSeamTest,
	"ApexSim.Motion.YawBlendsTheShortWay",
	ApexTestFlags)

bool FApexCarMotionYawSeamTest::RunTest(const FString& Parameters)
{
	// Heading crosses the +-180 seam between two samples: the blend must
	// turn through the few degrees between them, not spin the long way.
	const FSnapshot A = StraightAt(0, 1000.0, 176.0);
	const FSnapshot B = StraightAt(1, 1000.0, -176.0);
	const FPose Mid = Blend(A, B, 0.5, FrameSeconds);
	const double MidYaw = Mid.Rotation.Rotator().Yaw;
	TestTrue(FString::Printf(TEXT("midpoint yaw on the seam (%.1f)"), MidYaw), FMath::Abs(FMath::Abs(MidYaw) - 180.0) < 0.5);
	TestTrue(TEXT("turned only a few degrees"), FMath::RadiansToDegrees(A.Rotation.AngularDistance(Mid.Rotation)) < 5.0);

	// Dead reckoning carries the turn on at the same rate.
	const FPose Ahead = Extrapolate(A, B, FrameSeconds, FrameSeconds);
	TestTrue(TEXT("extrapolated another 8 degrees"),
		FMath::IsNearlyEqual(FMath::RadiansToDegrees(B.Rotation.AngularDistance(Ahead.Rotation)), 8.0, 0.5));
	TestTrue(TEXT("extrapolated the position"), FMath::IsNearlyEqual(Ahead.Location.X, B.Location.X + 1000.0 * FrameSeconds, 0.01));
	TestTrue(TEXT("flagged"), Ahead.bExtrapolated);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCarMotionRateTest,
	"ApexSim.Motion.TickRateIsMeasured",
	ApexTestFlags)

bool FApexCarMotionRateTest::RunTest(const FString& Parameters)
{
	// A server on 120Hz ticks with a divisor of 2 still broadcasts at 60Hz;
	// the buffer must learn the rate rather than assume 240.
	FRun Run;
	const double Rate = 120.0;
	const int64 Div = 2;
	for (int64 Frame = 0; Frame < 120; ++Frame)
	{
		FSnapshot S;
		S.Tick = Frame * Div;
		S.Location = FVector(4000.0 * Frame / 60.0, 0.0, 0.0);
		Run.Buffer.Push(S, Frame / 60.0, Run.Settings);
		Run.Render(1.0 / 60.0);
	}
	TestTrue(FString::Printf(TEXT("rate learned (%.1f)"), Run.Buffer.GetTicksPerSecond()),
		FMath::IsNearlyEqual(Run.Buffer.GetTicksPerSecond(), Rate, 2.0));
	TestEqual(TEXT("divisor learned"), Run.Buffer.GetFrameSpacingTicks(), Div);

	const double Expected = 4000.0 / 60.0;
	double MaxDeviation = 0.0;
	int32 Worst = -1;
	for (int32 i = 60; i < Run.Steps.Num(); ++i)
	{
		const double Dev = FMath::Abs(Run.Steps[i] - Expected) / Expected;
		if (Dev > MaxDeviation) { MaxDeviation = Dev; Worst = i; }
	}
	TestTrue(FString::Printf(TEXT("smooth at the learned rate (worst %.1f%% at %d)"), MaxDeviation * 100.0, Worst), MaxDeviation < 0.05);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

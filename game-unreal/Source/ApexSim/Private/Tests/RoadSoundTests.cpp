#include "ApexTestCommon.h"
#include "Audio/ApexRoadSound.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr float RoadTestSampleRate = 48000.0f;

	/** A second of one situation, settled: the levels start where they are held, as they would mid-corner. */
	TArray<float> RenderRoad(const ApexRoadSynth::FInputs& Inputs, float BumpMps = 0.0f, float ImpactMps = 0.0f,
		float SampleRate = RoadTestSampleRate)
	{
		ApexRoadSynth::FState State;
		State.Smoothed = Inputs;
		constexpr int32 Block = 480;
		TArray<float> Frames;
		Frames.SetNumZeroed(static_cast<int32>(SampleRate) / Block * Block);
		for (int32 At = 0; At < Frames.Num(); At += Block)
		{
			if (At == Block * 10)
			{
				ApexRoadSynth::Hit(State, BumpMps, ImpactMps);
			}
			ApexRoadSynth::Render(State, Inputs, SampleRate, Frames.GetData() + At, Block);
		}
		return Frames;
	}

	float RoadRms(const TArray<float>& Frames)
	{
		double Sum = 0.0;
		for (const float Sample : Frames)
		{
			Sum += Sample * Sample;
		}
		return static_cast<float>(FMath::Sqrt(Sum / FMath::Max(Frames.Num(), 1)));
	}

	float RoadPeak(const TArray<float>& Frames)
	{
		float Peak = 0.0f;
		for (const float Sample : Frames)
		{
			Peak = FMath::Max(Peak, FMath::Abs(Sample));
		}
		return Peak;
	}

	/** Power in a band a few dozen Hz wide around Hz: a squeal is narrow noise, not a line. */
	double RoadBandPower(const TArray<float>& Frames, double Hz, double SampleRate = RoadTestSampleRate)
	{
		double Total = 0.0;
		for (int32 Step = -4; Step <= 4; ++Step)
		{
			const double At = Hz + Step * 10.0;
			double Re = 0.0;
			double Im = 0.0;
			for (int32 Index = 0; Index < Frames.Num(); ++Index)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * At * Index / SampleRate;
				Re += Frames[Index] * FMath::Cos(Angle);
				Im += Frames[Index] * FMath::Sin(Angle);
			}
			Total += (Re * Re + Im * Im) / (static_cast<double>(Frames.Num()) * Frames.Num());
		}
		return Total;
	}

	ApexRoadSynth::FInputs Cruise(float SpeedMps)
	{
		ApexRoadSynth::FInputs Inputs;
		Inputs.SpeedMps = SpeedMps;
		return Inputs;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexRoadSquealTest,
	"ApexSim.Audio.RoadSqueal",
	ApexTestFlags)

bool FApexRoadSquealTest::RunTest(const FString& Parameters)
{
	const float Gripping = RoadRms(RenderRoad(Cruise(40.0f)));

	ApexRoadSynth::FInputs Understeer = Cruise(40.0f);
	Understeer.FrontSlide = 1.0f;
	ApexRoadSynth::FInputs Oversteer = Cruise(40.0f);
	Oversteer.RearSlide = 1.0f;
	const TArray<float> Front = RenderRoad(Understeer);
	const TArray<float> Rear = RenderRoad(Oversteer);

	TestTrue(*FString::Printf(TEXT("a sliding axle is heard over the road (%.3f against %.3f)"), RoadRms(Front), Gripping),
		RoadRms(Front) > Gripping * 4.0f);

	// Which end is sliding is told by pitch: each axle sings at its own note.
	const double FrontHz = ApexRoadSynth::SquealHz(0, 40.0f, 1.0f);
	const double RearHz = ApexRoadSynth::SquealHz(1, 40.0f, 1.0f);
	TestTrue(TEXT("the front sings higher than the rear"), FrontHz > RearHz * 1.15);
	TestTrue(TEXT("understeer is at the front's note"), RoadBandPower(Front, FrontHz) > RoadBandPower(Front, RearHz) * 5.0);
	TestTrue(TEXT("oversteer is at the rear's note"), RoadBandPower(Rear, RearHz) > RoadBandPower(Rear, FrontHz) * 5.0);

	// The sound's own thresholds, in multiples of the tyre's peak slip. A car
	// at its peak slip angle is being driven well, corner after corner, and
	// must be silent: driven from the force feedback's 0.9 onset this
	// screeched at every turn of the wheel.
	TestEqual(TEXT("no howl short of the peak"), ApexRoadSynth::SquealFromSlipAngle(0.9f), 0.0f);
	TestEqual(TEXT("none at the peak"), ApexRoadSynth::SquealFromSlipAngle(1.0f), 0.0f);
	TestEqual(TEXT("none a little past it"), ApexRoadSynth::SquealFromSlipAngle(-1.2f), 0.0f);
	TestTrue(TEXT("a slide howls"), ApexRoadSynth::SquealFromSlipAngle(1.8f) > 0.3f);
	TestEqual(TEXT("and twice and a half the peak is all of it"), ApexRoadSynth::SquealFromSlipAngle(-2.5f), 1.0f);
	TestEqual(TEXT("ABS holding a wheel at its peak is not a lockup"), ApexRoadSynth::LockupFromSlipRatio(-1.0f), 0.0f);
	TestEqual(TEXT("nor is traction control wheelspin"), ApexRoadSynth::WheelspinFromSlipRatio(1.0f), 0.0f);
	TestEqual(TEXT("braking is not wheelspin"), ApexRoadSynth::WheelspinFromSlipRatio(-6.0f), 0.0f);
	TestTrue(TEXT("a locked wheel is"), ApexRoadSynth::LockupFromSlipRatio(-6.0f) == 1.0f);

	// One telemetry frame of "sliding" is not a slide. (The feedback keeps the
	// peak slip between messages, so a single tick over a bump looks like this.)
	{
		ApexRoadSynth::FState State;
		ApexRoadSynth::FInputs Steady = Cruise(40.0f);
		State.Smoothed = Steady;
		ApexRoadSynth::FInputs Spike = Steady;
		Spike.FrontSlide = 1.0f;
		TArray<float> Frames;
		Frames.SetNumZeroed(800 * 12);
		for (int32 Frame = 0; Frame < 12; ++Frame)
		{
			ApexRoadSynth::Render(State, Frame == 2 ? Spike : Steady, RoadTestSampleRate, Frames.GetData() + Frame * 800, 800);
		}
		TestTrue(*FString::Printf(TEXT("a one-frame spike does not chirp (%.3f against a slide's %.3f)"), RoadPeak(Frames), RoadPeak(Front)),
			RoadPeak(Frames) < RoadPeak(Front) * 0.2f);
	}

	// Half way past the peak is a warning, not yet a howl.
	ApexRoadSynth::FInputs Edge = Cruise(40.0f);
	Edge.FrontSlide = 0.5f;
	TestTrue(TEXT("the squeal grows with the slide"), RoadRms(RenderRoad(Edge)) < RoadRms(Front) * 0.6f);

	// Nothing scrubs at a crawl, whatever the slip says: a parked car with
	// the wheels turned reports a slip angle too.
	ApexRoadSynth::FInputs Parked = Cruise(1.0f);
	Parked.FrontSlide = 1.0f;
	TestTrue(TEXT("no squeal at a crawl"), RoadRms(RenderRoad(Parked)) < 0.005f);

	// Rubber sings on a dry road and hisses on a wet one.
	ApexRoadSynth::FInputs Wet = Understeer;
	Wet.bWet = true;
	TestTrue(TEXT("a wet road takes the note out of the squeal"),
		RoadBandPower(RenderRoad(Wet), FrontHz) < RoadBandPower(Front, FrontHz) * 0.2);

	// Grass does not squeal.
	ApexRoadSynth::FInputs Grass = Understeer;
	Grass.OffTrack = 1.0f;
	TestTrue(TEXT("no squeal off the road"), RoadBandPower(RenderRoad(Grass), FrontHz) < RoadBandPower(Front, FrontHz) * 0.05);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexRoadCurbTest,
	"ApexSim.Audio.RoadCurb",
	ApexTestFlags)

bool FApexRoadCurbTest::RunTest(const FString& Parameters)
{
	// A kerb is a drone at the rate its ribs pass: 0.9 m apart, the force
	// feedback's spacing, so 45 m/s is 50 ribs a second at any device rate.
	TestEqual(TEXT("rib rate"), ApexRoadSynth::CurbRibHz(45.0f), 50.0f);

	ApexRoadSynth::FInputs OnCurb = Cruise(45.0f);
	OnCurb.CurbLeft = 1.0f;
	for (const float SampleRate : {48000.0f, 44100.0f})
	{
		const TArray<float> Frames = RenderRoad(OnCurb, 0.0f, 0.0f, SampleRate);
		const double AtRibs = RoadBandPower(Frames, 50.0, SampleRate);
		const double Beside = RoadBandPower(Frames, 50.0 * 1.5, SampleRate);
		TestTrue(*FString::Printf(TEXT("the drone is at the rib rate at %.0fHz (%.2e against %.2e)"), SampleRate, AtRibs, Beside),
			AtRibs > Beside * 8.0);
	}

	const float Road = RoadRms(RenderRoad(Cruise(45.0f)));
	const float Kerb = RoadRms(RenderRoad(OnCurb));
	TestTrue(*FString::Printf(TEXT("a kerb is heard over the road (%.3f against %.3f)"), Kerb, Road), Kerb > Road * 3.0f);

	ApexRoadSynth::FInputs OneWheel = OnCurb;
	OneWheel.CurbLeft = 0.5f;
	TestTrue(TEXT("one wheel on it is quieter than two"), RoadRms(RenderRoad(OneWheel)) < Kerb * 0.8f);

	// Off the road altogether: a rumble with nothing of the kerb's pitch in it.
	ApexRoadSynth::FInputs Grass = Cruise(35.0f);
	Grass.OffTrack = 1.0f;
	TestTrue(TEXT("grass rumbles"), RoadRms(RenderRoad(Grass)) > RoadRms(RenderRoad(Cruise(35.0f))) * 4.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexRoadHitsTest,
	"ApexSim.Audio.RoadHits",
	ApexTestFlags)

bool FApexRoadHitsTest::RunTest(const FString& Parameters)
{
	const float Rolling = RoadPeak(RenderRoad(Cruise(30.0f)));

	// The road's ordinary texture moves the suspension all the time; only a
	// real hit is a thud. Same threshold as the pad's thump.
	TestTrue(TEXT("a ripple is not a bump"), RoadPeak(RenderRoad(Cruise(30.0f), 0.2f)) < Rolling * 1.5f);
	const float Bump = RoadPeak(RenderRoad(Cruise(30.0f), 1.5f));
	const float Landing = RoadPeak(RenderRoad(Cruise(30.0f), 3.0f));
	TestTrue(*FString::Printf(TEXT("a bump thuds (%.2f against %.2f)"), Bump, Rolling), Bump > Rolling * 3.0f);
	TestTrue(TEXT("a landing thuds harder"), Landing > Bump * 1.3f);

	const float Tap = RoadPeak(RenderRoad(Cruise(30.0f), 0.0f, 2.0f));
	const float Crash = RoadPeak(RenderRoad(Cruise(30.0f), 0.0f, 12.0f));
	TestTrue(TEXT("contact crunches"), Tap > Rolling * 2.0f);
	TestTrue(TEXT("a crash crunches harder than a tap"), Crash > Tap * 1.5f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexRoadRenderTest,
	"ApexSim.Audio.RoadRender",
	ApexTestFlags)

bool FApexRoadRenderTest::RunTest(const FString& Parameters)
{
	// A parked car with nothing happening to it makes no sound at all.
	TestTrue(TEXT("standstill is silent"), RoadPeak(RenderRoad(Cruise(0.0f))) < 0.0005f);

	// Speed alone is heard: 300 km/h is not 70 with the same engine note.
	const float Slow = RoadRms(RenderRoad(Cruise(20.0f)));
	const float Fast = RoadRms(RenderRoad(Cruise(85.0f)));
	TestTrue(*FString::Printf(TEXT("wind and road grow with speed (%.3f to %.3f)"), Slow, Fast), Fast > Slow * 5.0f);
	TestTrue(TEXT("but stay under the engine"), Fast < 0.1f);

	// Everything at once, past every range: finite and inside the rails.
	ApexRoadSynth::FInputs Worst;
	Worst.SpeedMps = 120.0f;
	Worst.FrontSlide = Worst.RearSlide = Worst.Lockup = Worst.Wheelspin = 1.0f;
	Worst.CurbLeft = Worst.CurbRight = 1.0f;
	Worst.OffTrack = 0.5f;
	Worst.bWet = true;
	const TArray<float> Frames = RenderRoad(Worst, 10.0f, 50.0f);
	bool bFinite = true;
	for (const float Sample : Frames)
	{
		bFinite = bFinite && FMath::IsFinite(Sample);
	}
	TestTrue(TEXT("the worst corner in the world is finite"), bFinite);
	TestTrue(TEXT("and does not clip"), RoadPeak(Frames) <= 1.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

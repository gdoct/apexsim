#include "ApexTestCommon.h"
#include "Audio/ApexEngineSound.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The widest rev range in content/cars: the F1 idles at 4500 and revs to 15,500. */
	constexpr float F1IdleRpm = 4500.0f;
	constexpr float F1MaxRpm = 15500.0f;

	constexpr float TestSampleRate = 48000.0f;

	/**
	 * Renders a tenth of a second at a fixed operating point and counts
	 * upward zero crossings — a stand-in for "how high does this sound",
	 * which is the thing the ear notices and the numbers cannot fake.
	 */
	int32 CrossingsAt(const ApexEngineSynth::FParams& Params, float Rpm, int32 Gear)
	{
		ApexEngineSynth::FState State;
		TArray<float> Frames;
		Frames.SetNumUninitialized(static_cast<int32>(TestSampleRate) / 10);
		// Throttle closed: the grit layer is scaled by it, so the signal is a
		// pure harmonic stack whose crossings track the fundamental exactly.
		ApexEngineSynth::Render(State, Params, Rpm, 0.0f, Gear, TestSampleRate, Frames.GetData(), Frames.Num());

		int32 Crossings = 0;
		for (int32 Index = 1; Index < Frames.Num(); ++Index)
		{
			if (Frames[Index - 1] <= 0.0f && Frames[Index] > 0.0f)
			{
				++Crossings;
			}
		}
		return Crossings;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineNoteTest,
	"ApexSim.Audio.EngineNote",
	ApexTestFlags)

bool FApexEngineNoteTest::RunTest(const FString& Parameters)
{
	// The note has to keep climbing for as long as the revs do. It used to be a
	// lerp across a guessed rev range, which pinned the pitch at 8000rpm while
	// an F1 carried on to 15,000 — the whole top half of every gear at one note.
	float Previous = 0.0f;
	for (float Rpm = 1000.0f; Rpm <= F1MaxRpm; Rpm += 250.0f)
	{
		const float Hz = ApexEngineSynth::NoteHz(Rpm, 1);
		TestTrue(*FString::Printf(TEXT("%.0frpm (%.1fHz) is higher than %.0frpm (%.1fHz)"),
					 Rpm, Hz, Rpm - 250.0f, Previous),
			Hz > Previous);
		Previous = Hz;
	}

	// Proportional, so an upshift is heard as the interval the revs fell by.
	const float Before = ApexEngineSynth::NoteHz(15000.0f, 3);
	const float After = ApexEngineSynth::NoteHz(11000.0f, 4);
	TestTrue(*FString::Printf(TEXT("upshift drops the note (%.1fHz -> %.1fHz)"), Before, After), After < Before * 0.85f);

	// Idle is audible without a subwoofer, and the top of an F1's rev range
	// leaves its fourth harmonic far short of Nyquist at any device rate.
	const float IdleHz = ApexEngineSynth::NoteHz(F1IdleRpm, 1);
	const float TopHz = ApexEngineSynth::NoteHz(F1MaxRpm, 8);
	TestTrue(*FString::Printf(TEXT("idle is audible (%.1fHz)"), IdleHz), IdleHz > 40.0f);
	TestTrue(*FString::Printf(TEXT("top of the range stays musical (%.1fHz)"), TopHz), TopHz < 1000.0f);

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineGearTrimTest,
	"ApexSim.Audio.EngineGearTrim",
	ApexTestFlags)

bool FApexEngineGearTrimTest::RunTest(const FString& Parameters)
{
	// Each gear sits a little lower than the one below at the same revs, so a
	// shift is audible even in the instant before the revs fall.
	float Previous = ApexEngineSynth::NoteHz(8000.0f, 1);
	for (int32 Gear = 2; Gear <= 8; ++Gear)
	{
		const float Hz = ApexEngineSynth::NoteHz(8000.0f, Gear);
		TestTrue(*FString::Printf(TEXT("gear %d is lower than gear %d (%.1fHz < %.1fHz)"), Gear, Gear - 1, Hz, Previous),
			Hz < Previous);
		// A little lower, not a different engine: the drop across the whole
		// gearbox stays inside a couple of semitones.
		TestTrue(*FString::Printf(TEXT("gear %d is only a little lower (%.1fHz)"), Gear, Hz), Hz > Previous * 0.95f);
		Previous = Hz;
	}

	TestTrue(TEXT("first gear is untrimmed"), FMath::IsNearlyEqual(ApexEngineSynth::GearTrim(1), 1.0f));
	// Neutral and reverse are not gears above first; reverse is a negative index.
	TestTrue(TEXT("neutral matches first"), FMath::IsNearlyEqual(ApexEngineSynth::GearTrim(0), 1.0f));
	TestTrue(TEXT("reverse matches first"), FMath::IsNearlyEqual(ApexEngineSynth::GearTrim(-1), 1.0f));
	TestTrue(TEXT("trim stops compounding past the top gear"),
		FMath::IsNearlyEqual(ApexEngineSynth::GearTrim(20), ApexEngineSynth::GearTrim(8)));

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineRenderTest,
	"ApexSim.Audio.EngineRender",
	ApexTestFlags)

bool FApexEngineRenderTest::RunTest(const FString& Parameters)
{
	// A rev range that has not caught up yet — the state every car is in on its
	// first lap, since the redline is never broadcast. It may cost brightness;
	// it must not cost the pitch.
	const ApexEngineSynth::FParams Stale{800.0f, 8000.0f};
	const int32 Low = CrossingsAt(Stale, 9000.0f, 1);
	const int32 High = CrossingsAt(Stale, 12000.0f, 1);
	TestTrue(*FString::Printf(TEXT("pitch rises past a stale max (%d -> %d crossings)"), Low, High),
		High > Low * 5 / 4);

	// Whatever the operating point, the mixer gets a finite, unclipped signal.
	const ApexEngineSynth::FParams F1{F1IdleRpm, F1MaxRpm};
	const float Rpms[] = {0.0f, F1IdleRpm, 9000.0f, F1MaxRpm};
	const float Throttles[] = {0.0f, 0.5f, 1.0f};
	for (float Rpm : Rpms)
	{
		for (float Throttle : Throttles)
		{
			ApexEngineSynth::FState State;
			TArray<float> Frames;
			Frames.SetNumUninitialized(1024);
			ApexEngineSynth::Render(State, F1, Rpm, Throttle, 3, TestSampleRate, Frames.GetData(), Frames.Num());

			float Peak = 0.0f;
			bool bFinite = true;
			for (float Sample : Frames)
			{
				bFinite = bFinite && FMath::IsFinite(Sample);
				Peak = FMath::Max(Peak, FMath::Abs(Sample));
			}
			TestTrue(*FString::Printf(TEXT("%.0frpm at %.0f%% throttle is finite"), Rpm, Throttle * 100.0f), bFinite);
			TestTrue(*FString::Printf(TEXT("%.0frpm at %.0f%% throttle does not clip (peak %.2f)"),
						 Rpm, Throttle * 100.0f, Peak),
				Peak <= 1.0f);
			TestTrue(*FString::Printf(TEXT("%.0frpm at %.0f%% throttle is audible (peak %.2f)"),
						 Rpm, Throttle * 100.0f, Peak),
				Peak > 0.05f);
		}
	}

	// The device picks the rate, so the note must not depend on it.
	const int32 At48k = CrossingsAt(F1, 12000.0f, 2);
	ApexEngineSynth::FState State;
	TArray<float> Frames;
	Frames.SetNumUninitialized(4410);
	ApexEngineSynth::Render(State, F1, 12000.0f, 0.0f, 2, 44100.0f, Frames.GetData(), Frames.Num());
	int32 At44k = 0;
	for (int32 Index = 1; Index < Frames.Num(); ++Index)
	{
		if (Frames[Index - 1] <= 0.0f && Frames[Index] > 0.0f)
		{
			++At44k;
		}
	}
	TestTrue(*FString::Printf(TEXT("same note at 44.1k and 48k (%d vs %d crossings)"), At44k, At48k),
		FMath::Abs(At44k - At48k) <= FMath::Max(2, At48k / 20));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

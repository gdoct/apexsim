#include "ApexProtocolTypes.h"
#include "ApexTestCommon.h"
#include "Input/ApexForceFeedback.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr float FeedbackTestDt = 1.0f / 240.0f;

	/** The strength the effects were designed at. */
	const float DesignGain = ApexFfb::GainFromStrength(0.5f);

	/** A car rolling in a straight line on the road at `SpeedMps`, all four tyres gripping. */
	ApexFfb::FSignals Cruising(float SpeedMps)
	{
		ApexFfb::FSignals Signals;
		Signals.bActive = true;
		Signals.SpeedMps = SpeedMps;
		Signals.Gear = 4;
		return Signals;
	}

	/** The loudest each motor gets over `Seconds` of the same signals. */
	ApexFfb::FRumble PeakOver(const ApexFfb::FSignals& Signals, float Seconds, float Gain = DesignGain)
	{
		ApexFfb::FGamepadState State;
		ApexFfb::FRumble Peak;
		for (float T = 0.0f; T < Seconds; T += FeedbackTestDt)
		{
			const ApexFfb::FRumble Frame = ApexFfb::MixGamepad(Signals, State, FeedbackTestDt, Gain);
			Peak.Low = FMath::Max(Peak.Low, Frame.Low);
			Peak.High = FMath::Max(Peak.High, Frame.High);
		}
		return Peak;
	}

	/** Times the heavy motor switches from its quiet level to a pulse over one second. */
	int32 PulsesPerSecond(const ApexFfb::FSignals& Signals)
	{
		ApexFfb::FGamepadState State;
		int32 Rises = 0;
		float Previous = 0.0f;
		for (float T = 0.0f; T < 1.0f; T += FeedbackTestDt)
		{
			const float Low = ApexFfb::MixGamepad(Signals, State, FeedbackTestDt, DesignGain).Low;
			if (Low > Previous + 0.1f)
			{
				++Rises;
			}
			Previous = Low;
		}
		return Rises;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbSignalsTest,
	"ApexSim.Input.ForceFeedback.Signals",
	ApexTestFlags)

bool FApexFfbSignalsTest::RunTest(const FString& Parameters)
{
	using W = FApexDriverFeedback;

	FApexDriverFeedback Feedback;
	Feedback.SteerTorque = {-0.2f, -0.4f};
	// Fronts at their peak (just felt), rears well past it.
	Feedback.Wheels[W::FrontLeft].SlipAngle = -1.0f;
	Feedback.Wheels[W::RearRight].SlipAngle = 2.5f;
	// A locked front, an ABS-held rear (exactly at the peak: ABS's to report).
	Feedback.Wheels[W::FrontRight].SlipRatio = -12.5f;
	Feedback.Wheels[W::RearLeft].SlipRatio = -1.0f;
	Feedback.Wheels[W::FrontLeft].Surface = EApexContactSurface::Curb;
	Feedback.Wheels[W::RearLeft].Surface = EApexContactSurface::Curb;
	Feedback.Wheels[W::FrontRight].Surface = EApexContactSurface::Off;
	Feedback.Wheels[W::RearRight].SuspensionMps = -2.0f;
	Feedback.ImpactMps = 5.0f;
	Feedback.bAbsActive = true;

	const ApexFfb::FSignals Signals = ApexFfb::MakeSignals(Feedback, 30.0f, 3, true);
	TestTrue(TEXT("active"), Signals.bActive);
	TestEqual(TEXT("torque is the newest sample, in screen sign"), Signals.SteerTorque, 0.4f);
	TestTrue(TEXT("fronts at the peak are just felt"), Signals.FrontSlide > 0.0f && Signals.FrontSlide < 0.2f);
	TestEqual(TEXT("rears well past the peak"), Signals.RearSlide, 1.0f);
	TestEqual(TEXT("a locked wheel is full lockup"), Signals.Lockup, 1.0f);
	TestEqual(TEXT("no wheelspin"), Signals.Wheelspin, 0.0f);
	TestTrue(TEXT("ABS"), Signals.bAbs);
	TestEqual(TEXT("both left wheels on the curb"), Signals.CurbLeft, 1.0f);
	TestEqual(TEXT("no right wheel on the curb"), Signals.CurbRight, 0.0f);
	TestEqual(TEXT("one wheel in four off"), Signals.OffTrack, 0.25f);
	TestEqual(TEXT("bump magnitude"), Signals.BumpMps, 2.0f);
	TestEqual(TEXT("impact"), Signals.ImpactMps, 5.0f);

	// The same message on a later frame: the hits have already been felt.
	const ApexFfb::FSignals Later = ApexFfb::MakeSignals(Feedback, 30.0f, 3, false);
	TestEqual(TEXT("bump fires once"), Later.BumpMps, 0.0f);
	TestEqual(TEXT("impact fires once"), Later.ImpactMps, 0.0f);
	TestEqual(TEXT("slides hold"), Later.RearSlide, 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbGamepadFeelTest,
	"ApexSim.Input.ForceFeedback.GamepadFeel",
	ApexTestFlags)

bool FApexFfbGamepadFeelTest::RunTest(const FString& Parameters)
{
	// Gripping on the road is silence: a rumble that never stops says nothing.
	{
		const ApexFfb::FRumble Quiet = PeakOver(Cruising(45.0f), 1.0f);
		TestTrue(FString::Printf(TEXT("cruising is silent (low %.3f, high %.3f)"), Quiet.Low, Quiet.High),
			Quiet.Low < 0.01f && Quiet.High < 0.01f);
	}

	// Understeer buzzes, oversteer heaves: the two must not feel alike.
	{
		ApexFfb::FSignals Front = Cruising(30.0f);
		Front.FrontSlide = 0.8f;
		const ApexFfb::FRumble Under = PeakOver(Front, 0.2f);
		TestTrue(FString::Printf(TEXT("understeer is on the light motor (low %.2f, high %.2f)"), Under.Low, Under.High),
			Under.High > 0.3f && Under.High > 2.0f * Under.Low);

		ApexFfb::FSignals Rear = Cruising(30.0f);
		Rear.RearSlide = 0.8f;
		const ApexFfb::FRumble Over = PeakOver(Rear, 0.2f);
		TestTrue(FString::Printf(TEXT("oversteer is on the heavy motor (low %.2f, high %.2f)"), Over.Low, Over.High),
			Over.Low > 0.3f && Over.Low > 2.0f * Over.High);
	}

	// Curb ribs come faster the faster the car goes.
	{
		ApexFfb::FSignals Slow = Cruising(7.0f);
		Slow.CurbRight = 1.0f;
		ApexFfb::FSignals Fast = Slow;
		Fast.SpeedMps = 20.0f;
		const int32 SlowPulses = PulsesPerSecond(Slow);
		const int32 FastPulses = PulsesPerSecond(Fast);
		TestTrue(FString::Printf(TEXT("curb pulses %d/s at 7 m/s, %d/s at 20 m/s"), SlowPulses, FastPulses),
			SlowPulses >= 5 && FastPulses > SlowPulses + 5);
	}

	// ABS pulses at speed and lets go at walking pace, like the real thing.
	{
		ApexFfb::FSignals Braking = Cruising(25.0f);
		Braking.bAbs = true;
		TestTrue(TEXT("ABS pulses at speed"), PulsesPerSecond(Braking) >= 10);
		Braking.SpeedMps = 1.0f;
		TestEqual(TEXT("no ABS pulses at a crawl"), PulsesPerSecond(Braking), 0);
	}

	// Grass: rough, and rougher with speed.
	{
		ApexFfb::FSignals Grass = Cruising(8.0f);
		Grass.OffTrack = 1.0f;
		const float SlowLow = PeakOver(Grass, 1.0f).Low;
		Grass.SpeedMps = 30.0f;
		const float FastLow = PeakOver(Grass, 1.0f).Low;
		TestTrue(FString::Printf(TEXT("grass rumbles (%.2f at 8 m/s, %.2f at 30)"), SlowLow, FastLow),
			SlowLow > 0.1f && FastLow > SlowLow);
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbHitsTest,
	"ApexSim.Input.ForceFeedback.Hits",
	ApexTestFlags)

bool FApexFfbHitsTest::RunTest(const FString& Parameters)
{
	// A contact is a hard hit that dies away within a second, fired once.
	{
		ApexFfb::FGamepadState State;
		ApexFfb::FSignals Hit = Cruising(30.0f);
		Hit.ImpactMps = 8.0f;
		const ApexFfb::FRumble First = ApexFfb::MixGamepad(Hit, State, FeedbackTestDt, DesignGain);
		TestTrue(FString::Printf(TEXT("the hit lands on both motors (low %.2f, high %.2f)"), First.Low, First.High),
			First.Low > 0.6f && First.High > 0.4f);

		ApexFfb::FRumble After;
		const ApexFfb::FSignals Quiet = Cruising(30.0f);
		for (int32 Frame = 0; Frame < 240; ++Frame)
		{
			After = ApexFfb::MixGamepad(Quiet, State, FeedbackTestDt, DesignGain);
		}
		TestTrue(FString::Printf(TEXT("a second later it has died away (low %.3f)"), After.Low), After.Low < 0.05f);
	}

	// Ordinary road give is not a bump; a landing is.
	{
		ApexFfb::FSignals Road = Cruising(30.0f);
		Road.BumpMps = 0.1f;
		TestTrue(TEXT("road give is silent"), PeakOver(Road, 0.1f).Low < 0.01f);

		ApexFfb::FGamepadState State;
		ApexFfb::FSignals Landing = Cruising(30.0f);
		Landing.BumpMps = 3.0f;
		TestTrue(TEXT("a landing thumps"), ApexFfb::MixGamepad(Landing, State, FeedbackTestDt, DesignGain).Low > 0.6f);
	}

	// A gear change kicks; the first car seen does not.
	{
		ApexFfb::FGamepadState State;
		ApexFfb::FSignals Signals = Cruising(30.0f);
		TestTrue(TEXT("first frame is not a shift"), ApexFfb::MixGamepad(Signals, State, FeedbackTestDt, DesignGain).Low < 0.01f);
		Signals.Gear = 5;
		TestTrue(TEXT("a shift kicks"), ApexFfb::MixGamepad(Signals, State, FeedbackTestDt, DesignGain).Low > 0.3f);
	}

	// No car, no feedback: everything decays to nothing.
	{
		ApexFfb::FGamepadState State;
		ApexFfb::FSignals Sliding = Cruising(30.0f);
		Sliding.RearSlide = 1.0f;
		Sliding.ImpactMps = 10.0f;
		ApexFfb::MixGamepad(Sliding, State, FeedbackTestDt, DesignGain);
		ApexFfb::FRumble Out;
		for (int32 Frame = 0; Frame < 240; ++Frame)
		{
			Out = ApexFfb::MixGamepad(ApexFfb::FSignals(), State, FeedbackTestDt, DesignGain);
		}
		TestTrue(TEXT("inactive decays to silence"), Out.Low < 0.02f && Out.High < 0.02f);
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbStrengthTest,
	"ApexSim.Input.ForceFeedback.Strength",
	ApexTestFlags)

bool FApexFfbStrengthTest::RunTest(const FString& Parameters)
{
	// Everything at once, at every strength: always a valid motor level, and
	// zero strength is off.
	ApexFfb::FSignals Everything = Cruising(40.0f);
	Everything.FrontSlide = Everything.RearSlide = Everything.Lockup = Everything.Wheelspin = 1.0f;
	Everything.bAbs = Everything.bTractionControl = true;
	Everything.CurbLeft = Everything.CurbRight = Everything.OffTrack = 1.0f;
	Everything.BumpMps = 10.0f;
	Everything.ImpactMps = 50.0f;

	for (float Strength = 0.0f; Strength <= 1.0f; Strength += 0.25f)
	{
		ApexFfb::FGamepadState State;
		const float Gain = ApexFfb::GainFromStrength(Strength);
		for (int32 Frame = 0; Frame < 120; ++Frame)
		{
			const ApexFfb::FRumble Out = ApexFfb::MixGamepad(Everything, State, FeedbackTestDt, Gain);
			if (!(Out.Low >= 0.0f && Out.Low <= 1.0f && Out.High >= 0.0f && Out.High <= 1.0f))
			{
				AddError(FString::Printf(TEXT("strength %.2f frame %d out of range: low %f high %f"),
					Strength, Frame, Out.Low, Out.High));
				return false;
			}
			if (Strength == 0.0f && (Out.Low > 0.0f || Out.High > 0.0f))
			{
				AddError(TEXT("zero strength must be silent"));
				return false;
			}
		}
	}

	ApexFfb::FSignals Rear = Cruising(30.0f);
	Rear.RearSlide = 0.5f;
	TestTrue(TEXT("more strength, more rumble"),
		PeakOver(Rear, 0.1f, ApexFfb::GainFromStrength(0.8f)).Low > PeakOver(Rear, 0.1f, ApexFfb::GainFromStrength(0.3f)).Low);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

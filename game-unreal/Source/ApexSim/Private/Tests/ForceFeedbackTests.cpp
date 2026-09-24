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

	/** The wheel's forces once the smoothed torque has settled on these signals. */
	FApexWheelEffects SettledWheel(
		const ApexFfb::FSignals& Signals, const ApexFfb::FWheelTuning& Tuning, float Seconds = 0.5f)
	{
		ApexFfb::FWheelState State;
		FApexWheelEffects Effects;
		for (float T = 0.0f; T < Seconds; T += FeedbackTestDt)
		{
			Effects = ApexFfb::MixWheel(Signals, State, FeedbackTestDt, Tuning);
		}
		return Effects;
	}

	/** The loudest vibration the wheel is asked for over `Seconds`, and at what rate. */
	FApexWheelEffects PeakWheelVibration(
		const ApexFfb::FSignals& Signals, const ApexFfb::FWheelTuning& Tuning, float Seconds = 0.5f)
	{
		ApexFfb::FWheelState State;
		FApexWheelEffects Peak;
		for (float T = 0.0f; T < Seconds; T += FeedbackTestDt)
		{
			const FApexWheelEffects Frame = ApexFfb::MixWheel(Signals, State, FeedbackTestDt, Tuning);
			if (Frame.VibrationAmplitude > Peak.VibrationAmplitude)
			{
				Peak = Frame;
			}
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

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbWheelTorqueTest,
	"ApexSim.Input.ForceFeedback.WheelTorque",
	ApexTestFlags)

bool FApexFfbWheelTorqueTest::RunTest(const FString& Parameters)
{
	const ApexFfb::FWheelTuning Designed;	 // force 0.5, the strength the numbers were chosen at

	// Straight and gripping: the rim is being held where the driver put it.
	{
		const FApexWheelEffects Quiet = SettledWheel(Cruising(50.0f), Designed);
		TestTrue(FString::Printf(TEXT("no torque with no torque (%.3f)"), Quiet.Constant),
			FMath::Abs(Quiet.Constant) < 0.01f);
		TestEqual(TEXT("and nothing to feel from the road"), Quiet.VibrationAmplitude, 0.0f);
		TestTrue(TEXT("but some weight in the rim"), Quiet.Damper > 0.0f);
		TestEqual(TEXT("and no centring while driving"), Quiet.Spring, 0.0f);
	}

	// The car's own torque, and which way it pushes.
	ApexFfb::FSignals Loaded = Cruising(45.0f);
	Loaded.SteerTorque = 1.0f;
	const float AtTheLimit = SettledWheel(Loaded, Designed).Constant;
	TestTrue(FString::Printf(TEXT("the grip limit is most of the base's force (%.2f)"), AtTheLimit),
		AtTheLimit > 0.85f && AtTheLimit < 0.97f);

	Loaded.SteerTorque = -1.0f;
	TestTrue(TEXT("the other way round pushes the other way"),
		FMath::IsNearlyEqual(SettledWheel(Loaded, Designed).Constant, -AtTheLimit, 0.01f));

	// Downforce takes the torque past its reference; a fast corner must still
	// feel stronger than a slow one rather than both hitting the stop.
	Loaded.SteerTorque = 2.0f;
	const float Loaded2 = SettledWheel(Loaded, Designed).Constant;
	Loaded.SteerTorque = 3.5f;
	const float Loaded3 = SettledWheel(Loaded, Designed).Constant;
	TestTrue(FString::Printf(TEXT("past the reference it keeps growing (%.2f then %.2f)"), Loaded2, Loaded3),
		Loaded2 > AtTheLimit && Loaded3 > Loaded2 && Loaded3 <= 1.0f);

	// A base wired the other way: the same feel, mirrored, and nothing else moved.
	ApexFfb::FWheelTuning Inverted = Designed;
	Inverted.bInvert = true;
	Loaded.SteerTorque = 1.0f;
	const FApexWheelEffects Mirrored = SettledWheel(Loaded, Inverted);
	TestTrue(TEXT("inverting flips the force"), FMath::IsNearlyEqual(Mirrored.Constant, -AtTheLimit, 0.01f));

	// Strength: more is more, and zero is a wheel the game is not touching.
	ApexFfb::FWheelTuning Strong = Designed;
	Strong.Force = 1.0f;
	TestTrue(TEXT("more strength, more force"), SettledWheel(Loaded, Strong).Constant > AtTheLimit);

	ApexFfb::FWheelTuning Off;
	Off.Force = 0.0f;
	Off.Damping = 0.0f;
	Off.RoadEffects = 0.0f;
	const FApexWheelEffects Silent = SettledWheel(Loaded, Off);
	TestEqual(TEXT("no force"), Silent.Constant, 0.0f);
	TestEqual(TEXT("no damper"), Silent.Damper, 0.0f);
	TestEqual(TEXT("and no centring either"), Silent.Spring, 0.0f);

	// No car: the force is let down rather than dropped, and the rim is given
	// something to sit against while the player is in the menus.
	{
		ApexFfb::FWheelState State;
		ApexFfb::MixWheel(Loaded, State, FeedbackTestDt, Designed);
		FApexWheelEffects Idle;
		for (int32 Frame = 0; Frame < 240; ++Frame)
		{
			Idle = ApexFfb::MixWheel(ApexFfb::FSignals(), State, FeedbackTestDt, Designed);
		}
		TestTrue(FString::Printf(TEXT("the force runs down to nothing (%.3f)"), Idle.Constant),
			FMath::Abs(Idle.Constant) < 0.02f);
		TestTrue(TEXT("and the rim is centred in the menus"), Idle.Spring > 0.0f);
	}

	// A hit yanks the rim its way on top of the torque, at once, and lets go
	// within a tenth of a second or so.
	{
		ApexFfb::FSignals Cornering = Cruising(30.0f);
		Cornering.SteerTorque = 0.5f;
		ApexFfb::FWheelState State;
		for (int32 Frame = 0; Frame < 120; ++Frame)
		{
			ApexFfb::MixWheel(Cornering, State, FeedbackTestDt, Designed);
		}
		const float Before = ApexFfb::MixWheel(Cornering, State, FeedbackTestDt, Designed).Constant;

		ApexFfb::FSignals Hit = Cornering;
		Hit.SteerKick = -1.5f;
		const float Yanked = ApexFfb::MixWheel(Hit, State, FeedbackTestDt, Designed).Constant;
		TestTrue(FString::Printf(TEXT("the kick overrides the torque on its frame (%.2f from %.2f)"), Yanked, Before),
			Yanked < -0.3f && Before > 0.2f);

		float After = Yanked;
		for (int32 Frame = 0; Frame < 72; ++Frame)	// 0.3 s
		{
			After = ApexFfb::MixWheel(Cornering, State, FeedbackTestDt, Designed).Constant;
		}
		TestTrue(FString::Printf(TEXT("and is gone again (%.2f vs %.2f)"), After, Before),
			FMath::IsNearlyEqual(After, Before, 0.05f));
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbWheelRoadTest,
	"ApexSim.Input.ForceFeedback.WheelRoad",
	ApexTestFlags)

bool FApexFfbWheelRoadTest::RunTest(const FString& Parameters)
{
	const ApexFfb::FWheelTuning Designed;

	// Curb ribs pass under the tyre faster the faster the car is going, and
	// that rate is what a curb feels like rather than how hard it buzzes.
	{
		ApexFfb::FSignals Slow = Cruising(8.0f);
		Slow.CurbLeft = 1.0f;
		ApexFfb::FSignals Fast = Slow;
		Fast.SpeedMps = 25.0f;

		const FApexWheelEffects SlowCurb = PeakWheelVibration(Slow, Designed);
		const FApexWheelEffects FastCurb = PeakWheelVibration(Fast, Designed);
		TestTrue(FString::Printf(TEXT("a curb is felt (%.2f at %.0f Hz)"), SlowCurb.VibrationAmplitude, SlowCurb.VibrationHz),
			SlowCurb.VibrationAmplitude > 0.2f);
		TestTrue(FString::Printf(TEXT("faster ribs at speed (%.0f Hz then %.0f Hz)"), SlowCurb.VibrationHz, FastCurb.VibrationHz),
			FastCurb.VibrationHz > SlowCurb.VibrationHz + 5.0f);
	}

	// Grass is rough, and it is the only thing the rim is saying while on it.
	{
		ApexFfb::FSignals Grass = Cruising(30.0f);
		Grass.OffTrack = 1.0f;
		TestTrue(TEXT("grass shakes the wheel"), PeakWheelVibration(Grass, Designed).VibrationAmplitude > 0.2f);
	}

	// Fronts sliding: a fine grain under the torque, quieter than a curb.
	{
		ApexFfb::FSignals Understeer = Cruising(30.0f);
		Understeer.FrontSlide = 1.0f;
		const FApexWheelEffects Scrub = PeakWheelVibration(Understeer, Designed);
		TestTrue(FString::Printf(TEXT("a sliding front is felt (%.2f)"), Scrub.VibrationAmplitude),
			Scrub.VibrationAmplitude > 0.05f);
		ApexFfb::FSignals Curbing = Cruising(30.0f);
		Curbing.CurbLeft = 1.0f;
		TestTrue(TEXT("but under a curb"),
			Scrub.VibrationAmplitude < PeakWheelVibration(Curbing, Designed).VibrationAmplitude);
	}

	// ABS through the column, as a real car's pedal and rack both do.
	{
		ApexFfb::FSignals Braking = Cruising(30.0f);
		Braking.bAbs = true;
		const FApexWheelEffects Pulsing = PeakWheelVibration(Braking, Designed);
		TestTrue(TEXT("ABS is felt"), Pulsing.VibrationAmplitude > 0.1f);
		TestTrue(FString::Printf(TEXT("at the rate it cycles (%.0f Hz)"), Pulsing.VibrationHz),
			Pulsing.VibrationHz > 8.0f && Pulsing.VibrationHz < 20.0f);
	}

	// A hit is the loudest thing a rim can say, so it takes the channel from
	// whatever else was on it.
	{
		ApexFfb::FSignals Curbing = Cruising(20.0f);
		Curbing.CurbRight = 1.0f;
		ApexFfb::FSignals Landing = Curbing;
		Landing.BumpMps = 3.0f;

		ApexFfb::FWheelState State;
		const float Steady = ApexFfb::MixWheel(Curbing, State, FeedbackTestDt, Designed).VibrationAmplitude;
		ApexFfb::FWheelState HitState;
		const float Hit = ApexFfb::MixWheel(Landing, HitState, FeedbackTestDt, Designed).VibrationAmplitude;
		TestTrue(FString::Printf(TEXT("a landing is louder than the curb it lands on (%.2f vs %.2f)"), Hit, Steady),
			Hit > Steady);
	}

	// The road-effects slider, and everything at once staying inside what a
	// device can be asked for, at every setting.
	{
		ApexFfb::FSignals Everything = Cruising(40.0f);
		Everything.SteerTorque = 2.5f;
		Everything.FrontSlide = Everything.RearSlide = Everything.Lockup = Everything.Wheelspin = 1.0f;
		Everything.bAbs = Everything.bTractionControl = true;
		Everything.CurbLeft = Everything.CurbRight = Everything.OffTrack = 1.0f;
		Everything.BumpMps = 10.0f;
		Everything.ImpactMps = 50.0f;
		Everything.SteerKick = -3.0f;

		for (float Setting = 0.0f; Setting <= 1.0f; Setting += 0.25f)
		{
			ApexFfb::FWheelTuning Tuning;
			Tuning.Force = Tuning.RoadEffects = Tuning.Damping = Setting;

			ApexFfb::FWheelState State;
			for (int32 Frame = 0; Frame < 120; ++Frame)
			{
				const FApexWheelEffects Out = ApexFfb::MixWheel(Everything, State, FeedbackTestDt, Tuning);
				const bool bInRange = FMath::Abs(Out.Constant) <= 1.0f
					&& Out.VibrationAmplitude >= 0.0f && Out.VibrationAmplitude <= 1.0f
					&& Out.Damper >= 0.0f && Out.Damper <= 1.0f
					&& Out.Spring >= 0.0f && Out.Spring <= 1.0f
					&& Out.VibrationHz >= 0.0f && Out.VibrationHz < 200.0f;
				if (!bInRange)
				{
					AddError(FString::Printf(
						TEXT("setting %.2f frame %d out of range: force %f, vibration %f @ %f Hz, damper %f, spring %f"),
						Setting, Frame, Out.Constant, Out.VibrationAmplitude, Out.VibrationHz, Out.Damper, Out.Spring));
					return false;
				}
			}
		}

		ApexFfb::FWheelTuning Quiet;
		Quiet.RoadEffects = 0.2f;
		ApexFfb::FWheelTuning Loud;
		Loud.RoadEffects = 0.9f;
		TestTrue(TEXT("more road effects, more vibration"),
			PeakWheelVibration(Everything, Loud).VibrationAmplitude > PeakWheelVibration(Everything, Quiet).VibrationAmplitude);
	}
	return true;
}

// -----------------------------------------------------------------------------

namespace
{
	/**
	 * A rim on a base, crudely: the force turns it (positive to the right),
	 * its own friction slows it. Enough to see whether the centring brings a
	 * rim home without swinging it past.
	 */
	struct FTestRim
	{
		float Degrees = 0.0f;
		float Rate = 0.0f;

		void Step(float Constant, float Dt)
		{
			Rate += (2500.0f * Constant - 6.0f * Rate) * Dt;
			Degrees += Rate * Dt;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbWheelCentreTest,
	"ApexSim.Input.ForceFeedback.WheelCentre",
	ApexTestFlags)

bool FApexFfbWheelCentreTest::RunTest(const FString& Parameters)
{
	const ApexFfb::FWheelTuning Designed;

	// A session starting with the rim left a quarter turn to the right: it is
	// brought home, not past, and let go of once it is there.
	{
		ApexFfb::FWheelState State;
		FTestRim Rim;
		Rim.Degrees = 90.0f;

		// In the menu first, then the car arrives on the grid.
		ApexFfb::FSignals Menu;
		Menu.bHasRim = true;
		Menu.RimDegrees = Rim.Degrees;
		ApexFfb::MixWheel(Menu, State, FeedbackTestDt, Designed);

		float Furthest = 0.0f;
		float Constant = 0.0f;
		for (float T = 0.0f; T < 3.5f; T += FeedbackTestDt)
		{
			ApexFfb::FSignals Grid = Cruising(0.0f);
			Grid.bHasRim = true;
			Grid.RimDegrees = Rim.Degrees;
			Constant = ApexFfb::MixWheel(Grid, State, FeedbackTestDt, Designed).Constant;
			Rim.Step(Constant, FeedbackTestDt);
			if (T > 0.1f)
			{
				Furthest = FMath::Min(Furthest, Rim.Degrees);
			}
		}
		TestTrue(FString::Printf(TEXT("the rim is centred (%.1f deg)"), Rim.Degrees), FMath::Abs(Rim.Degrees) < 3.0f);
		TestTrue(FString::Printf(TEXT("without swinging far past (%.1f deg)"), Furthest), Furthest > -10.0f);
		TestTrue(FString::Printf(TEXT("and then let go (%.3f)"), Constant), FMath::Abs(Constant) < 0.02f);
	}

	// Back from a pause mid-lap: the driver's hands are not overruled.
	{
		ApexFfb::FWheelState State;
		ApexFfb::FSignals Paused;
		Paused.bHasRim = true;
		Paused.RimDegrees = 60.0f;
		ApexFfb::MixWheel(Paused, State, FeedbackTestDt, Designed);

		ApexFfb::FSignals Racing = Cruising(40.0f);
		Racing.bHasRim = true;
		Racing.RimDegrees = 60.0f;
		FApexWheelEffects Out;
		for (int32 Frame = 0; Frame < 60; ++Frame)
		{
			Out = ApexFfb::MixWheel(Racing, State, FeedbackTestDt, Designed);
		}
		TestTrue(FString::Printf(TEXT("no centring at speed (%.3f)"), Out.Constant), FMath::Abs(Out.Constant) < 0.01f);
	}

	// Rolling away ends it: the car's torque is what the rim should feel.
	{
		ApexFfb::FWheelState State;
		ApexFfb::MixWheel(ApexFfb::FSignals(), State, FeedbackTestDt, Designed);
		ApexFfb::FSignals Grid = Cruising(0.0f);
		Grid.bHasRim = true;
		Grid.RimDegrees = 45.0f;
		const float Held = ApexFfb::MixWheel(Grid, State, FeedbackTestDt, Designed).Constant;
		ApexFfb::FSignals Off = Grid;
		Off.SpeedMps = 10.0f;
		float Let = Held;
		for (int32 Frame = 0; Frame < 120; ++Frame)
		{
			Let = ApexFfb::MixWheel(Off, State, FeedbackTestDt, Designed).Constant;
		}
		TestTrue(FString::Printf(TEXT("pushed home on the grid (%.3f)"), Held), Held < 0.0f);
		TestTrue(FString::Printf(TEXT("let go once rolling (%.3f)"), Let), FMath::Abs(Let) < 0.01f);
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexFfbWheelSoftLockTest,
	"ApexSim.Input.ForceFeedback.WheelSoftLock",
	ApexTestFlags)

bool FApexFfbWheelSoftLockTest::RunTest(const FString& Parameters)
{
	ApexFfb::FWheelTuning Tuning;
	Tuning.SteeringLockDeg = 480.0f;

	auto ForceAt = [&Tuning](float RimDegrees)
	{
		ApexFfb::FWheelState State;
		State.bWasActive = true;	// already driving: no centring
		ApexFfb::FSignals Signals = Cruising(30.0f);
		Signals.bHasRim = true;
		Signals.RimDegrees = RimDegrees;
		FApexWheelEffects Out;
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			Out = ApexFfb::MixWheel(Signals, State, FeedbackTestDt, Tuning);
		}
		return Out;
	};

	TestTrue(TEXT("inside the lock the rim is free"), FMath::Abs(ForceAt(230.0f).Constant) < 0.01f);
	const FApexWheelEffects Right = ForceAt(252.0f);
	TestTrue(FString::Printf(TEXT("past it to the right, pushed back left (%.2f)"), Right.Constant), Right.Constant < -0.6f);
	TestTrue(TEXT("and damped against bouncing"), Right.Damper >= 0.45f);
	TestTrue(TEXT("the same to the left"), ForceAt(-252.0f).Constant > 0.6f);
	TestTrue(TEXT("it builds over a few degrees rather than snapping"),
		FMath::Abs(ForceAt(242.0f).Constant) < FMath::Abs(Right.Constant));

	Tuning.bInvert = true;
	TestTrue(TEXT("an inverted base is pushed its own way"), ForceAt(252.0f).Constant > 0.6f);

	Tuning.bInvert = false;
	Tuning.SteeringLockDeg = 0.0f;
	TestTrue(TEXT("no stop when the lock is the base's whole rotation"), FMath::Abs(ForceAt(400.0f).Constant) < 0.01f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

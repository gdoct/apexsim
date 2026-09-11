#include "Input/ApexForceFeedback.h"

#include "ApexProtocolTypes.h"

namespace
{
	// --- Reading the tyres --------------------------------------------------------
	//
	// The wire gives slip as multiples of the tyre's own peak, so these hold for
	// every car: 1 is the top of the grip curve whatever the tyre.

	/** Slide is felt from a little before the peak, so the limit arrives in the hands before it arrives in the car. */
	constexpr float SlideOnset = 0.9f;
	/** Twice the peak slip angle is a car well into a slide. */
	constexpr float SlideFull = 2.0f;

	/**
	 * Past the peak slip ratio by this much counts as locking or spinning. ABS
	 * and traction control hold a wheel at exactly 1, which is theirs to report.
	 */
	constexpr float WheelSlipOnset = 1.1f;
	/** A locked wheel is ~12x its peak slip ratio; this is well short of that. */
	constexpr float LockupFullExcess = 3.0f;
	/** Wheelspin tops out near 4x the peak. */
	constexpr float WheelspinFullExcess = 2.0f;

	// --- The pad --------------------------------------------------------------

	/** Below this the tyres are not doing anything worth feeling. */
	constexpr float CrawlSpeedMps = 3.0f;
	/** Scrub from a sliding tyre builds over the first few metres a second. */
	constexpr float SlideFullSpeedMps = 10.0f;
	/** Road textures (curbs, grass) reach full strength by here. */
	constexpr float TextureFullSpeedMps = 25.0f;

	/** Distance between curb ribs: sets how fast the pulses come with speed. */
	constexpr float CurbRibSpacingM = 0.9f;
	/** The heavy motor smears anything past ~30 Hz into a hum, and below 6 a curb feels like separate hits. */
	constexpr float CurbMinHz = 6.0f;
	constexpr float CurbMaxHz = 30.0f;

	/** Pulse trains, whole numbers so a one-second clock wraps cleanly. */
	constexpr float AbsPulseHz = 14.0f;
	constexpr float TractionPulseHz = 18.0f;

	/** How often the off-track roughness picks a new level. */
	constexpr float NoiseHz = 25.0f;

	/** Suspension speed below this is the road's ordinary give. */
	constexpr float BumpThresholdMps = 0.3f;
	/** A landing or a kerb strike at this speed is the hardest thump. */
	constexpr float BumpFullMps = 3.0f;
	/** Contact below this closing speed is two cars leaning on each other. */
	constexpr float ImpactThresholdMps = 0.5f;
	/** Closing speed for the hardest hit. */
	constexpr float ImpactFullMps = 10.0f;

	constexpr float ThumpDecaySeconds = 0.08f;
	constexpr float ImpactDecaySeconds = 0.22f;
	constexpr float ShiftDecaySeconds = 0.05f;
	constexpr float ShiftKick = 0.35f;

	float Ramp(float Value, float From, float To)
	{
		return FMath::Clamp((Value - From) / (To - From), 0.0f, 1.0f);
	}

	float Slide(float SlipAngle)
	{
		return Ramp(FMath::Abs(SlipAngle), SlideOnset, SlideFull);
	}

	/**
	 * Adds an effect to a motor the way independent shakes combine: two halves
	 * make three quarters, and nothing makes more than full. The gain goes on
	 * each effect, so turning the strength up saturates effects one at a time
	 * instead of clipping the whole mix.
	 */
	void Add(float& Motor, float Effect, float Gain)
	{
		const float Scaled = FMath::Clamp(Effect * Gain, 0.0f, 1.0f);
		Motor = 1.0f - (1.0f - Motor) * (1.0f - Scaled);
	}

	/** On for the first half of every cycle. */
	bool SquareWave(float Cycles)
	{
		return FMath::Frac(Cycles) < 0.5f;
	}

	/** xorshift32 into [0, 1). Deterministic, so the tests can rely on it. */
	float NextNoise(uint32& Seed)
	{
		Seed ^= Seed << 13;
		Seed ^= Seed >> 17;
		Seed ^= Seed << 5;
		return static_cast<float>(Seed & 0xFFFFFF) / static_cast<float>(0x1000000);
	}
}

namespace ApexFfb
{
	FSignals MakeSignals(const FApexDriverFeedback& Feedback, float SpeedMps, int32 Gear, bool bNewMessage)
	{
		using W = FApexDriverFeedback;
		const FApexWheelFeedback* Wheels = Feedback.Wheels;

		FSignals Signals;
		Signals.bActive = true;
		Signals.SpeedMps = SpeedMps;
		Signals.Gear = Gear;
		// The wire has the server's steering sign, positive to the left.
		Signals.SteerTorque = Feedback.SteerTorque.Num() > 0 ? -Feedback.SteerTorque.Last() : 0.0f;

		Signals.FrontSlide = FMath::Max(Slide(Wheels[W::FrontLeft].SlipAngle), Slide(Wheels[W::FrontRight].SlipAngle));
		Signals.RearSlide = FMath::Max(Slide(Wheels[W::RearLeft].SlipAngle), Slide(Wheels[W::RearRight].SlipAngle));

		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			const float SlipRatio = Wheels[Wheel].SlipRatio;
			Signals.Lockup = FMath::Max(Signals.Lockup,
				Ramp(-SlipRatio, WheelSlipOnset, WheelSlipOnset + LockupFullExcess));
			Signals.Wheelspin = FMath::Max(Signals.Wheelspin,
				Ramp(SlipRatio, WheelSlipOnset, WheelSlipOnset + WheelspinFullExcess));
			Signals.OffTrack += Wheels[Wheel].Surface == EApexContactSurface::Off ? 0.25f : 0.0f;
			if (bNewMessage)
			{
				Signals.BumpMps = FMath::Max(Signals.BumpMps, FMath::Abs(Wheels[Wheel].SuspensionMps));
			}
		}
		Signals.bAbs = Feedback.bAbsActive;
		Signals.bTractionControl = Feedback.bTcActive;

		auto OnCurb = [Wheels](int32 Wheel) { return Wheels[Wheel].Surface == EApexContactSurface::Curb ? 0.5f : 0.0f; };
		Signals.CurbLeft = OnCurb(W::FrontLeft) + OnCurb(W::RearLeft);
		Signals.CurbRight = OnCurb(W::FrontRight) + OnCurb(W::RearRight);

		if (bNewMessage)
		{
			Signals.ImpactMps = Feedback.ImpactMps;
		}
		return Signals;
	}

	float GainFromStrength(float Strength01)
	{
		return 2.0f * FMath::Clamp(Strength01, 0.0f, 1.0f);
	}

	FRumble MixGamepad(const FSignals& Signals, FGamepadState& State, float DeltaSeconds, float Gain)
	{
		const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);

		// Hits decay first, so one landing this frame starts at full strength.
		State.Thump *= FMath::Exp(-Dt / ThumpDecaySeconds);
		State.Impact *= FMath::Exp(-Dt / ImpactDecaySeconds);
		State.ShiftKick *= FMath::Exp(-Dt / ShiftDecaySeconds);

		FRumble Out;
		if (Signals.bActive)
		{
			const float Speed = Signals.SpeedMps;
			const float SlideGate = Ramp(Speed, 0.0f, SlideFullSpeedMps);
			const float Texture = Ramp(Speed, CrawlSpeedMps, TextureFullSpeedMps);

			// Tyres. Scrub from the fronts is a buzz (the light motor), the rear
			// stepping out is a heave (the heavy one), so understeer and
			// oversteer never feel alike.
			Add(Out.High, 0.55f * Signals.FrontSlide * SlideGate, Gain);
			Add(Out.Low, 0.10f * Signals.FrontSlide * SlideGate, Gain);
			Add(Out.Low, 0.60f * Signals.RearSlide * SlideGate, Gain);
			Add(Out.High, 0.15f * Signals.RearSlide * SlideGate, Gain);
			Add(Out.High, 0.75f * Signals.Lockup * SlideGate, Gain);
			Add(Out.Low, 0.25f * Signals.Lockup * SlideGate, Gain);
			Add(Out.High, 0.50f * Signals.Wheelspin, Gain);

			// ABS and traction control as pulse trains: they are the car
			// catching a wheel, which a steady rumble would not say. Real ABS
			// switches off at walking pace; so does this.
			State.PulseClock = FMath::Fmod(State.PulseClock + Dt, 1.0f);
			if (Signals.bAbs && Speed > CrawlSpeedMps && SquareWave(State.PulseClock * AbsPulseHz))
			{
				Add(Out.Low, 0.45f, Gain);
				Add(Out.High, 0.20f, Gain);
			}
			if (Signals.bTractionControl && SquareWave(State.PulseClock * TractionPulseHz))
			{
				Add(Out.High, 0.30f, Gain);
			}

			// Curbs: ribs under the tyre at a rate set by speed, over a floor
			// of rumble so the strip is felt even where the pulses blur.
			const float Curb = FMath::Max(Signals.CurbLeft, Signals.CurbRight);
			if (Curb > 0.0f && Speed > CrawlSpeedMps)
			{
				const float Hz = FMath::Clamp(Speed / CurbRibSpacingM, CurbMinHz, CurbMaxHz);
				State.CurbPhase = FMath::Frac(State.CurbPhase + Dt * Hz);
				const float Level = Curb * (0.5f + 0.5f * Texture);
				Add(Out.Low, 0.15f * Level, Gain);
				if (State.CurbPhase < 0.5f)
				{
					Add(Out.Low, 0.45f * Level, Gain);
					Add(Out.High, 0.35f * Level, Gain);
				}
			}

			// Grass and gravel: an uneven rumble that grows with speed.
			if (Signals.OffTrack > 0.0f && Speed > 0.5f * CrawlSpeedMps)
			{
				State.NoiseClock += Dt;
				if (State.NoiseClock >= 1.0f / NoiseHz || State.NoiseLevel == 0.0f)
				{
					State.NoiseClock = FMath::Fmod(State.NoiseClock, 1.0f / NoiseHz);
					State.NoiseLevel = 0.4f + 0.6f * NextNoise(State.NoiseSeed);
				}
				const float Level = Signals.OffTrack * (0.25f + 0.75f * Texture) * State.NoiseLevel;
				Add(Out.Low, 0.55f * Level, Gain);
				Add(Out.High, 0.20f * Level, Gain);
			}

			// Hits.
			if (Signals.BumpMps > BumpThresholdMps)
			{
				State.Thump = FMath::Max(State.Thump,
					0.8f * FMath::Max(0.15f, Ramp(Signals.BumpMps, BumpThresholdMps, BumpFullMps)));
			}
			if (Signals.ImpactMps > ImpactThresholdMps)
			{
				State.Impact = FMath::Max(State.Impact,
					FMath::Max(0.3f, Ramp(Signals.ImpactMps, 0.0f, ImpactFullMps)));
			}
			if (State.LastGear != INDEX_NONE && Signals.Gear != State.LastGear)
			{
				State.ShiftKick = ShiftKick;
			}
			State.LastGear = Signals.Gear;
		}
		else
		{
			// The next car seen is not a gear change.
			State.LastGear = INDEX_NONE;
		}

		Add(Out.Low, State.Thump, Gain);
		Add(Out.Low, State.Impact, Gain);
		Add(Out.High, 0.7f * State.Impact, Gain);
		Add(Out.Low, State.ShiftKick, Gain);
		return Out;
	}
}

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

	// --- The wheel ------------------------------------------------------------

	/**
	 * Torque at the strength the effects were designed at (0.5), as a share of
	 * the base's peak: a car at the front axle's static grip limit. The rest is
	 * headroom, because downforce takes the torque past 1 on a fast corner.
	 */
	constexpr float WheelTorqueReference = 0.6f;

	/** Where the torque stops being linear and starts being compressed. */
	constexpr float WheelSoftKnee = 0.75f;

	/** One pole over the 60 Hz message steps; long enough to smooth, short enough not to lag. */
	constexpr float WheelTorqueSmoothingSeconds = 0.012f;

	/** Hits die away a little more slowly on a rim than on a motor. */
	constexpr float WheelBumpDecaySeconds = 0.09f;
	constexpr float WheelImpactDecaySeconds = 0.25f;

	/** Damping is heaviest at a standstill and mostly gone by here. */
	constexpr float WheelDamperFullSpeedMps = 18.0f;

	/** Centring while no car is being driven, so the rim does not flop about in the menus. */
	constexpr float WheelMenuSpring = 0.3f;

	/**
	 * Softly limits the torque instead of clipping it.
	 *
	 * Linear to the knee, then closing on 1: a car pulling twice its reference
	 * torque still feels stronger than one pulling one and a half times it,
	 * which flat clipping would throw away — and that difference is exactly
	 * what tells a driver how loaded the front axle is.
	 */
	float SoftLimit(float Value)
	{
		const float Magnitude = FMath::Abs(Value);
		if (Magnitude <= WheelSoftKnee)
		{
			return Value;
		}
		const float Over = (Magnitude - WheelSoftKnee) / (1.0f - WheelSoftKnee);
		return FMath::Sign(Value) * (WheelSoftKnee + (1.0f - WheelSoftKnee) * (Over / (1.0f + Over)));
	}

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

	FApexWheelEffects MixWheel(
		const FSignals& Signals, FWheelState& State, float DeltaSeconds, const FWheelTuning& Tuning)
	{
		const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);

		State.Bump *= FMath::Exp(-Dt / WheelBumpDecaySeconds);
		State.Impact *= FMath::Exp(-Dt / WheelImpactDecaySeconds);
		State.ShiftKick *= FMath::Exp(-Dt / ShiftDecaySeconds);

		// The torque, smoothed towards where the car says it should be. With no
		// car it runs down to nothing rather than being dropped, which would be
		// a wheel let go of mid-corner.
		const float Target = Signals.bActive ? Signals.SteerTorque : 0.0f;
		State.Torque = FMath::Lerp(State.Torque, Target, 1.0f - FMath::Exp(-Dt / WheelTorqueSmoothingSeconds));

		FApexWheelEffects Out;
		const float Force = SoftLimit(State.Torque * 2.0f * FMath::Clamp(Tuning.Force, 0.0f, 1.0f) * WheelTorqueReference);
		Out.Constant = Tuning.bInvert ? -Force : Force;

		if (Signals.bActive)
		{
			if (Signals.BumpMps > BumpThresholdMps)
			{
				State.Bump = FMath::Max(State.Bump,
					0.7f * FMath::Max(0.15f, Ramp(Signals.BumpMps, BumpThresholdMps, BumpFullMps)));
			}
			if (Signals.ImpactMps > ImpactThresholdMps)
			{
				State.Impact = FMath::Max(State.Impact, FMath::Max(0.35f, Ramp(Signals.ImpactMps, 0.0f, ImpactFullMps)));
			}
			if (State.LastGear != INDEX_NONE && Signals.Gear != State.LastGear)
			{
				State.ShiftKick = ShiftKick;
			}
			State.LastGear = Signals.Gear;
		}
		else
		{
			State.LastGear = INDEX_NONE;
		}

		// One vibration channel, so the loudest voice takes it.
		struct FTexture
		{
			float Amplitude = 0.0f;
			float Hz = 0.0f;
		};
		FTexture Texture;
		auto Louder = [&Texture](float Amplitude, float Hz)
		{
			if (Amplitude > Texture.Amplitude)
			{
				Texture.Amplitude = Amplitude;
				Texture.Hz = Hz;
			}
		};

		if (Signals.bActive)
		{
			const float Speed = Signals.SpeedMps;
			const float Road = Ramp(Speed, CrawlSpeedMps, TextureFullSpeedMps);

			const float Curb = FMath::Max(Signals.CurbLeft, Signals.CurbRight);
			if (Curb > 0.0f && Speed > CrawlSpeedMps)
			{
				Louder(0.55f * Curb * (0.4f + 0.6f * Road),
					FMath::Clamp(Speed / CurbRibSpacingM, CurbMinHz, CurbMaxHz));
			}

			if (Signals.OffTrack > 0.0f && Speed > 0.5f * CrawlSpeedMps)
			{
				State.NoiseClock += Dt;
				if (State.NoiseClock >= 1.0f / NoiseHz || State.NoiseLevel == 0.0f)
				{
					State.NoiseClock = FMath::Fmod(State.NoiseClock, 1.0f / NoiseHz);
					State.NoiseLevel = 0.4f + 0.6f * NextNoise(State.NoiseSeed);
				}
				// Grass is uneven in how hard it hits and in how fast it comes.
				Louder(0.45f * Signals.OffTrack * (0.3f + 0.7f * Road) * State.NoiseLevel,
					22.0f + 18.0f * State.NoiseLevel);
			}

			// The car catching a wheel, felt through the column as it is in a
			// real car with the brakes cycling.
			if (Signals.bAbs && Speed > CrawlSpeedMps)
			{
				Louder(0.35f, AbsPulseHz);
			}
			if (Signals.bTractionControl)
			{
				Louder(0.2f, TractionPulseHz);
			}
			// A locked or spinning tyre judders faster than the road does.
			Louder(0.3f * Signals.Lockup, 45.0f);
			Louder(0.2f * Signals.Wheelspin, 32.0f);
		}

		Louder(State.Bump, 14.0f);
		Louder(State.Impact, 9.0f);
		Louder(0.8f * State.ShiftKick, 26.0f);

		Out.VibrationAmplitude = FMath::Clamp(Texture.Amplitude * 2.0f * FMath::Clamp(Tuning.RoadEffects, 0.0f, 1.0f), 0.0f, 1.0f);
		Out.VibrationHz = Texture.Hz;

		// Heavy at a standstill, where a real car's steering is heavy and where
		// a wheel with nothing to push against would otherwise spin freely.
		const float Parked = 1.0f - Ramp(Signals.SpeedMps, 0.0f, WheelDamperFullSpeedMps);
		Out.Damper = FMath::Clamp(Tuning.Damping, 0.0f, 1.0f) * (0.35f + 0.65f * Parked);

		// Only in the menus, and only if forces are on at all: a rim that
		// flops to one side while the player picks a car feels broken.
		Out.Spring = Signals.bActive ? 0.0f : WheelMenuSpring * FMath::Clamp(2.0f * Tuning.Force, 0.0f, 1.0f);
		return Out;
	}
}

#include "Audio/ApexRoadSound.h"

namespace ApexRoadSynth
{
	namespace
	{
		/** Rib pitch of a kerb, metres: the force feedback's figure, so hand and ear agree. */
		constexpr float CurbRibSpacingM = 0.9f;

		/** Below this the car is creeping: nothing scrubs, nothing drones. */
		constexpr float CrawlSpeedMps = 2.0f;

		/** Levels ease to their targets over this long. */
		constexpr float SmoothingSeconds = 0.04f;

		/**
		 * When a tyre makes a noise, in multiples of its peak slip (what the
		 * server's feedback is in). A car cornering *well* sits right at the peak
		 * slip angle, corner after corner, in silence: rubber only howls once it
		 * is properly sliding. The force feedback starts its scrub at 0.9 — a
		 * rumble motor hinting at the limit — and driven from that, this
		 * screeched at every turn of the wheel.
		 */
		constexpr float SquealOnsetSlipAngle = 1.25f;
		constexpr float SquealFullSlipAngle = 2.4f;
		/** ABS and traction control hold a wheel at 1; a wheel has to be well past it to be locked or lit up. */
		constexpr float SkidOnsetSlipRatio = 1.6f;
		constexpr float LockupFullSlipRatio = 5.0f;
		constexpr float WheelspinFullSlipRatio = 3.5f;

		/**
		 * A slide has to last to be heard. The feedback keeps the *peak* slip
		 * between messages, so one tick over a bump arrives as a whole frame of
		 * sliding; rising this slowly, that is nothing, and a real slide is all
		 * there within a couple of tenths. It stops rather faster than it starts.
		 */
		constexpr float SlideRiseSeconds = 0.14f;
		constexpr float SlideFallSeconds = 0.07f;

		/** Front and rear squeal at rest, Hz: a major third apart. */
		constexpr float SquealBaseHz[2] = {760.0f, 600.0f};

		constexpr float OutputGain = 1.0f;

		/**
		 * Suspension speed below which a hit is the road's ordinary texture, and
		 * the speed of a full-size one: the force feedback's thresholds, so what
		 * thumps the pad is what thuds in the ear.
		 */
		constexpr float BumpThresholdMps = 0.3f;
		constexpr float BumpFullMps = 3.0f;

		float NextNoise(uint32& State)
		{
			if (State == 0)
			{
				State = 0x6A09E667u;
			}
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			return (static_cast<float>(State) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
		}

		float AlphaFor(float Seconds, float SampleRate)
		{
			return 1.0f - FMath::Exp(-1.0f / (FMath::Max(Seconds, 1.0e-5f) * SampleRate));
		}

		float AlphaForHz(float Hz, float SampleRate)
		{
			return 1.0f - FMath::Exp(-2.0f * PI * FMath::Min(Hz, SampleRate * 0.45f) / SampleRate);
		}

		/** Coefficients of a two-pole resonance at Hz whose ring dies away in about DecaySeconds. */
		struct FResonance
		{
			float A1 = 0.0f;
			float A2 = 0.0f;
			/** Input scale for noise: it comes out with the power it went in with, whatever the tuning. */
			float NoiseGain = 0.0f;
			/** Input scale for a single-sample impulse: the ring it starts peaks at the impulse's size. */
			float ImpulseGain = 0.0f;

			FResonance(float Hz, float DecaySeconds, float SampleRate)
			{
				const float R = FMath::Exp(-1.0f / (FMath::Max(DecaySeconds, 1.0e-4f) * SampleRate));
				const float Theta = 2.0f * PI * FMath::Min(Hz, SampleRate * 0.45f) / SampleRate;
				const float R2 = R * R;
				A1 = 2.0f * R * FMath::Cos(Theta);
				A2 = -R2;
				NoiseGain = FMath::Sqrt((1.0f - R2) * (1.0f - 2.0f * R2 * FMath::Cos(2.0f * Theta) + R2 * R2) / (1.0f + R2));
				ImpulseGain = FMath::Sin(Theta);
			}

			/** `Input` already scaled by NoiseGain or ImpulseGain. */
			float Tick(FResonator& State, float Input) const
			{
				const float Out = A1 * State.Y1 + A2 * State.Y2 + Input;
				State.Y2 = State.Y1;
				State.Y1 = Out;
				return Out;
			}
		};

		/** 0 below Low, 1 above High, smooth between. */
		float Gate(float Value, float Low, float High)
		{
			const float T = FMath::Clamp((Value - Low) / (High - Low), 0.0f, 1.0f);
			return T * T * (3.0f - 2.0f * T);
		}

		float SoftClip(float Value)
		{
			const float X = FMath::Clamp(Value, -1.5f, 1.5f);
			return X - X * X * X / 6.75f;
		}

		float Ramp(float Value, float From, float To)
		{
			return FMath::Clamp((Value - From) / (To - From), 0.0f, 1.0f);
		}

		/** What a slow one-pole leaves of ±1 white noise, inverted: brings it back to about ±1. */
		float SlowNoiseScale(float Alpha)
		{
			return 1.0f / (0.58f * FMath::Sqrt(FMath::Max(Alpha, 1.0e-9f) * 0.5f));
		}

		/**
		 * A tyre's howl. Sliding rubber sticks and slips at a rate of its own, so
		 * a squeal is a *tone* — a fundamental and a few falling harmonics —
		 * whose pitch wanders a few percent and whose level flutters with the
		 * road's grain. (It used to be noise through a narrow resonance: a
		 * wavering whistle over hiss, which is a shortwave radio, and was
		 * taken for the wind.) `Roughness` is how deep the flutter goes: a
		 * squeal sings, a locked wheel grinds.
		 */
		float TickHowl(FHowl& Voice, float Hz, float Roughness, float Noise, float Dt,
			float WanderAlpha, float WanderScale, float FlutterAlpha, float FlutterScale)
		{
			Voice.Wander += (Noise - Voice.Wander) * WanderAlpha;
			Voice.Flutter += (Noise - Voice.Flutter) * FlutterAlpha;
			const float Pitch = Hz * (1.0f + 0.03f * FMath::Clamp(Voice.Wander * WanderScale, -1.5f, 1.5f));
			Voice.Phase += Pitch * Dt;
			Voice.Phase -= static_cast<float>(static_cast<int32>(Voice.Phase));
			const float Angle = 2.0f * PI * Voice.Phase;
			const float Tone = FMath::Sin(Angle) + 0.45f * FMath::Sin(2.0f * Angle) + 0.22f * FMath::Sin(3.0f * Angle)
				+ 0.1f * FMath::Sin(4.0f * Angle);
			const float Level = 1.0f + Roughness * FMath::Clamp(Voice.Flutter * FlutterScale, -1.0f, 1.0f);
			return Tone * 0.6f * FMath::Max(Level, 0.0f);
		}
	}

	float SquealFromSlipAngle(float SlipAngleInPeaks)
	{
		return Ramp(FMath::Abs(SlipAngleInPeaks), SquealOnsetSlipAngle, SquealFullSlipAngle);
	}

	float LockupFromSlipRatio(float SlipRatioInPeaks)
	{
		return Ramp(-SlipRatioInPeaks, SkidOnsetSlipRatio, LockupFullSlipRatio);
	}

	float WheelspinFromSlipRatio(float SlipRatioInPeaks)
	{
		return Ramp(SlipRatioInPeaks, SkidOnsetSlipRatio, WheelspinFullSlipRatio);
	}

	void Hit(FState& State, float BumpMps, float ImpactMps)
	{
		if (BumpMps > BumpThresholdMps)
		{
			// An impulse into the thud's resonance; a kerb strike is ~0.5 m/s, a landing 2.
			const float Size = FMath::Clamp((BumpMps - BumpThresholdMps) / (BumpFullMps - BumpThresholdMps), 0.15f, 1.0f);
			State.PendingThud += 0.9f * Size;
		}
		if (ImpactMps > 0.0f)
		{
			const float Size = FMath::Clamp(ImpactMps / 12.0f, 0.0f, 1.0f);
			State.CrunchEnvelope = FMath::Max(State.CrunchEnvelope, 0.25f + 0.75f * Size);
			State.PendingThud += Size;
		}
	}

	float CurbRibHz(float SpeedMps)
	{
		return FMath::Max(SpeedMps, 0.0f) / CurbRibSpacingM;
	}

	float SquealHz(int32 Axle, float SpeedMps, float Slide)
	{
		// Faster scrubbing sings higher; so does a tyre further past its peak.
		const float Speed = FMath::Clamp(SpeedMps / 60.0f, 0.0f, 1.0f);
		return SquealBaseHz[Axle == 0 ? 0 : 1] * (0.85f + 0.3f * Speed) * (1.0f + 0.12f * FMath::Clamp(Slide, 0.0f, 1.0f));
	}

	void Render(FState& State, const FInputs& Inputs, float SampleRate, float* OutFrames, int32 NumFrames)
	{
		if (SampleRate <= 0.0f || NumFrames <= 0)
		{
			return;
		}

		FInputs& S = State.Smoothed;
		S.bWet = Inputs.bWet;
		const float InputAlpha = AlphaFor(SmoothingSeconds, SampleRate);
		const float Dt = 1.0f / SampleRate;

		// Filters are placed once per buffer from where the levels stand: a
		// buffer is a few milliseconds and none of these move far in one.
		const float Speed = FMath::Max(S.SpeedMps, 0.0f);
		const float Wet = S.bWet ? 1.0f : 0.0f;
		const float FrontHz = SquealHz(0, Speed, S.FrontSlide);
		const float RearHz = SquealHz(1, Speed, S.RearSlide);
		const float LockHz = 430.0f + 120.0f * FMath::Clamp(Speed / 50.0f, 0.0f, 1.0f);
		const float SpinHz = 560.0f + 420.0f * S.Wheelspin;
		// The pitch wanders over a quarter of a second; the level flutters at the road's grain, tens of Hz.
		const float WanderAlpha = AlphaFor(0.25f, SampleRate);
		const float WanderScale = SlowNoiseScale(WanderAlpha);
		const float FlutterAlpha = AlphaForHz(35.0f, SampleRate);
		const float FlutterScale = SlowNoiseScale(FlutterAlpha);
		// Under the howl, rubber scrubbing: a band of "shh" in the low mids, behind
		// two poles. Never white noise: gated by a slide, that is static.
		const float ScrubHighAlpha = AlphaForHz(1400.0f, SampleRate);
		const float ScrubLowAlpha = AlphaForHz(350.0f, SampleRate);
		const float SlideRise = AlphaFor(SlideRiseSeconds, SampleRate);
		const float SlideFall = AlphaFor(SlideFallSeconds, SampleRate);
		const FResonance CurbThud(95.0f, 0.03f, SampleRate);
		const FResonance Stones(2600.0f, 0.0012f, SampleRate);
		const FResonance BumpThud(68.0f, 0.06f, SampleRate);

		const float RumbleAlpha = AlphaForHz(70.0f + 3.0f * Speed, SampleRate);
		// Rolling and wind are both noise, and what keeps noise from sounding
		// like a detuned radio is where it sits and how it moves: low, behind
		// two poles so there is no hiss tail, and steady. The first version was
		// white noise band-passed to 350-4000 Hz with its level jittering several
		// times a second — which is static, fading in and out. Wind at speed is
		// mostly buffeting under 200 Hz, with a soft rush in the low mids that
		// swells over seconds, not tenths.
		const float RollAlpha = AlphaForHz(90.0f + 2.5f * Speed, SampleRate);
		const float BuffetAlpha = AlphaForHz(70.0f + 1.2f * Speed, SampleRate);
		const float RushLowAlpha = AlphaForHz(250.0f, SampleRate);
		const float RushHighAlpha = AlphaForHz(700.0f + 4.0f * Speed, SampleRate);
		const float SprayAlpha = AlphaForHz(3000.0f, SampleRate);
		const float SlapDecay = 1.0f - AlphaFor(0.004f, SampleRate);
		const float CrunchDecay = 1.0f - AlphaFor(0.11f, SampleRate);
		const float CrunchAlpha = AlphaForHz(1400.0f, SampleRate);
		const float GustAlpha = AlphaFor(2.5f, SampleRate);
		// Noise behind a pole that slow is tiny: this brings it back to about ±1.
		const float GustScale = 0.6f * FMath::Sqrt(2.5f * SampleRate);

		// Rubber sings on a dry road and only hisses on a wet one.
		const float Sing = 1.0f - 0.8f * Wet;
		const float Scrub = Gate(Speed, 5.0f, 15.0f);
		const float Moving = Gate(Speed, CrawlSpeedMps, 10.0f);
		const float RibStep = CurbRibHz(Speed) * Dt;
		// Stones per sample: a few dozen a second at speed, fully off the road.
		const float StoneChance = S.OffTrack * FMath::Clamp(Speed / 30.0f, 0.0f, 1.0f) * 60.0f * Dt;

		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			S.SpeedMps += (Inputs.SpeedMps - S.SpeedMps) * InputAlpha;
			S.FrontSlide += (Inputs.FrontSlide - S.FrontSlide) * (Inputs.FrontSlide > S.FrontSlide ? SlideRise : SlideFall);
			S.RearSlide += (Inputs.RearSlide - S.RearSlide) * (Inputs.RearSlide > S.RearSlide ? SlideRise : SlideFall);
			S.Lockup += (Inputs.Lockup - S.Lockup) * (Inputs.Lockup > S.Lockup ? SlideRise : SlideFall);
			S.Wheelspin += (Inputs.Wheelspin - S.Wheelspin) * (Inputs.Wheelspin > S.Wheelspin ? SlideRise : SlideFall);
			S.CurbLeft += (Inputs.CurbLeft - S.CurbLeft) * InputAlpha;
			S.CurbRight += (Inputs.CurbRight - S.CurbRight) * InputAlpha;
			S.OffTrack += (Inputs.OffTrack - S.OffTrack) * InputAlpha;

			const float Noise = NextNoise(State.NoiseState);
			const float Noise2 = NextNoise(State.NoiseState);
			// Grass does not squeal: what is off the road only rumbles.
			const float OnTarmac = 1.0f - S.OffTrack;

			// --- Squeal ----------------------------------------------------------
			const float FrontLevel = S.FrontSlide * FMath::Sqrt(S.FrontSlide) * Scrub * OnTarmac;
			const float RearLevel = S.RearSlide * FMath::Sqrt(S.RearSlide) * Scrub * OnTarmac;
			const float LockLevel = S.Lockup * Scrub * OnTarmac;
			const float SpinLevel = S.Wheelspin * OnTarmac;
			float Tyres = 0.0f;
			// A silent voice is not run: its phase and wander can wait.
			if (FrontLevel > 0.001f)
			{
				Tyres += TickHowl(State.FrontHowl, FrontHz, 0.35f, Noise, Dt, WanderAlpha, WanderScale, FlutterAlpha, FlutterScale)
					* FrontLevel * 0.2f * Sing;
			}
			if (RearLevel > 0.001f)
			{
				Tyres += TickHowl(State.RearHowl, RearHz, 0.35f, Noise2, Dt, WanderAlpha, WanderScale, FlutterAlpha, FlutterScale)
					* RearLevel * 0.2f * Sing;
			}
			if (LockLevel > 0.001f)
			{
				Tyres += TickHowl(State.LockHowl, LockHz, 0.9f, Noise2, Dt, WanderAlpha, WanderScale, FlutterAlpha, FlutterScale)
					* LockLevel * (0.2f - 0.1f * Wet);
			}
			if (SpinLevel > 0.001f)
			{
				Tyres += TickHowl(State.SpinHowl, SpinHz, 0.6f, Noise, Dt, WanderAlpha, WanderScale, FlutterAlpha, FlutterScale)
					* SpinLevel * (0.14f - 0.06f * Wet);
			}
			// What does not sing still scrubs, and in the wet it is all there is.
			State.ScrubHigh += (Noise - State.ScrubHigh) * ScrubHighAlpha;
			State.ScrubHigh2 += (State.ScrubHigh - State.ScrubHigh2) * ScrubHighAlpha;
			State.ScrubLow += (State.ScrubHigh2 - State.ScrubLow) * ScrubLowAlpha;
			Tyres += (State.ScrubHigh2 - State.ScrubLow) * (FrontLevel + RearLevel + LockLevel + SpinLevel) * (0.12f + 0.3f * Wet);

			// --- Kerbs -----------------------------------------------------------
			float RibHit = 0.0f;
			const float Sides[2] = {S.CurbLeft, S.CurbRight};
			for (int32 Side = 0; Side < 2; ++Side)
			{
				State.CurbPhase[Side] += RibStep;
				if (State.CurbPhase[Side] >= 1.0f)
				{
					State.CurbPhase[Side] -= 1.0f;
					RibHit += Sides[Side] * Moving;
				}
			}
			State.CurbSlap = State.CurbSlap * SlapDecay + RibHit;
			const float Curb = CurbThud.Tick(State.CurbThud, RibHit * CurbThud.ImpulseGain) * 0.24f + State.CurbSlap * Noise * 0.22f;

			// --- Off the road ----------------------------------------------------
			State.RumbleLowpass += (Noise2 - State.RumbleLowpass) * RumbleAlpha;
			const float Stone = NextNoise(State.NoiseState) * 0.5f + 0.5f < StoneChance ? Noise * 4.0f : 0.0f;
			const float Rough = State.RumbleLowpass * S.OffTrack * Moving * 2.2f + Stones.Tick(State.Stones, Stone * Stones.ImpulseGain) * 0.12f;

			// --- Hits ------------------------------------------------------------
			State.CrunchEnvelope *= CrunchDecay;
			State.CrunchLowpass += (Noise - State.CrunchLowpass) * CrunchAlpha;
			const float Hits = BumpThud.Tick(State.BumpThud, State.PendingThud * BumpThud.ImpulseGain) * 0.6f + State.CrunchLowpass * State.CrunchEnvelope * 1.6f;
			State.PendingThud = 0.0f;

			// --- Rolling and wind ------------------------------------------------
			const float SpeedShare = FMath::Clamp(S.SpeedMps / 85.0f, 0.0f, 1.2f);
			State.RollLowpass += (Noise - State.RollLowpass) * RollAlpha;
			State.RollLowpass2 += (State.RollLowpass - State.RollLowpass2) * RollAlpha;
			State.Buffet += (Noise2 - State.Buffet) * BuffetAlpha;
			State.Buffet2 += (State.Buffet - State.Buffet2) * BuffetAlpha;
			// The rush: two poles above, one below, so it has no top and no thump.
			State.WindHigh += (Noise2 - State.WindHigh) * RushHighAlpha;
			State.WindHigh2 += (State.WindHigh - State.WindHigh2) * RushHighAlpha;
			State.WindLow += (State.WindHigh2 - State.WindLow) * RushLowAlpha;
			State.Gust += (Noise - State.Gust) * GustAlpha;
			State.SprayLowpass += (Noise - State.SprayLowpass) * SprayAlpha;
			const float Swell = FMath::Clamp(1.0f + 0.25f * State.Gust * GustScale, 0.6f, 1.4f);
			const float Roll = State.RollLowpass2 * SpeedShare * FMath::Sqrt(SpeedShare) * 0.3f * OnTarmac;
			const float Wind = (State.Buffet2 * 0.5f + (State.WindHigh2 - State.WindLow) * 0.05f)
				* SpeedShare * SpeedShare * Swell;
			const float Spray = (Noise - State.SprayLowpass) * Wet * SpeedShare * 0.10f * OnTarmac;

			OutFrames[Index] = SoftClip((Tyres + Curb + Rough + Hits + Roll + Wind + Spray) * OutputGain);
		}
	}
}

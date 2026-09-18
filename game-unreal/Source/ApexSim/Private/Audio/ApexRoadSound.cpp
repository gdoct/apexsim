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

		/** Front and rear squeal at rest, Hz: a major third apart. */
		constexpr float SquealBaseHz[2] = {1040.0f, 830.0f};

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
		const FResonance FrontSqueal(SquealHz(0, Speed, S.FrontSlide), 0.012f, SampleRate);
		const FResonance FrontSquealUpper(SquealHz(0, Speed, S.FrontSlide) * 2.07f, 0.006f, SampleRate);
		const FResonance RearSqueal(SquealHz(1, Speed, S.RearSlide), 0.012f, SampleRate);
		const FResonance RearSquealUpper(SquealHz(1, Speed, S.RearSlide) * 2.07f, 0.006f, SampleRate);
		const FResonance LockSkid(480.0f + 140.0f * FMath::Clamp(Speed / 50.0f, 0.0f, 1.0f), 0.003f, SampleRate);
		const FResonance SpinChirp(620.0f + 500.0f * S.Wheelspin, 0.005f, SampleRate);
		const FResonance CurbThud(95.0f, 0.03f, SampleRate);
		const FResonance Stones(2600.0f, 0.0012f, SampleRate);
		const FResonance BumpThud(68.0f, 0.06f, SampleRate);

		const float RumbleAlpha = AlphaForHz(70.0f + 3.0f * Speed, SampleRate);
		const float RollAlpha = AlphaForHz(140.0f + 5.0f * Speed, SampleRate);
		const float WindLowAlpha = AlphaForHz(350.0f, SampleRate);
		const float WindHighAlpha = AlphaForHz(2200.0f + 20.0f * Speed, SampleRate);
		const float SprayAlpha = AlphaForHz(3000.0f, SampleRate);
		const float SlapDecay = 1.0f - AlphaFor(0.004f, SampleRate);
		const float CrunchDecay = 1.0f - AlphaFor(0.11f, SampleRate);
		const float CrunchAlpha = AlphaForHz(1400.0f, SampleRate);
		const float GustAlpha = AlphaFor(0.8f, SampleRate);

		// Rubber sings on a dry road and only hisses on a wet one.
		const float Sing = 1.0f - 0.8f * Wet;
		const float Scrub = Gate(Speed, 3.0f, 12.0f);
		const float Moving = Gate(Speed, CrawlSpeedMps, 10.0f);
		const float RibStep = CurbRibHz(Speed) * Dt;
		// Stones per sample: a few dozen a second at speed, fully off the road.
		const float StoneChance = S.OffTrack * FMath::Clamp(Speed / 30.0f, 0.0f, 1.0f) * 60.0f * Dt;

		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			S.SpeedMps += (Inputs.SpeedMps - S.SpeedMps) * InputAlpha;
			S.FrontSlide += (Inputs.FrontSlide - S.FrontSlide) * InputAlpha;
			S.RearSlide += (Inputs.RearSlide - S.RearSlide) * InputAlpha;
			S.Lockup += (Inputs.Lockup - S.Lockup) * InputAlpha;
			S.Wheelspin += (Inputs.Wheelspin - S.Wheelspin) * InputAlpha;
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
			float Tyres = (FrontSqueal.Tick(State.FrontSqueal[0], Noise * FrontSqueal.NoiseGain)
					+ 0.4f * FrontSquealUpper.Tick(State.FrontSqueal[1], Noise2 * FrontSquealUpper.NoiseGain))
					* FrontLevel
				+ (RearSqueal.Tick(State.RearSqueal[0], Noise2 * RearSqueal.NoiseGain)
					+ 0.4f * RearSquealUpper.Tick(State.RearSqueal[1], Noise * RearSquealUpper.NoiseGain))
					* RearLevel;
			Tyres *= 0.22f * Sing;
			// What does not sing still scrubs: broadband, and all there is in the wet.
			Tyres += Noise * (FrontLevel + RearLevel) * (0.03f + 0.07f * Wet);

			Tyres += LockSkid.Tick(State.LockSkid, Noise * LockSkid.NoiseGain) * S.Lockup * Scrub * OnTarmac * (0.26f - 0.14f * Wet);
			Tyres += SpinChirp.Tick(State.SpinChirp, Noise2 * SpinChirp.NoiseGain) * S.Wheelspin * OnTarmac * (0.17f - 0.08f * Wet);

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
			State.Gust += (Noise2 - State.Gust) * GustAlpha * 0.02f;
			State.WindLow += (Noise2 - State.WindLow) * WindLowAlpha;
			State.WindHigh += (Noise2 - State.WindHigh) * WindHighAlpha;
			State.SprayLowpass += (Noise - State.SprayLowpass) * SprayAlpha;
			const float Roll = State.RollLowpass * SpeedShare * FMath::Sqrt(SpeedShare) * 0.32f * OnTarmac;
			const float Wind = (State.WindHigh - State.WindLow) * SpeedShare * SpeedShare * (0.20f + 4.0f * State.Gust);
			const float Spray = (Noise - State.SprayLowpass) * Wet * SpeedShare * 0.10f * OnTarmac;

			OutFrames[Index] = SoftClip((Tyres + Curb + Rough + Hits + Roll + Wind + Spray) * OutputGain);
		}
	}
}

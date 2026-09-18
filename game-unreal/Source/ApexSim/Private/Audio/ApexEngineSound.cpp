#include "Audio/ApexEngineSound.h"

namespace ApexEngineSynth
{
	namespace
	{
		constexpr float SpeedOfSoundMps = 343.0f;

		/** Below this the crank is not turning an engine anybody can hear. */
		constexpr float DeadEngineRpm = 100.0f;

		/**
		 * How quickly the crank follows the revs it is given. The car actor
		 * feeds the motion buffer's blended RPM every render frame, so this only
		 * has to hide the frame steps, not the 60Hz telemetry.
		 */
		constexpr float SmoothingSeconds = 0.02f;

		/**
		 * A firing is let into the pipe over this long: a couple of samples, so
		 * the edge is band-limited rather than a step. Any longer is a low-pass
		 * on the whole engine (0.25 ms is a 640 Hz corner) and takes the top off
		 * every harmonic that makes one engine sound unlike another.
		 */
		constexpr float PulseAttackSeconds = 0.00004f;

		/**
		 * The blow-down lasts a fixed share of the cycle, so its length in
		 * seconds shrinks as the revs rise: 6/RPM is ~7 ms at a 900rpm idle and
		 * 0.4 ms at 15,000. It has to stay well inside the gap between one
		 * bank's firings, or the pulses merge and the harmonics that make a
		 * high-revving engine scream are never generated.
		 */
		constexpr float PulseSecondsTimesRpm = 6.0f;
		constexpr float PulseMinSeconds = 0.0003f;
		constexpr float PulseMaxSeconds = 0.02f;

		/**
		 * What a closed throttle still burns: enough to keep an idle turning, and
		 * next to nothing on the overrun, where the injectors are shut.
		 */
		constexpr float IdleLoad = 0.30f;
		constexpr float OverrunLoad = 0.10f;

		/**
		 * Pulse size against load. A real exhaust spans some 30 dB between idle
		 * and full throttle; a game mix cannot, so the curve is flattened.
		 */
		constexpr float LoadLoudnessExponent = 0.65f;

		/** Overrun: throttle shut above this share of the rev range. */
		constexpr float OverrunThrottle = 0.15f;
		constexpr float OverrunRevShare = 0.35f;

		/** How long the pops go on after a lift, and how likely each firing is one at the start. */
		constexpr float PopBudgetSeconds = 0.9f;
		constexpr float PopChancePerFiring = 0.22f;
		constexpr float PopBurstSeconds = 0.015f;

		/** The limiter cuts the spark for half of every cycle this long. */
		constexpr float LimiterCycleSeconds = 0.045f;
		constexpr float LimiterMarginRpm = 30.0f;

		constexpr float TurboSpoolUpSeconds = 0.5f;
		constexpr float TurboSpoolDownSeconds = 1.1f;
		constexpr float BlowOffSeconds = 0.16f;

		/**
		 * The second bank's pipe against the first: see the pipes in Render. The
		 * level is how much nearer the listener is to one tailpipe than the
		 * other, and it is what lets a bank's own firing pattern be heard: with
		 * both banks equal an engine is an even pulse train whatever its crank.
		 * (Manifold back-pressure choking a pulse that follows its neighbour at
		 * 90 degrees was tried as the source of the crossplane burble; at any
		 * plausible depth it moved the half-orders by a couple of dB and partly
		 * cancelled this. It is not modelled.)
		 */
		constexpr float SecondBankLength = 1.07f;
		constexpr float SecondBankLevel = 0.6f;

		/**
		 * How much of the pulse's edge open pipes let out, as a first difference
		 * (at 48 kHz; scaled by the device rate). A blow-down pulse's harmonics
		 * fall 6 dB an octave and the difference rises by as much, so from about
		 * 48000/(2π·Rasp) Hz upward they come out level: ~1 kHz on open pipes.
		 */
		constexpr float RaspAmount = 9.0f;

		/**
		 * A harder blow-down is a steeper one: the pulse is sharpened by its own
		 * square as the load rises, which is where a full-throttle engine's
		 * upper harmonics come from.
		 */
		constexpr float PulseSteepening = 1.4f;

		/** Gas rushing out of the pipe hisses in step with the pulses; open pipes let it out. */
		constexpr float FlowNoise = 0.55f;

		/** Brings a full-throttle engine near the top of the range the soft clip leaves clean. */
		constexpr float OutputGain = 0.3f;

		/**
		 * Driving-gear tooth counts, first to eighth: the mesh frequency is the
		 * input shaft's speed times these. Invented, but shaped like a real
		 * ladder — more teeth on the driving gear as the ratio gets taller.
		 */
		constexpr float GearTeeth[] = {17.0f, 20.0f, 23.0f, 26.0f, 28.0f, 30.0f, 32.0f, 33.0f};

		/**
		 * A private xorshift32 generator. Not FMath::FRand: that shares state
		 * across every caller, and this one runs on the audio thread on every
		 * sample.
		 */
		float NextNoise(uint32& State)
		{
			if (State == 0)
			{
				State = 0x9E3779B9u;
			}
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			return (static_cast<float>(State) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
		}

		float NextUnit(uint32& State)
		{
			return NextNoise(State) * 0.5f + 0.5f;
		}

		/** One-pole coefficient: the share of the gap closed each sample for a time constant. */
		float AlphaFor(float Seconds, float SampleRate)
		{
			return 1.0f - FMath::Exp(-1.0f / (FMath::Max(Seconds, 1.0e-5f) * SampleRate));
		}

		/** One-pole coefficient for a cutoff in Hz. */
		float AlphaForHz(float Hz, float SampleRate)
		{
			return 1.0f - FMath::Exp(-2.0f * PI * FMath::Min(Hz, SampleRate * 0.45f) / SampleRate);
		}

		/** Slope 1 through zero, flat at ±1: a pop saturates instead of wrapping or clicking. */
		float SoftClip(float Value)
		{
			const float X = FMath::Clamp(Value, -1.5f, 1.5f);
			return X - X * X * X / 6.75f;
		}
	}

	uint32 BankMask(int32 Cylinders, bool bCrossplane)
	{
		if (bCrossplane && Cylinders == 8)
		{
			// L R L L R R L R: each bank fires at 90-180-270-180° intervals
			// instead of an even 180, which is the whole of the burble.
			return 0xB2u;
		}
		// Alternate banks. A single-bank engine's two "banks" are then the
		// two halves of its manifold, which is how a 4-2-1 or a 6-2-1 pairs them.
		return 0xAAAAAAAAu;
	}

	float NoteHz(const FEngineSpec& Spec, float Rpm)
	{
		return FMath::Max(Rpm, 0.0f) / 60.0f * static_cast<float>(FMath::Max(Spec.Cylinders, 1)) * 0.5f;
	}

	float WhineHz(float Rpm, int32 Gear)
	{
		if (Gear < 1)
		{
			// Neutral drives no pair under load; reverse is an idler nobody tunes a note for.
			return 0.0f;
		}
		const int32 Index = FMath::Min(Gear, static_cast<int32>(sizeof(GearTeeth) / sizeof(GearTeeth[0]))) - 1;
		return FMath::Max(Rpm, 0.0f) / 60.0f * GearTeeth[Index];
	}

	void Render(FState& State, const FEngineSpec& Spec, const FInputs& Inputs,
		float SampleRate, float* OutFrames, int32 NumFrames)
	{
		if (SampleRate <= 0.0f || NumFrames <= 0)
		{
			return;
		}

		const float TargetRpm = FMath::Max(Inputs.Rpm, 0.0f);
		const float TargetThrottle = FMath::Clamp(Inputs.Throttle, 0.0f, 1.0f);
		if (!State.bPrimed)
		{
			State.SmoothedRpm = TargetRpm;
			State.SmoothedThrottle = TargetThrottle;
			State.LastGear = Inputs.Gear;
			State.SmoothedWhineHz = WhineHz(TargetRpm, Inputs.Gear);
			State.bPrimed = true;
		}

		const int32 Cylinders = FMath::Clamp(Spec.Cylinders, 1, 16);
		const float FiringIntervalDeg = 720.0f / static_cast<float>(Cylinders);
		const float RevRange = FMath::Max(Spec.RedlineRpm - Spec.IdleRpm, 1.0f);
		const float Openness = 1.0f - FMath::Clamp(Spec.Muffling, 0.0f, 1.0f);
		const float Pops = FMath::Clamp(Spec.Pops, 0.0f, 1.0f);
		const float Dt = 1.0f / SampleRate;

		// Per-buffer constants. The ones that ride the revs are taken at the
		// buffer's target: a buffer is a few milliseconds, the revs do not move
		// far in one, and an Exp per sample per car is what this avoids.
		const float InputAlpha = AlphaFor(SmoothingSeconds, SampleRate);
		const float AttackAlpha = AlphaFor(PulseAttackSeconds, SampleRate);
		const float PulseSeconds = FMath::Clamp(
			PulseSecondsTimesRpm / FMath::Max(State.SmoothedRpm, DeadEngineRpm), PulseMinSeconds, PulseMaxSeconds);
		const float PulseDecay = 1.0f - AlphaFor(PulseSeconds, SampleRate);
		const float PopDecay = 1.0f - AlphaFor(PopBurstSeconds, SampleRate);
		const float PopBudgetDecay = 1.0f - AlphaFor(PopBudgetSeconds, SampleRate);
		const float ThrottlePeakDecay = 1.0f - AlphaFor(0.3f, SampleRate);
		const float RoughnessAlpha = AlphaForHz(2500.0f, SampleRate);
		const float HissAlpha = AlphaForHz(6000.0f, SampleRate);

		// The pipe: a round trip of 2L at the speed of sound, reflected inverted
		// from the open end (a quarter-wave resonator: modes at c/4L, 3c/4L, ...),
		// losing its top end and some of its level on every trip.
		//
		// The second bank's pipe is a little longer and a little quieter, as
		// they are on a car and as they reach one listener: two identical pipes
		// fired half a period apart cancel every half-order exactly, and the
		// half-orders are what gives an engine its grain.
		const float RoundTripSamples = 2.0f * FMath::Clamp(Spec.ExhaustLengthM, 0.2f, 2.8f) / SpeedOfSoundMps * SampleRate;
		const int32 PipeDelay[2] = {
			FMath::Clamp(static_cast<int32>(RoundTripSamples + 0.5f), 8, PipeCapacity - 1),
			FMath::Clamp(static_cast<int32>(RoundTripSamples * SecondBankLength + 0.5f), 8, PipeCapacity - 1)};
		const float BankLevel[2] = {1.0f, SecondBankLevel};
		const float LoopGain = 0.35f + 0.3f * Openness;
		const float LoopAlpha = AlphaForHz(1200.0f + 7000.0f * Openness, SampleRate);
		// 900 Hz for a road muffler to 12 kHz for open pipes, evenly in octaves.
		const float MufflerAlpha = AlphaForHz(900.0f * FMath::Pow(12000.0f / 900.0f, Openness), SampleRate);
		// The steep front of a blow-down pulse survives an open pipe as rasp; a
		// muffler is there to take it off. A first difference is that edge.
		const float Rasp = RaspAmount * Openness * Openness * SampleRate / 48000.0f;

		// Intake: the airbox's resonance, driven by the same cycle that fires the
		// cylinders (every firing is an intake stroke two strokes earlier), so the
		// roar is a harmonic of the engine and not a drone beside it.
		const float IntakeTheta = 2.0f * PI * 330.0f / SampleRate;
		const float IntakeR = 0.93f;
		const float IntakeA1 = 2.0f * IntakeR * FMath::Cos(IntakeTheta);
		const float IntakeA2 = -IntakeR * IntakeR;
		const float IntakeNoiseAlpha = AlphaForHz(1800.0f, SampleRate);

		const float WhineAlpha = AlphaFor(0.03f, SampleRate);
		const float TurboUpAlpha = AlphaFor(TurboSpoolUpSeconds, SampleRate);
		const float TurboDownAlpha = AlphaFor(TurboSpoolDownSeconds, SampleRate);
		const float BlowOffDecay = 1.0f - AlphaFor(BlowOffSeconds, SampleRate);
		const float BlowOffAlpha = AlphaForHz(2500.0f, SampleRate);

		// A full-throttle upshift cracks: the cut leaves a charge for the pipe.
		if (Inputs.Gear > State.LastGear && State.LastGear >= 1 && State.SmoothedThrottle > 0.5f
			&& NextUnit(State.NoiseState) < Pops)
		{
			FBankState& Bank = State.Banks[NextUnit(State.NoiseState) < 0.5f ? 0 : 1];
			Bank.Pending += 1.2f + 1.3f * Pops;
			Bank.PopBurst += 0.5f + 0.5f * Pops;
		}
		State.LastGear = Inputs.Gear;

		const float TargetWhineHz = WhineHz(TargetRpm, Inputs.Gear);

		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			State.SmoothedRpm += (TargetRpm - State.SmoothedRpm) * InputAlpha;
			State.SmoothedThrottle += (TargetThrottle - State.SmoothedThrottle) * InputAlpha;
			const float Rpm = State.SmoothedRpm;
			const float Throttle = State.SmoothedThrottle;
			const float RevShare = FMath::Clamp((Rpm - Spec.IdleRpm) / RevRange, 0.0f, 1.0f);
			const bool bRunning = Rpm > DeadEngineRpm;
			const bool bOverrun = Throttle < OverrunThrottle && RevShare > OverrunRevShare;

			// A lift: the throttle seen lately falls to nothing while the revs
			// are up. The pipe is hot and the mixture is wrong for a second.
			State.ThrottlePeak = FMath::Max(State.ThrottlePeak * ThrottlePeakDecay, Throttle);
			if (bOverrun && State.ThrottlePeak > 0.4f)
			{
				State.PopBudget = FMath::Max(State.PopBudget, State.ThrottlePeak * (0.4f + 0.6f * RevShare));
				State.ThrottlePeak = 0.0f;
				// Shut on a spinning turbo: the boost has nowhere to go but the valve.
				if (Spec.bTurbo && State.TurboSpeed > 0.25f)
				{
					State.BlowOff = State.TurboSpeed;
				}
			}
			State.PopBudget *= PopBudgetDecay;
			if (!bOverrun)
			{
				// Back on the throttle: burning properly again.
				State.PopBudget *= 0.999f;
			}

			// The limiter: spark cut for half of each cycle while the revs sit on it.
			bool bSparkCut = false;
			if (bRunning && Rpm >= Spec.LimiterRpm - LimiterMarginRpm)
			{
				State.LimiterClock += Dt;
				if (State.LimiterClock >= LimiterCycleSeconds)
				{
					State.LimiterClock -= LimiterCycleSeconds;
				}
				bSparkCut = State.LimiterClock > LimiterCycleSeconds * 0.5f;
			}
			else
			{
				State.LimiterClock = 0.0f;
			}

			// --- The crank, and whoever's turn it is to fire ---------------------
			const float ClosedLoad = FMath::Lerp(IdleLoad, OverrunLoad, RevShare);
			const float Load = ClosedLoad + (1.0f - ClosedLoad) * FMath::Pow(Throttle, 1.2f);
			const float PulseSize = FMath::Pow(Load, LoadLoudnessExponent);
			if (bRunning)
			{
				State.CrankDeg += Rpm * 6.0f * Dt;
				while (State.CrankDeg >= static_cast<float>(State.NextFiring) * FiringIntervalDeg)
				{
					FBankState& Bank = State.Banks[(Spec.BankMask >> State.NextFiring) & 1u];
					if (bSparkCut)
					{
						// An unlit charge goes down the pipe; some of them light there.
						if (NextUnit(State.NoiseState) < 0.35f)
						{
							Bank.Pending += 0.7f;
							Bank.PopBurst += 0.25f;
						}
					}
					else
					{
						// No two combustions are alike, least of all at idle and
						// on a shut throttle: that unevenness is the lumpy idle.
						const float Unevenness = 0.05f + 0.3f * (1.0f - Throttle) * (1.0f - RevShare);
						Bank.Pending += PulseSize * (1.0f + NextNoise(State.NoiseState) * Unevenness);

						if (State.PopBudget > 0.02f
							&& NextUnit(State.NoiseState) < State.PopBudget * Pops * PopChancePerFiring)
						{
							const float Size = NextUnit(State.NoiseState);
							Bank.Pending += Pops * (0.6f + 1.3f * Size);
							Bank.PopBurst += Pops * (0.3f + 0.7f * Size);
						}
					}

					if (++State.NextFiring >= Cylinders)
					{
						State.NextFiring = 0;
						State.CrankDeg -= 720.0f;
					}
				}
			}

			// --- Two banks, two pipes -------------------------------------------
			float Exhaust = 0.0f;
			float Breathing = 0.0f;
			for (int32 BankIndex = 0; BankIndex < 2; ++BankIndex)
			{
				FBankState& Bank = State.Banks[BankIndex];
				const float Taken = Bank.Pending * AttackAlpha;
				Bank.Pending -= Taken;
				Bank.Envelope = (Bank.Envelope + Taken) * PulseDecay;
				Bank.PopBurst *= PopDecay;

				const float Noise = NextNoise(State.NoiseState);
				Bank.Roughness += (Noise - Bank.Roughness) * RoughnessAlpha;
				Bank.Hiss += (Noise - Bank.Hiss) * HissAlpha;
				const float Turbulence = Bank.Roughness * 0.9f + Bank.Hiss * FlowNoise * Openness;
				// The edge is taken from the pulse alone, before the turbulence
				// rides it: differencing the noise as well is only hiss.
				const float Body = Bank.Envelope * (1.0f + PulseSteepening * Load * Bank.Envelope);
				const float Pulse = Body + Rasp * (Body - Bank.LastBody);
				Bank.LastBody = Body;
				// A pop is a bang, not static: its noise is the low-passed kind.
				const float Excitation = Pulse * (1.0f + Load * Turbulence) + Bank.PopBurst * Bank.Roughness * 2.5f;
				Breathing += Bank.Envelope;

				const int32 Read = (Bank.Write - PipeDelay[BankIndex] + PipeCapacity) % PipeCapacity;
				Bank.LoopLowpass += (Bank.Pipe[Read] - Bank.LoopLowpass) * LoopAlpha;
				const float Out = Excitation - LoopGain * Bank.LoopLowpass;
				Bank.Pipe[Bank.Write] = Out;
				Bank.Write = (Bank.Write + 1) % PipeCapacity;
				Exhaust += Out * BankLevel[BankIndex];
			}
			State.MufflerLowpass += (Exhaust * 0.5f - State.MufflerLowpass) * MufflerAlpha;
			float Sample = State.MufflerLowpass * (0.55f + 0.45f * RevShare);

			// --- Intake ----------------------------------------------------------
			const float IntakeHiss = NextNoise(State.NoiseState);
			State.IntakeNoise += (IntakeHiss - State.IntakeNoise) * IntakeNoiseAlpha;
			const float IntakeDrive = bRunning ? Spec.IntakeRoar * Throttle * (0.35f + 0.65f * RevShare) : 0.0f;
			const float Intake = IntakeA1 * State.IntakeY1 + IntakeA2 * State.IntakeY2
				+ (Breathing * 0.5f + State.IntakeNoise * 0.6f) * IntakeDrive * 0.05f;
			State.IntakeY2 = State.IntakeY1;
			State.IntakeY1 = Intake;
			Sample += Intake + IntakeHiss * IntakeDrive * 0.01f;

			// --- Gearbox ---------------------------------------------------------
			State.SmoothedWhineHz += (TargetWhineHz - State.SmoothedWhineHz) * WhineAlpha;
			if (Spec.GearWhine > 0.0f && State.SmoothedWhineHz > 20.0f && bRunning)
			{
				State.WhinePhase += State.SmoothedWhineHz * Dt;
				State.WhinePhase -= static_cast<float>(static_cast<int32>(State.WhinePhase));
				const float Angle = 2.0f * PI * State.WhinePhase;
				// Loudest under drive, still there on the overrun: the teeth are loaded either way.
				const float Drive = 0.35f + 0.65f * FMath::Max(Throttle, bOverrun ? 0.6f : 0.0f);
				Sample += (FMath::Sin(Angle) + 0.35f * FMath::Sin(2.0f * Angle))
					* Spec.GearWhine * 0.045f * Drive * (0.3f + 0.7f * RevShare);
			}

			// --- Turbo -----------------------------------------------------------
			if (Spec.bTurbo)
			{
				const float Wanted = bRunning ? FMath::Sqrt(RevShare) * (0.2f + 0.8f * Throttle) : 0.0f;
				State.TurboSpeed += (Wanted - State.TurboSpeed) * (Wanted > State.TurboSpeed ? TurboUpAlpha : TurboDownAlpha);

				State.BlowOff *= BlowOffDecay;

				State.TurboPhase += (1000.0f + 5200.0f * State.TurboSpeed) * Dt;
				State.TurboPhase -= static_cast<float>(static_cast<int32>(State.TurboPhase));
				const float Hiss = NextNoise(State.NoiseState);
				State.BlowOffHighpass += (Hiss - State.BlowOffHighpass) * BlowOffAlpha;
				Sample += FMath::Sin(2.0f * PI * State.TurboPhase) * 0.03f * State.TurboSpeed * State.TurboSpeed
					+ (Hiss - State.BlowOffHighpass) * (0.22f * State.BlowOff + 0.01f * State.TurboSpeed);
			}

			// --- Out -------------------------------------------------------------
			const float Blocked = Sample - State.DcX1 + 0.995f * State.DcY1;
			State.DcX1 = Sample;
			State.DcY1 = Blocked;
			OutFrames[Index] = SoftClip(Blocked * OutputGain);
		}
	}
}

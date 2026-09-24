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
	 * the base's peak, for a car at the front axle's static grip limit; the
	 * soft limit below keeps what is past it. An ordinary 1 g corner is about
	 * 0.6 of the reference and a downforce car's fast corner 1.5 to 2.4, so a
	 * corner asks the base for a third to most of its force and a fast one
	 * leaves the soft limit room to show the fronts letting go.
	 *
	 * It was raised to 1.1 on reports that the rim "never pushed back", which
	 * turned out to be no force reaching the motor at all (base firmware
	 * behind its driver) and then every force mirrored (DirectInput's sign).
	 * At 1.1 with both fixed, 25% was too heavy to drive with on an 8 Nm base
	 * and a hypercar sat at the base's limit a third of the lap.
	 */
	constexpr float WheelTorqueReference = 0.6f;

	/**
	 * Stiffest the rim may be, as a share of the base's force per rim degree.
	 * The torque reaches the rim through the server's round trip and the
	 * driver's own delay, and past a stiffness that delay makes the rim ring
	 * around the centre by itself — damping does not stop it, only a softer
	 * spring does. Measured on a ClubSport V2.5, let go of 30 degrees off
	 * centre against a 60 Hz server 40 ms late: 0.05 per degree (a hypercar
	 * at 50 m/s at Force 50%) rang at +-16 degrees for good, 0.025 settled
	 * within 5 degrees of overshoot, braking at twice the slope included.
	 */
	constexpr float MaxRimStiffnessPerDeg = 0.025f;

	/** The damper while driving never drops under this share of the force (see MixWheel). */
	constexpr float MinDriveDamper = 0.3f;

	/** Where the torque stops being linear and starts being compressed. */
	constexpr float WheelSoftKnee = 0.8f;

	/** One pole over the 60 Hz message steps; long enough to smooth, short enough not to lag. */
	constexpr float WheelTorqueSmoothingSeconds = 0.012f;

	// --- The rim's own movement ------------------------------------------------

	/**
	 * Furthest the rim's movement since the server's sample is carried by the
	 * slope, in steering input (full lock is 1). A slope is a slope only so
	 * far, and a jump in the input (a pad, a rebind) must not become a jump
	 * in the force.
	 */
	constexpr float MaxCorrectionInput = 0.15f;

	/**
	 * The correction fades in over these speeds. At a crawl the car turns
	 * with the wheels almost at once, so the tyres' slope over one round trip
	 * is a spring that is not there a moment later.
	 */
	constexpr float CorrectionFromMps = 3.0f;
	constexpr float CorrectionFullMps = 12.0f;

	// --- The road ------------------------------------------------------------

	/**
	 * Peak of the road's texture on the rim, as a share of the base, at the
	 * Road effects setting the effects were designed at (0.5), on a statically
	 * loaded front at speed. At full road effects its peaks are a fifth of the
	 * base and twice that on a front loaded by braking: texture on a belt
	 * base, detail on a direct drive. Half this was measured at 0.02 rms at
	 * full road effects, which is below what a belt's own friction lets
	 * through.
	 */
	constexpr float RoadTextureForce = 0.08f;

	/** The texture builds with speed, full by here. */
	constexpr float RoadTextureFullMps = 40.0f;

	/** Load passes the road up the column less than in proportion. */
	constexpr float RoadLoadExponent = 0.7f;

	/**
	 * The road's surface, per side, as wavelengths along the lap and their
	 * weights: seams and grain, patches and ripples, and the long undulation
	 * that tugs the rim over a whole corner. The shortest fall away at speed
	 * rather than alias against the frame rate.
	 */
	struct FRoadOctave
	{
		float WavelengthM;
		float Weight;
	};
	constexpr FRoadOctave RoadOctaves[] = { { 0.45f, 0.35f }, { 1.6f, 0.5f }, { 6.0f, 0.4f } };

	/** Grass and gravel are this much rougher than asphalt. */
	constexpr float OffTrackRoughness = 3.0f;

	/** A station this far from where the texture is being read is a new lap or a reset: jump to it. */
	constexpr float RoadStationSnapM = 20.0f;
	/** Otherwise each telemetry sample pulls it this much of the way. */
	constexpr float RoadStationPull = 0.25f;

	/**
	 * The fronts working under braking, felt as a fine grain before they lock:
	 * from half their peak slip ratio to it. Under ABS it sits at full, beneath
	 * the pulse train.
	 */
	constexpr float BrakeGrainAmplitude = 0.22f;
	constexpr float BrakeGrainHz = 62.0f;

	/** Hits die away a little more slowly on a rim than on a motor. */
	constexpr float WheelBumpDecaySeconds = 0.09f;
	constexpr float WheelImpactDecaySeconds = 0.25f;

	/**
	 * A hit's yank on the rim: sharp, and gone before the driver has had to
	 * fight it for long. Long enough that a 60 Hz message is felt as one push
	 * rather than a click.
	 */
	constexpr float WheelSteerKickDecaySeconds = 0.07f;

	/**
	 * Fronts sliding scrub across the road, which comes up the column as a fine
	 * grain under the rim going light. Quiet on purpose: the torque falling
	 * away is the message, this only says it is the tyres doing it.
	 */
	constexpr float WheelScrubAmplitude = 0.14f;
	constexpr float WheelScrubHz = 55.0f;

	// --- Centring and the steering lock's stop ---------------------------------

	/**
	 * Only a car standing still is centred when it arrives: a pause menu
	 * closed mid-lap must not pull the rim out of the driver's hands.
	 */
	constexpr float CentreStartMaxSpeedMps = 2.0f;
	/** Rolling away ends it; the car's own torque takes over. */
	constexpr float CentreEndSpeedMps = 3.0f;
	constexpr float CentreMaxSeconds = 3.0f;
	/**
	 * The push toward the middle: full strength this far off centre, never
	 * more than the cap. A position loop rather than DirectInput's spring,
	 * whose force is a share of the base's whole travel: at 30% it put 6% of
	 * the base behind a rim a quarter turn off centre, which a direct drive's
	 * own friction holds still.
	 */
	constexpr float CentreFullDeg = 30.0f;
	constexpr float CentreMaxForce = 0.35f;
	/** Braking by the rim's speed, per degree a second, so it arrives without swinging past. */
	constexpr float CentreRateDamping = 1.0f / 360.0f;
	/** Centred: within this and this slow, for this long. */
	constexpr float CentreDoneDeg = 2.0f;
	constexpr float CentreDoneRateDegPerS = 30.0f;
	constexpr float CentreSettleSeconds = 0.25f;
	constexpr float CentreEaseSeconds = 0.08f;

	/** The rim's speed is a difference of readings; this takes the USB jitter out of it. */
	constexpr float RimRateSmoothingSeconds = 0.03f;

	/**
	 * Past the steering lock the rim meets a stop that builds to this over
	 * these degrees, with a damper so it does not bounce off it: a wall, but
	 * not one that snaps a wrist.
	 */
	constexpr float SoftLockForce = 0.9f;
	constexpr float SoftLockRampDeg = 6.0f;
	constexpr float SoftLockDamper = 0.5f;

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

	/** A lattice point's height, -1..1, the same for the same point and seed on every run. */
	float LatticeHeight(int64 Point, uint32 Seed)
	{
		uint32 Hash = static_cast<uint32>(Point) * 0x9E3779B1u ^ static_cast<uint32>(Point >> 32) * 0x85EBCA77u ^ Seed;
		Hash ^= Hash >> 16;
		Hash *= 0x7FEB352Du;
		Hash ^= Hash >> 15;
		Hash *= 0x846CA68Bu;
		Hash ^= Hash >> 16;
		return static_cast<float>(Hash & 0xFFFFFF) / static_cast<float>(0x800000) - 1.0f;
	}

	/** Smooth 1-D value noise, -1..1, one lattice point per unit of `X`. */
	float ValueNoise(double X, uint32 Seed)
	{
		const double Cell = FMath::FloorToDouble(X);
		const float Frac = static_cast<float>(X - Cell);
		const int64 Point = static_cast<int64>(Cell);
		const float Ease = Frac * Frac * (3.0f - 2.0f * Frac);
		return FMath::Lerp(LatticeHeight(Point, Seed), LatticeHeight(Point + 1, Seed), Ease);
	}

	/**
	 * The road under the two front tyres at `StationM`, as the torque it puts
	 * on the rim: -1..1 or so, positive to the right. A bump under one wheel
	 * and not the other is what tugs a rim, so it is the two sides'
	 * difference. Octaves that would pass faster than half the frame rate are
	 * left out rather than aliased.
	 */
	float RoadTug(double StationM, float SpeedMps, float DeltaSeconds)
	{
		const float Nyquist = 0.5f / FMath::Max(DeltaSeconds, 1e-4f);
		float Tug = 0.0f;
		uint32 Seed = 0x51ED270Bu;
		for (const FRoadOctave& Octave : RoadOctaves)
		{
			const float Hz = SpeedMps / Octave.WavelengthM;
			const float Audible = 1.0f - Ramp(Hz, 0.5f * Nyquist, 0.9f * Nyquist);
			if (Audible > 0.0f)
			{
				const double X = StationM / Octave.WavelengthM;
				Tug += Octave.Weight * Audible * 0.5f * (ValueNoise(X, Seed) - ValueNoise(X, Seed ^ 0xA5A5A5A5u));
			}
			Seed = Seed * 747796405u + 2891336453u;
		}
		return Tug;
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
		Signals.ServerSteer = -Feedback.SteerInput;
		Signals.SteerStiffness = Feedback.SteerStiffness;
		Signals.FrontLoad = FMath::Max(0.0f, Feedback.FrontLoad);
		Signals.FrontBrakeSlip = FMath::Max(
			Ramp(-Wheels[W::FrontLeft].SlipRatio, 0.5f, 1.0f), Ramp(-Wheels[W::FrontRight].SlipRatio, 0.5f, 1.0f));

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
			Signals.SteerKick = -Feedback.SteerKick;
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

	void RequestCentre(FWheelState& State)
	{
		State.CentreSeconds = CentreMaxSeconds;
		State.CentredFor = 0.0f;
	}

	FApexWheelEffects MixWheel(
		const FSignals& Signals, FWheelState& State, float DeltaSeconds, const FWheelTuning& Tuning)
	{
		const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);

		// The rim's own speed, for damping the centring.
		if (Signals.bHasRim)
		{
			if (State.bHaveRim && Dt > 0.0f)
			{
				const float Raw = (Signals.RimDegrees - State.LastRim) / Dt;
				State.RimRate = FMath::Lerp(State.RimRate, Raw, 1.0f - FMath::Exp(-Dt / RimRateSmoothingSeconds));
			}
			State.LastRim = Signals.RimDegrees;
			State.bHaveRim = true;
		}
		else
		{
			State.bHaveRim = false;
			State.RimRate = 0.0f;
		}

		// A car arriving standing still: the start of a session, the garage.
		const bool bArriving = Signals.bActive && !State.bWasActive;
		if (bArriving && Signals.SpeedMps < CentreStartMaxSpeedMps)
		{
			RequestCentre(State);
		}
		State.bWasActive = Signals.bActive;

		if (State.CentreSeconds > 0.0f)
		{
			State.CentreSeconds -= Dt;
			if (!Signals.bHasRim || Signals.SpeedMps > CentreEndSpeedMps)
			{
				State.CentreSeconds = 0.0f;
			}
			else if (FMath::Abs(Signals.RimDegrees) < CentreDoneDeg && FMath::Abs(State.RimRate) < CentreDoneRateDegPerS)
			{
				State.CentredFor += Dt;
				if (State.CentredFor >= CentreSettleSeconds)
				{
					State.CentreSeconds = 0.0f;
				}
			}
			else
			{
				State.CentredFor = 0.0f;
			}
		}
		State.CentreGain = FMath::Lerp(State.CentreGain, State.CentreSeconds > 0.0f ? 1.0f : 0.0f,
			1.0f - FMath::Exp(-Dt / CentreEaseSeconds));

		State.Bump *= FMath::Exp(-Dt / WheelBumpDecaySeconds);
		State.Impact *= FMath::Exp(-Dt / WheelImpactDecaySeconds);
		State.ShiftKick *= FMath::Exp(-Dt / ShiftDecaySeconds);
		State.SteerKick *= FMath::Exp(-Dt / WheelSteerKickDecaySeconds);
		if (Signals.bActive && FMath::Abs(Signals.SteerKick) > FMath::Abs(State.SteerKick))
		{
			State.SteerKick = Signals.SteerKick;
		}

		// The torque, smoothed towards where the car says it should be. With no
		// car it runs down to nothing rather than being dropped, which would be
		// a wheel let go of mid-corner.
		const float Target = Signals.bActive ? Signals.SteerTorque : 0.0f;
		const float Smoothing = 1.0f - FMath::Exp(-Dt / WheelTorqueSmoothingSeconds);
		State.Torque = FMath::Lerp(State.Torque, Target, Smoothing);
		if (bArriving)
		{
			State.ServerSteer = Signals.ServerSteer;
			State.Stiffness = Signals.SteerStiffness;
		}
		else if (Signals.bActive)
		{
			State.ServerSteer = FMath::Lerp(State.ServerSteer, Signals.ServerSteer, Smoothing);
			State.Stiffness = FMath::Lerp(State.Stiffness, Signals.SteerStiffness, Smoothing);
		}
		else
		{
			State.Stiffness = FMath::Lerp(State.Stiffness, 0.0f, Smoothing);
		}

		// The torque at the rim's position now: the server's sample, plus its
		// slope times how far the input has moved since the sample was taken.
		// Only a slope that pushes back is carried: past the aligning crest the
		// slope pulls the rim on the way it is going, which the car answers by
		// sliding further, and without the car in the loop that is a force
		// that feeds itself. The server's own samples still show the rim going
		// light, a round trip later.
		State.Correction = 0.0f;
		if (Signals.bActive && Signals.bHasLocalSteer)
		{
			const float Moved = FMath::Clamp(Signals.LocalSteer - State.ServerSteer, -MaxCorrectionInput, MaxCorrectionInput);
			State.Correction = FMath::Min(State.Stiffness, 0.0f) * Moved
				* Ramp(Signals.SpeedMps, CorrectionFromMps, CorrectionFullMps);
		}

		// The road under the fronts, read where the car is on the lap; with no
		// place on the lap there is no road to read.
		State.Road = 0.0f;
		if (Signals.bActive && Signals.bHasStation)
		{
			if (!State.bHaveStation || FMath::Abs(Signals.StationM - State.RoadStation) > RoadStationSnapM)
			{
				State.RoadStation = Signals.StationM;
			}
			else if (Signals.StationM != State.LastStationSample)
			{
				State.RoadStation += RoadStationPull * (Signals.StationM - State.RoadStation);
			}
			State.LastStationSample = Signals.StationM;
			State.bHaveStation = true;
			State.RoadStation += Signals.SpeedMps * Dt;

			const float Load = FMath::Pow(FMath::Clamp(Signals.FrontLoad, 0.0f, 3.0f), RoadLoadExponent);
			const float Rough = 1.0f + (OffTrackRoughness - 1.0f) * Signals.OffTrack;
			State.Road = RoadTextureForce * 2.0f * FMath::Clamp(Tuning.RoadEffects, 0.0f, 1.0f)
				* Ramp(Signals.SpeedMps, 0.0f, RoadTextureFullMps) * Load * Rough
				* RoadTug(State.RoadStation, Signals.SpeedMps, Dt);
		}
		else
		{
			State.bHaveStation = false;
		}

		FApexWheelEffects Out;
		// The centring and the stop are not the car's: they follow the Force
		// slider only as far as switching off with it.
		const float Strength = FMath::Clamp(2.0f * Tuning.Force, 0.0f, 1.0f);

		float Centring = 0.0f;
		if (Signals.bHasRim && State.CentreGain > 1e-3f)
		{
			Centring = State.CentreGain * Strength * CentreMaxForce
				* FMath::Clamp(-Signals.RimDegrees / CentreFullDeg - State.RimRate * CentreRateDamping, -1.0f, 1.0f);
		}

		float Stop = 0.0f;
		bool bPastLock = false;
		if (Signals.bActive && Signals.bHasRim && Tuning.SteeringLockDeg > 0.0f)
		{
			const float Over = FMath::Abs(Signals.RimDegrees) - 0.5f * Tuning.SteeringLockDeg;
			if (Over > 0.0f)
			{
				bPastLock = true;
				Stop = -FMath::Sign(Signals.RimDegrees) * Strength * SoftLockForce * FMath::Clamp(Over / SoftLockRampDeg, 0.0f, 1.0f);
			}
		}

		// A hit is not smoothed: its edge is what makes it a hit.
		// The stiffness limit: where the tyres' slope would make the rim
		// stiffer than the loop can hold, the whole torque comes down with it.
		// Only a steep slope is touched (a fast straight, heavy braking); near
		// the grip limit the slope is small and a corner keeps its weight.
		const float Gain = 2.0f * FMath::Clamp(Tuning.Force, 0.0f, 1.0f) * WheelTorqueReference;
		float Limit = 1.0f;
		if (Tuning.RimDegreesPerInput > 0.0f)
		{
			const float PerDegree = Gain * FMath::Max(-State.Stiffness, 0.0f) / Tuning.RimDegreesPerInput;
			if (PerDegree > MaxRimStiffnessPerDeg)
			{
				Limit = MaxRimStiffnessPerDeg / PerDegree;
			}
		}
		State.StiffnessLimit = Limit;
		const float Torque = SoftLimit((State.Torque + State.Correction + State.SteerKick) * Gain * Limit);
		// The road rides on top of the soft limit, so a corner that has the
		// torque near the base's peak still has a road under it.
		const float Force = FMath::Clamp(Torque + State.Road + Centring + Stop, -1.0f, 1.0f);
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
			// The fronts working toward their limit under braking: the grain a
			// driver brakes by, before anything locks.
			Louder(BrakeGrainAmplitude * Signals.FrontBrakeSlip * Road, BrakeGrainHz);
			// A locked or spinning tyre judders faster than the road does.
			Louder(0.3f * Signals.Lockup, 45.0f);
			Louder(0.2f * Signals.Wheelspin, 32.0f);
			Louder(WheelScrubAmplitude * Signals.FrontSlide * Ramp(Speed, 0.0f, SlideFullSpeedMps), WheelScrubHz);
		}

		Louder(State.Bump, 14.0f);
		Louder(State.Impact, 9.0f);
		Louder(0.8f * State.ShiftKick, 26.0f);

		Out.VibrationAmplitude = FMath::Clamp(Texture.Amplitude * 2.0f * FMath::Clamp(Tuning.RoadEffects, 0.0f, 1.0f), 0.0f, 1.0f);
		Out.VibrationHz = Texture.Hz;

		// Heavy at a standstill, where a real car's steering is heavy and where
		// a wheel with nothing to push against would otherwise spin freely. At
		// speed a third of it, which keeps the rim from swinging on the
		// torque's network delay without reading as a heavy wheel.
		const float Parked = 1.0f - Ramp(Signals.SpeedMps, 0.0f, WheelDamperFullSpeedMps);
		Out.Damper = FMath::Clamp(Tuning.Damping, 0.0f, 1.0f) * (0.35f + 0.65f * Parked);
		// While driving, never less than a share of the force: the rim of a
		// belt base has almost no friction of its own, and this is what stops
		// it overshooting the centre (measured: 8.6 degrees at 0.05, 4.5 at
		// 0.3, let go at 50 m/s).
		if (Signals.bActive)
		{
			Out.Damper = FMath::Max(Out.Damper, MinDriveDamper * Strength);
		}
		if (bPastLock)
		{
			Out.Damper = FMath::Max(Out.Damper, Strength * SoftLockDamper);
		}

		// Only in the menus, and only if forces are on at all: a rim that
		// flops to one side while the player picks a car feels broken.
		Out.Spring = Signals.bActive ? 0.0f : WheelMenuSpring * FMath::Clamp(2.0f * Tuning.Force, 0.0f, 1.0f);
		return Out;
	}
}

#include "ApexTestCommon.h"
#include "Audio/ApexEngineSound.h"
#include "Audio/ApexEngineSoundWave.h"
#include "Catalog/ApexCatalogRows.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The widest rev range in content/cars: the F1 idles at 4500 and revs to 15,500. */
	constexpr float EngineTestF1IdleRpm = 4500.0f;
	constexpr float EngineTestF1MaxRpm = 15000.0f;

	constexpr float EngineTestSampleRate = 48000.0f;

	/** Seconds of a steady operating point, in buffers the size a device asks for. */
	TArray<float> RenderSteady(const ApexEngineSynth::FEngineSpec& Spec, float Rpm, float Throttle, int32 Gear,
		float Seconds, float SampleRate = EngineTestSampleRate)
	{
		// On the heap: the state carries both exhaust pipes.
		const TUniquePtr<ApexEngineSynth::FState> State = MakeUnique<ApexEngineSynth::FState>();
		ApexEngineSynth::FInputs Inputs;
		Inputs.Rpm = Rpm;
		Inputs.Throttle = Throttle;
		Inputs.Gear = Gear;

		constexpr int32 Block = 480;
		TArray<float> Frames;
		Frames.SetNumZeroed(static_cast<int32>(SampleRate * Seconds) / Block * Block);
		for (int32 At = 0; At < Frames.Num(); At += Block)
		{
			ApexEngineSynth::Render(*State, Spec, Inputs, SampleRate, Frames.GetData() + At, Block);
		}
		return Frames;
	}

	/** Power at one frequency (Goertzel), skipping the first quarter second while the pipes fill. */
	double PowerAt(const TArray<float>& Frames, double Hz, double SampleRate)
	{
		const int32 From = static_cast<int32>(SampleRate / 4.0);
		double Re = 0.0;
		double Im = 0.0;
		for (int32 Index = From; Index < Frames.Num(); ++Index)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * Hz * Index / SampleRate;
			Re += Frames[Index] * FMath::Cos(Angle);
			Im += Frames[Index] * FMath::Sin(Angle);
		}
		const double Count = FMath::Max(Frames.Num() - From, 1);
		return (Re * Re + Im * Im) / (Count * Count);
	}

	struct FLevel
	{
		float Rms = 0.0f;
		float Peak = 0.0f;
		bool bFinite = true;
	};

	FLevel Measure(const TArray<float>& Frames, int32 From = 0)
	{
		FLevel Level;
		double Sum = 0.0;
		for (int32 Index = From; Index < Frames.Num(); ++Index)
		{
			Level.bFinite = Level.bFinite && FMath::IsFinite(Frames[Index]);
			Level.Peak = FMath::Max(Level.Peak, FMath::Abs(Frames[Index]));
			Sum += Frames[Index] * Frames[Index];
		}
		Level.Rms = static_cast<float>(FMath::Sqrt(Sum / FMath::Max(Frames.Num() - From, 1)));
		return Level;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineNoteTest,
	"ApexSim.Audio.EngineNote",
	ApexTestFlags)

bool FApexEngineNoteTest::RunTest(const FString& Parameters)
{
	ApexEngineSynth::FEngineSpec V8;
	ApexEngineSynth::FEngineSpec V6 = V8;
	V6.Cylinders = 6;

	// The note is the firing rate: four firings a rev for a V8, three for a V6.
	TestEqual(TEXT("V8 at 6000rpm"), ApexEngineSynth::NoteHz(V8, 6000.0f), 400.0f);
	TestEqual(TEXT("V6 at 15,000rpm"), ApexEngineSynth::NoteHz(V6, 15000.0f), 750.0f);

	// It keeps climbing for as long as the revs do. It used to be a lerp across
	// a guessed rev range, which pinned the pitch at 8000rpm while an F1 carried
	// on to 15,000 — the whole top half of every gear at one note.
	float Previous = 0.0f;
	for (float Rpm = 1000.0f; Rpm <= EngineTestF1MaxRpm; Rpm += 250.0f)
	{
		const float Hz = ApexEngineSynth::NoteHz(V6, Rpm);
		TestTrue(*FString::Printf(TEXT("%.0frpm (%.1fHz) is higher than %.0frpm (%.1fHz)"),
					 Rpm, Hz, Rpm - 250.0f, Previous),
			Hz > Previous);
		Previous = Hz;
	}

	// And what is rendered is that note: the power sits on the firing
	// frequency, not beside it, at either device rate.
	for (const float SampleRate : {48000.0f, 44100.0f})
	{
		const TArray<float> Frames = RenderSteady(V8, 4200.0f, 1.0f, 3, 1.5f, SampleRate);
		const double Note = ApexEngineSynth::NoteHz(V8, 4200.0f);
		const double On = PowerAt(Frames, Note, SampleRate);
		const double Off = PowerAt(Frames, Note * 1.0925, SampleRate);
		TestTrue(*FString::Printf(TEXT("the firing note carries the sound at %.0fHz (%.2e on, %.2e off)"),
					 SampleRate, On, Off),
			On > Off * 200.0);
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineCrankTest,
	"ApexSim.Audio.EngineCrank",
	ApexTestFlags)

bool FApexEngineCrankTest::RunTest(const FString& Parameters)
{
	// Same eight cylinders, same revs, same pipes: the crank alone decides how
	// much there is under the firing note. Every four-stroke has some — no two
	// cylinders are alike, and the pattern repeats every two revs — but a
	// crossplane's banks also fire unevenly, which puts far more power on the
	// crank's own frequency and its odd halves: the burble.
	ApexEngineSynth::FEngineSpec Cross;
	Cross.GearWhine = 0.0f;
	Cross.IntakeRoar = 0.0f;
	Cross.BankMask = ApexEngineSynth::BankMask(8, true);
	ApexEngineSynth::FEngineSpec Flat = Cross;
	Flat.BankMask = ApexEngineSynth::BankMask(8, false);

	constexpr float Rpm = 4200.0f;
	const double CrankHz = Rpm / 60.0;
	const TArray<float> CrossFrames = RenderSteady(Cross, Rpm, 1.0f, 3, 2.0f);
	const TArray<float> FlatFrames = RenderSteady(Flat, Rpm, 1.0f, 3, 2.0f);

	const double CrossBurble = PowerAt(CrossFrames, CrankHz, EngineTestSampleRate)
		+ PowerAt(CrossFrames, CrankHz * 1.5, EngineTestSampleRate) + PowerAt(CrossFrames, CrankHz * 3.0, EngineTestSampleRate);
	const double FlatBurble = PowerAt(FlatFrames, CrankHz, EngineTestSampleRate)
		+ PowerAt(FlatFrames, CrankHz * 1.5, EngineTestSampleRate) + PowerAt(FlatFrames, CrankHz * 3.0, EngineTestSampleRate);
	TestTrue(*FString::Printf(TEXT("a crossplane burbles as a flatplane does not (%.2e vs %.2e)"), CrossBurble, FlatBurble),
		CrossBurble > FlatBurble * 5.0);

	// The flatplane is still an engine and not a tone: its cylinders differ,
	// so the half-orders are there, well clear of the floor between orders.
	// (With every firing alike — or differing at random, which is a misfire —
	// the model sounded like a two-stroke.)
	const double FlatOrders = PowerAt(FlatFrames, CrankHz * 0.5, EngineTestSampleRate)
		+ PowerAt(FlatFrames, CrankHz, EngineTestSampleRate) + PowerAt(FlatFrames, CrankHz * 1.5, EngineTestSampleRate);
	const double FlatFloor = PowerAt(FlatFrames, CrankHz * 0.73, EngineTestSampleRate)
		+ PowerAt(FlatFrames, CrankHz * 1.27, EngineTestSampleRate) + PowerAt(FlatFrames, CrankHz * 1.73, EngineTestSampleRate);
	TestTrue(*FString::Printf(TEXT("every four-stroke has its half-orders (%.2e over a floor of %.2e)"), FlatOrders, FlatFloor),
		FlatOrders > FlatFloor * 10.0);

	// Audible, not dominant: within 20 dB of the firing note, and under it.
	const double CrossNote = PowerAt(CrossFrames, CrankHz * 4.0, EngineTestSampleRate);
	TestTrue(*FString::Printf(TEXT("the burble is heard under the note (%.2e of %.2e)"), CrossBurble, CrossNote),
		CrossBurble > CrossNote * 0.01 && CrossBurble < CrossNote);

	TestEqual(TEXT("crossplane V8 pattern is L R L L R R L R"), ApexEngineSynth::BankMask(8, true) & 0xFFu, 0xB2u);
	TestEqual(TEXT("everything else alternates"), ApexEngineSynth::BankMask(6, false) & 0x3Fu, 0x2Au);
	TestEqual(TEXT("a crossplane six is not a thing"), ApexEngineSynth::BankMask(6, true), ApexEngineSynth::BankMask(6, false));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineTimbreTest,
	"ApexSim.Audio.EngineTimbre",
	ApexTestFlags)

bool FApexEngineTimbreTest::RunTest(const FString& Parameters)
{
	// What the player asked for, as numbers: the GT3 V8 is a low roar and the
	// F1 a scream. Measured as the share of the power above 2 kHz at three
	// quarters of each car's own rev range, flat out.
	ApexEngineSynth::FEngineSpec Gt3;
	Gt3.ExhaustLengthM = 1.9f;
	Gt3.Muffling = 0.45f;
	Gt3.IdleRpm = 850.0f;
	Gt3.RedlineRpm = 7500.0f;
	Gt3.LimiterRpm = 7650.0f;

	ApexEngineSynth::FEngineSpec F1;
	F1.Cylinders = 6;
	F1.BankMask = ApexEngineSynth::BankMask(6, false);
	F1.bTurbo = true;
	F1.ExhaustLengthM = 0.6f;
	F1.Muffling = 0.05f;
	F1.IdleRpm = EngineTestF1IdleRpm;
	F1.RedlineRpm = EngineTestF1MaxRpm;
	F1.LimiterRpm = 15300.0f;

	auto HighShare = [](const TArray<float>& Frames)
	{
		// A one-pole split at 2 kHz is all the precision the claim needs.
		const float Alpha = 1.0f - FMath::Exp(-2.0f * PI * 2000.0f / EngineTestSampleRate);
		float Low = 0.0f;
		double LowPower = 0.0;
		double HighPower = 0.0;
		for (const float Sample : Frames)
		{
			Low += (Sample - Low) * Alpha;
			LowPower += Low * Low;
			HighPower += (Sample - Low) * (Sample - Low);
		}
		return static_cast<float>(HighPower / FMath::Max(LowPower + HighPower, 1.0e-12));
	};

	const float Gt3Share = HighShare(RenderSteady(Gt3, 5800.0f, 1.0f, 4, 1.0f));
	const float F1Share = HighShare(RenderSteady(F1, 12400.0f, 1.0f, 6, 1.0f));
	TestTrue(*FString::Printf(TEXT("the V8 keeps its power low (%.0f%% above 2 kHz)"), Gt3Share * 100.0f), Gt3Share < 0.08f);
	TestTrue(*FString::Printf(TEXT("the F1 screams (%.0f%% above 2 kHz)"), F1Share * 100.0f), F1Share > 0.18f);
	TestTrue(TEXT("and they are nothing alike"), F1Share > Gt3Share * 2.5f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineGearWhineTest,
	"ApexSim.Audio.EngineGearWhine",
	ApexTestFlags)

bool FApexEngineGearWhineTest::RunTest(const FString& Parameters)
{
	// Each pair of gears has its own tooth count, so the whine steps up on an
	// upshift at unchanged revs: the shift is heard before the revs fall.
	float Previous = ApexEngineSynth::WhineHz(8000.0f, 1);
	TestTrue(TEXT("first gear whines"), Previous > 1000.0f);
	for (int32 Gear = 2; Gear <= 8; ++Gear)
	{
		const float Hz = ApexEngineSynth::WhineHz(8000.0f, Gear);
		TestTrue(*FString::Printf(TEXT("gear %d whines higher than gear %d (%.0fHz > %.0fHz)"), Gear, Gear - 1, Hz, Previous),
			Hz > Previous);
		Previous = Hz;
	}
	TestEqual(TEXT("neutral does not whine"), ApexEngineSynth::WhineHz(8000.0f, 0), 0.0f);
	TestEqual(TEXT("nor reverse"), ApexEngineSynth::WhineHz(8000.0f, -1), 0.0f);
	TestEqual(TEXT("an exotic gearbox tops out"), ApexEngineSynth::WhineHz(8000.0f, 20), ApexEngineSynth::WhineHz(8000.0f, 8));
	TestTrue(TEXT("stays inside the band at an F1's revs"), ApexEngineSynth::WhineHz(EngineTestF1MaxRpm, 8) < 10000.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEnginePopsTest,
	"ApexSim.Audio.EnginePops",
	ApexTestFlags)

bool FApexEnginePopsTest::RunTest(const FString& Parameters)
{
	// Flat out at 6500, then off the throttle: the overrun. With `pops` the
	// pipe cracks — peaks standing far out of a quiet floor; without, it only
	// hums. The noise is seeded, so this is the same lift every run.
	auto Overrun = [](float Pops)
	{
		ApexEngineSynth::FEngineSpec Spec;
		Spec.Pops = Pops;
		const TUniquePtr<ApexEngineSynth::FState> State = MakeUnique<ApexEngineSynth::FState>();
		ApexEngineSynth::FInputs Inputs;
		Inputs.Rpm = 6500.0f;
		Inputs.Gear = 3;

		constexpr int32 Block = 480;
		constexpr int32 LiftAt = 24000;
		TArray<float> Frames;
		Frames.SetNumZeroed(96000);
		for (int32 At = 0; At < Frames.Num(); At += Block)
		{
			Inputs.Throttle = At < LiftAt ? 1.0f : 0.0f;
			ApexEngineSynth::Render(*State, Spec, Inputs, EngineTestSampleRate, Frames.GetData() + At, Block);
		}
		return Measure(Frames, LiftAt + 6000);
	};

	const FLevel Quiet = Overrun(0.0f);
	const FLevel Popping = Overrun(1.0f);
	TestTrue(*FString::Printf(TEXT("no pops: the overrun hums (crest %.1f)"), Quiet.Peak / Quiet.Rms), Quiet.Peak < Quiet.Rms * 6.0f);
	TestTrue(*FString::Printf(TEXT("pops stand out of the overrun (peak %.2f against %.2f)"), Popping.Peak, Quiet.Peak),
		Popping.Peak > Quiet.Peak * 4.0f);
	TestTrue(TEXT("a pop saturates, it does not clip"), Popping.Peak <= 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineRenderTest,
	"ApexSim.Audio.EngineRender",
	ApexTestFlags)

bool FApexEngineRenderTest::RunTest(const FString& Parameters)
{
	ApexEngineSynth::FEngineSpec F1;
	F1.Cylinders = 6;
	F1.BankMask = ApexEngineSynth::BankMask(6, false);
	F1.bTurbo = true;
	F1.ExhaustLengthM = 0.6f;
	F1.Muffling = 0.05f;
	F1.IdleRpm = EngineTestF1IdleRpm;
	F1.RedlineRpm = EngineTestF1MaxRpm;
	F1.LimiterRpm = 15300.0f;

	// Whatever the operating point, the mixer gets a finite, unclipped,
	// audible signal — the limiter and beyond included.
	const float Rpms[] = {EngineTestF1IdleRpm, 9000.0f, EngineTestF1MaxRpm, 15400.0f, 40000.0f};
	const float Throttles[] = {0.0f, 0.5f, 1.0f};
	for (const float Rpm : Rpms)
	{
		for (const float Throttle : Throttles)
		{
			const FLevel Level = Measure(RenderSteady(F1, Rpm, Throttle, 3, 0.5f), 4800);
			TestTrue(*FString::Printf(TEXT("%.0frpm at %.0f%% throttle is finite"), Rpm, Throttle * 100.0f), Level.bFinite);
			TestTrue(*FString::Printf(TEXT("%.0frpm at %.0f%% throttle does not clip (peak %.2f)"),
						 Rpm, Throttle * 100.0f, Level.Peak),
				Level.Peak <= 1.0f);
			TestTrue(*FString::Printf(TEXT("%.0frpm at %.0f%% throttle is audible (rms %.3f)"),
						 Rpm, Throttle * 100.0f, Level.Rms),
				Level.Rms > 0.003f);
		}
	}

	// Load is heard: flat out is well above a trailing throttle at the same revs.
	const FLevel Open = Measure(RenderSteady(F1, 11000.0f, 1.0f, 5, 0.5f), 4800);
	const FLevel Shut = Measure(RenderSteady(F1, 11000.0f, 0.0f, 5, 0.5f), 4800);
	TestTrue(*FString::Printf(TEXT("throttle opens the engine up (%.3f against %.3f)"), Open.Rms, Shut.Rms),
		Open.Rms > Shut.Rms * 3.0f);

	// A stopped crank is a stopped engine.
	const FLevel Dead = Measure(RenderSteady(F1, 0.0f, 0.0f, 0, 0.25f));
	TestTrue(*FString::Printf(TEXT("a dead engine is silent (peak %.4f)"), Dead.Peak), Dead.Peak < 0.001f);

	// On the limiter the spark is cut half the time: the level stutters in a
	// way it does not a few hundred revs below.
	auto Stutter = [&F1](float Rpm)
	{
		const TArray<float> Frames = RenderSteady(F1, Rpm, 1.0f, 8, 1.0f);
		float Lowest = TNumericLimits<float>::Max();
		float Highest = 0.0f;
		for (int32 Window = 24000; Window + 480 <= Frames.Num(); Window += 480)
		{
			double Sum = 0.0;
			for (int32 Index = Window; Index < Window + 480; ++Index)
			{
				Sum += Frames[Index] * Frames[Index];
			}
			const float Rms = static_cast<float>(FMath::Sqrt(Sum / 480.0));
			Lowest = FMath::Min(Lowest, Rms);
			Highest = FMath::Max(Highest, Rms);
		}
		return Highest / FMath::Max(Lowest, 1.0e-6f);
	};
	const float Below = Stutter(14000.0f);
	const float OnLimiter = Stutter(15300.0f);
	TestTrue(*FString::Printf(TEXT("the limiter stutters (%.1f against %.1f)"), OnLimiter, Below), OnLimiter > Below * 1.2f);

	// The device picks the rate, so the level must not depend on it.
	const FLevel At48k = Measure(RenderSteady(F1, 12000.0f, 1.0f, 2, 0.5f, 48000.0f), 4800);
	const FLevel At44k = Measure(RenderSteady(F1, 12000.0f, 1.0f, 2, 0.5f, 44100.0f), 4410);
	TestTrue(*FString::Printf(TEXT("same level at 44.1k and 48k (%.3f vs %.3f)"), At44k.Rms, At48k.Rms),
		FMath::Abs(At44k.Rms - At48k.Rms) < At48k.Rms * 0.2f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEngineSpecTest,
	"ApexSim.Audio.EngineSpec",
	ApexTestFlags)

bool FApexEngineSpecTest::RunTest(const FString& Parameters)
{
	// A row with the car.toml's [sound] table: taken as given.
	FApexEngineSoundSpec Row;
	Row.Cylinders = 8;
	Row.bCrossplane = true;
	Row.IdleRpm = 850.0f;
	Row.RedlineRpm = 7500.0f;
	Row.LimiterRpm = 7650.0f;
	Row.ExhaustLengthM = 1.9f;
	Row.Muffling = 0.45f;
	Row.Pops = 0.9f;
	ApexEngineSynth::FEngineSpec Spec = ApexEngineAudio::MakeSpec(Row, TEXT("GT3"));
	TestEqual(TEXT("cylinders"), Spec.Cylinders, 8);
	TestEqual(TEXT("crossplane banks"), Spec.BankMask & 0xFFu, 0xB2u);
	TestEqual(TEXT("redline"), Spec.RedlineRpm, 7500.0f);
	TestEqual(TEXT("limiter"), Spec.LimiterRpm, 7650.0f);
	TestEqual(TEXT("pops"), Spec.Pops, 0.9f);

	// A row from before the field: the class supplies the engine.
	const ApexEngineSynth::FEngineSpec Formula = ApexEngineAudio::MakeSpec(FApexEngineSoundSpec(), TEXT("F1"));
	TestEqual(TEXT("an F1 is a six"), Formula.Cylinders, 6);
	TestTrue(TEXT("with a turbo"), Formula.bTurbo);
	TestEqual(TEXT("that revs to 15,000"), Formula.RedlineRpm, 15000.0f);
	TestTrue(TEXT("the limiter sits above the redline"), Formula.LimiterRpm > Formula.RedlineRpm);

	const ApexEngineSynth::FEngineSpec Prototype = ApexEngineAudio::MakeSpec(FApexEngineSoundSpec(), TEXT("LMP2"));
	TestEqual(TEXT("a prototype is a flat-plane eight"), Prototype.BankMask & 0xFFu, 0xAAu);

	// A row that knows the rev range but not the engine keeps the range.
	FApexEngineSoundSpec RangeOnly;
	RangeOnly.IdleRpm = 900.0f;
	RangeOnly.RedlineRpm = 9000.0f;
	const ApexEngineSynth::FEngineSpec Ranged = ApexEngineAudio::MakeSpec(RangeOnly, TEXT("GT3"));
	TestEqual(TEXT("range kept"), Ranged.RedlineRpm, 9000.0f);
	TestTrue(TEXT("limiter derived"), Ranged.LimiterRpm > 9000.0f && Ranged.LimiterRpm < 9300.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

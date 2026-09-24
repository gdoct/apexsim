#include "Audio/ApexEngineSoundWave.h"

#include "AudioDefines.h"
#include "Catalog/ApexCatalogRows.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundGroups.h"

namespace ApexEngineAudio
{
	ApexEngineSynth::FEngineSpec MakeSpec(const FApexEngineSoundSpec& Row, const FString& CarClass)
	{
		using namespace ApexEngineSynth;
		const bool bFormula = CarClass.Equals(TEXT("F1"), ESearchCase::IgnoreCase);
		const bool bPrototype = CarClass.StartsWith(TEXT("LMP"), ESearchCase::IgnoreCase)
			|| CarClass.Equals(TEXT("Hypercar"), ESearchCase::IgnoreCase);

		FEngineSpec Spec;
		if (Row.Cylinders > 0)
		{
			Spec.Cylinders = Row.Cylinders;
			Spec.BankMask = BankMask(Row.Cylinders, Row.bCrossplane);
			Spec.bTurbo = Row.bTurbo;
			Spec.ExhaustLengthM = Row.ExhaustLengthM;
			Spec.Muffling = Row.Muffling;
			Spec.Pops = Row.Pops;
			Spec.GearWhine = Row.GearWhine;
			Spec.IntakeRoar = Row.IntakeRoar;
		}
		else if (bFormula)
		{
			Spec.Cylinders = 6;
			Spec.BankMask = BankMask(6, false);
			Spec.bTurbo = true;
			Spec.ExhaustLengthM = 0.6f;
			Spec.Muffling = 0.05f;
			Spec.Pops = 0.15f;
			Spec.GearWhine = 0.4f;
			Spec.IntakeRoar = 0.4f;
		}
		else if (bPrototype)
		{
			Spec.BankMask = BankMask(8, false);
			Spec.ExhaustLengthM = 0.95f;
			Spec.Muffling = 0.15f;
			Spec.GearWhine = 0.45f;
		}
		// Anything else keeps FEngineSpec's own defaults: a crossplane race V8.

		if (Row.RedlineRpm > Row.IdleRpm && Row.IdleRpm > 0.0f)
		{
			Spec.IdleRpm = Row.IdleRpm;
			Spec.RedlineRpm = Row.RedlineRpm;
		}
		else if (bFormula)
		{
			Spec.IdleRpm = 4500.0f;
			Spec.RedlineRpm = 15000.0f;
		}
		// A limiter below the redline would stutter through the top of every gear.
		Spec.LimiterRpm = Row.LimiterRpm >= Spec.RedlineRpm ? Row.LimiterRpm : Spec.RedlineRpm * 1.015f;
		return Spec;
	}
}

namespace
{
	/** Nominal rate for the asset's metadata; the generator renders at the device's real one. */
	constexpr int32 EngineNominalSampleRate = 48000;

	/** Continuously renders the engine from the live RPM/gear/throttle the wave last set. */
	class FApexEngineSoundGenerator : public ISoundGenerator
	{
	public:
		FApexEngineSoundGenerator(TSharedRef<FApexEngineLiveState, ESPMode::ThreadSafe> InLive,
			float InSampleRate, int32 InNumChannels)
			: Live(MoveTemp(InLive))
			, State(MakeUnique<ApexEngineSynth::FState>())
			, Space(InNumChannels >= 2 ? MakeUnique<ApexSpace::FState>() : nullptr)
			, SampleRate(InSampleRate > 0.0f ? InSampleRate : static_cast<float>(EngineNominalSampleRate))
			, NumChannels(FMath::Max(1, InNumChannels))
		{
		}

		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			const int32 NumFrames = NumSamples / NumChannels;
			Mono.SetNumUninitialized(NumFrames);

			// The engine changes a handful of times in a car's life (the roster
			// arriving, a car swap), so the lock is all but never contended; the
			// serial keeps even the uncontended lock off the usual buffer.
			const uint32 Serial = Live->SpecSerial.load(std::memory_order_acquire);
			if (Serial != SeenSpecSerial)
			{
				FScopeLock Lock(&Live->SpecLock);
				Spec = Live->Spec;
				SeenSpecSerial = Serial;
			}

			ApexEngineSynth::FInputs Inputs;
			Inputs.Rpm = Live->Rpm.load(std::memory_order_relaxed);
			Inputs.Throttle = Live->Throttle.load(std::memory_order_relaxed);
			Inputs.Gear = Live->Gear.load(std::memory_order_relaxed);
			ApexEngineSynth::Render(*State, Spec, Inputs, SampleRate, Mono.GetData(), NumFrames);

			int32 Written = 0;
			if (Space && NumChannels == 2)
			{
				// The player's own car: the same engine, heard from its seat.
				const ApexSpace::ESeat Seat = static_cast<ApexSpace::ESeat>(Live->Seat.load(std::memory_order_relaxed));
				ApexSpace::Process(*Space, ApexSpace::ForSeat(Seat), SampleRate, Mono.GetData(), OutAudio, NumFrames);
				Written = NumFrames * 2;
			}
			else
			{
				for (int32 Frame = 0; Frame < NumFrames; ++Frame)
				{
					for (int32 Channel = 0; Channel < NumChannels; ++Channel)
					{
						OutAudio[Written++] = Mono[Frame];
					}
				}
			}
			// The mixer expects every sample it asked for to be written.
			while (Written < NumSamples)
			{
				OutAudio[Written++] = 0.0f;
			}
			return NumSamples;
		}

		// Runs for as long as the audio component plays it; nothing here ever finishes on its own.
		virtual bool IsFinished() const override { return false; }

	private:
		TSharedRef<FApexEngineLiveState, ESPMode::ThreadSafe> Live;
		/** On the heap: the exhaust's delay lines make it 20 KB, too much to carry by value. */
		TUniquePtr<ApexEngineSynth::FState> State;
		/** The own-car wave's cabin or trackside; null for the mono wave every other car plays. */
		TUniquePtr<ApexSpace::FState> Space;
		ApexEngineSynth::FEngineSpec Spec;
		uint32 SeenSpecSerial = 0;
		float SampleRate;
		int32 NumChannels;
		TArray<float> Mono;
	};
}

UApexEngineSoundWave::UApexEngineSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, Live(MakeShared<FApexEngineLiveState, ESPMode::ThreadSafe>())
{
	// bProcedural routes this wave to CreateSoundGenerator instead of a decoder;
	// bLooping plus the sentinel duration below tell the mixer never to stop it on its own.
	bProcedural = true;
	bLooping = true;
	NumChannels = 1;
	SetSampleRate(EngineNominalSampleRate);
	SoundGroup = SOUNDGROUP_Effects;
	Duration = INDEFINITELY_LOOPING_DURATION;
}

UApexEngineSoundWave* UApexEngineSoundWave::MakeOwnCar(UObject* Outer)
{
	UApexEngineSoundWave* Wave = NewObject<UApexEngineSoundWave>(Outer);
	// Before it is ever played: the mixer sizes the source from this.
	Wave->NumChannels = 2;
	return Wave;
}

void UApexEngineSoundWave::SetSeat(ApexSpace::ESeat Seat)
{
	Live->Seat.store(static_cast<uint8>(Seat), std::memory_order_relaxed);
}

void UApexEngineSoundWave::SetSpec(const ApexEngineSynth::FEngineSpec& Spec)
{
	{
		FScopeLock Lock(&Live->SpecLock);
		Live->Spec = Spec;
	}
	Live->SpecSerial.fetch_add(1, std::memory_order_release);
}

void UApexEngineSoundWave::SetLive(float Rpm, float Throttle, int32 Gear)
{
	Live->Rpm.store(Rpm, std::memory_order_relaxed);
	Live->Throttle.store(FMath::Clamp(Throttle, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->Gear.store(Gear, std::memory_order_relaxed);
}

ISoundGeneratorPtr UApexEngineSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	// Called on the audio thread; everything it needs is read from Live.
	return MakeShared<FApexEngineSoundGenerator, ESPMode::ThreadSafe>(Live, InParams.SampleRate, InParams.NumChannels);
}

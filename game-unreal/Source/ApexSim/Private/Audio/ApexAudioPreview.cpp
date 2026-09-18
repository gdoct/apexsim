#include "Audio/ApexEngineSound.h"
#include "Audio/ApexEngineSoundWave.h"
#include "Audio/ApexListenerSpace.h"
#include "Audio/ApexRoadSound.h"
#include "ApexSim.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/DataTable.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * `apexsim.audio.RenderCars [dir]` — every car in the catalog driven through
 * the same scripted run (idle, two blips, up through the gears flat out, the
 * limiter, a lift and the overrun down through the box, a part-throttle
 * cruise) and written twice: `<folder>.wav` is the engine as it leaves the
 * tailpipe, mono, which is what goes into the world for everybody else to
 * hear, and `<folder>_own.wav` is what its driver hears, in stereo, from the
 * seat (ApexSpace: a closed cabin, or an open cockpit for an F1). Plus
 * `road.wav` with each tyre and road voice in turn. Into Saved/Audio unless
 * told otherwise.
 *
 * The synthesisers are judged by ear, and a race is a poor place to do it: one
 * car, whatever revs the corner allows, under the wind and everybody else's
 * engine. This is every engine over its whole range, alone, in a file that
 * can be played twice — what a `[sound]` table is tuned against.
 */
namespace
{
	constexpr int32 PreviewSampleRate = 48000;
	/** The car actor feeds the synth once a render frame; so does this. */
	constexpr int32 PreviewBlock = PreviewSampleRate / 120;
	constexpr float PreviewStep = 1.0f / 120.0f;

	/** `Samples` interleaved when there is more than one channel. */
	bool SaveWav(const FString& Path, const TArray<float>& Samples, uint16 Channels = 1)
	{
		const TArray<float>& Frames = Samples;
		const uint32 DataBytes = static_cast<uint32>(Frames.Num()) * 2u;
		TArray<uint8> Bytes;
		Bytes.Reserve(44 + DataBytes);
		auto Tag = [&Bytes](const ANSICHAR* Text) { Bytes.Append(reinterpret_cast<const uint8*>(Text), 4); };
		auto U32 = [&Bytes](uint32 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		auto U16 = [&Bytes](uint16 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2); };
		Tag("RIFF"); U32(36u + DataBytes); Tag("WAVE");
		Tag("fmt "); U32(16); U16(1); U16(Channels); U32(PreviewSampleRate); U32(PreviewSampleRate * 2 * Channels);
		U16(static_cast<uint16>(2 * Channels)); U16(16);
		Tag("data"); U32(DataBytes);
		for (const float Sample : Frames)
		{
			U16(static_cast<uint16>(static_cast<int16>(FMath::Clamp(Sample, -1.0f, 1.0f) * 32767.0f)));
		}
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	/** One engine through the scripted run. */
	TArray<float> RenderDrive(const ApexEngineSynth::FEngineSpec& Spec, int32 Gears)
	{
		const TUniquePtr<ApexEngineSynth::FState> State = MakeUnique<ApexEngineSynth::FState>();
		ApexEngineSynth::FInputs Inputs;
		Inputs.Rpm = Spec.IdleRpm;
		Inputs.Gear = 1;
		TArray<float> Frames;

		// `Advance` moves the revs and throttle one step and says whether the phase goes on.
		auto Phase = [&](float MaxSeconds, TFunctionRef<bool(float)> Advance)
		{
			for (float Elapsed = 0.0f; Elapsed < MaxSeconds && Advance(PreviewStep); Elapsed += PreviewStep)
			{
				const int32 At = Frames.AddZeroed(PreviewBlock);
				ApexEngineSynth::Render(*State, Spec, Inputs, PreviewSampleRate, Frames.GetData() + At, PreviewBlock);
			}
		};
		const float Range = Spec.RedlineRpm - Spec.IdleRpm;
		auto Fall = [&](float RevsPerSecond, float Dt) { Inputs.Rpm = FMath::Max(Spec.IdleRpm, Inputs.Rpm - Range * RevsPerSecond * Dt); };

		Phase(2.0f, [&](float) { Inputs.Throttle = 0.0f; return true; });
		for (int32 Blip = 0; Blip < 2; ++Blip)
		{
			Phase(0.35f, [&](float Dt) { Inputs.Throttle = 0.8f; Inputs.Rpm += Range * 1.6f * Dt; return true; });
			Phase(0.9f, [&](float Dt) { Inputs.Throttle = 0.0f; Fall(0.9f, Dt); return true; });
		}
		for (int32 Gear = 1; Gear <= Gears; ++Gear)
		{
			Inputs.Gear = Gear;
			Phase(30.0f, [&](float Dt)
			{
				Inputs.Throttle = 1.0f;
				Inputs.Rpm += Range * 0.9f / static_cast<float>(Gear) * Dt;
				return Inputs.Rpm < Spec.RedlineRpm * 0.985f;
			});
			if (Gear < Gears)
			{
				Inputs.Rpm *= 0.78f;
			}
		}
		Phase(1.0f, [&](float) { Inputs.Rpm = Spec.LimiterRpm; return true; });
		for (int32 Gear = Gears; Gear > 2; --Gear)
		{
			Inputs.Gear = Gear;
			Phase(0.9f, [&](float Dt) { Inputs.Throttle = 0.0f; Fall(0.28f, Dt); return true; });
			Inputs.Rpm = FMath::Min(Inputs.Rpm * 1.25f, Spec.RedlineRpm);
		}
		Inputs.Gear = 2;
		Phase(1.5f, [&](float Dt) { Fall(0.4f, Dt); return true; });
		Phase(2.0f, [&](float) { Inputs.Throttle = 0.35f; Inputs.Rpm = Spec.IdleRpm + Range * 0.45f; return true; });
		return Frames;
	}

	/** A mono render as its driver hears it: interleaved stereo. */
	TArray<float> HeardFrom(ApexSpace::ESeat Seat, const TArray<float>& Mono)
	{
		const TUniquePtr<ApexSpace::FState> State = MakeUnique<ApexSpace::FState>();
		const ApexSpace::FParams Params = ApexSpace::ForSeat(Seat);
		TArray<float> Stereo;
		Stereo.SetNumZeroed(Mono.Num() * 2);
		for (int32 At = 0; At + PreviewBlock <= Mono.Num(); At += PreviewBlock)
		{
			ApexSpace::Process(*State, Params, PreviewSampleRate, Mono.GetData() + At, Stereo.GetData() + At * 2, PreviewBlock);
		}
		return Stereo;
	}

	/** Each road voice for a second and a half, in the order the comment in the log gives. */
	TArray<float> RenderRoadTour(FString& OutOrder)
	{
		struct FStop
		{
			const TCHAR* Name;
			ApexRoadSynth::FInputs Levels;
			float BumpMps;
			float ImpactMps;
		};
		auto At = [](float Speed) { ApexRoadSynth::FInputs L; L.SpeedMps = Speed; return L; };
		TArray<FStop> Stops;
		Stops.Add({TEXT("rolling at 110 km/h"), At(30.0f), 0.0f, 0.0f});
		Stops.Add({TEXT("rolling at 300 km/h"), At(85.0f), 0.0f, 0.0f});
		{ FStop S{TEXT("understeer"), At(40.0f), 0.0f, 0.0f}; S.Levels.FrontSlide = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("oversteer"), At(40.0f), 0.0f, 0.0f}; S.Levels.RearSlide = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("lockup"), At(45.0f), 0.0f, 0.0f}; S.Levels.Lockup = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("wheelspin"), At(6.0f), 0.0f, 0.0f}; S.Levels.Wheelspin = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("left kerb at 145 km/h"), At(40.0f), 0.0f, 0.0f}; S.Levels.CurbLeft = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("both kerbs at 250 km/h"), At(70.0f), 0.0f, 0.0f}; S.Levels.CurbLeft = S.Levels.CurbRight = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("grass"), At(35.0f), 0.0f, 0.0f}; S.Levels.OffTrack = 1.0f; Stops.Add(S); }
		{ FStop S{TEXT("sliding in the wet"), At(40.0f), 0.0f, 0.0f}; S.Levels.FrontSlide = 1.0f; S.Levels.bWet = true; Stops.Add(S); }
		Stops.Add({TEXT("a bump"), At(30.0f), 1.5f, 0.0f});
		Stops.Add({TEXT("contact"), At(30.0f), 0.0f, 9.0f});

		ApexRoadSynth::FState State;
		TArray<float> Frames;
		for (const FStop& Stop : Stops)
		{
			OutOrder += (OutOrder.IsEmpty() ? TEXT("") : TEXT(", "));
			OutOrder += Stop.Name;
			for (int32 Step = 0; Step < 180; ++Step)
			{
				if (Step == 60)
				{
					ApexRoadSynth::Hit(State, Stop.BumpMps, Stop.ImpactMps);
				}
				const int32 Offset = Frames.AddZeroed(PreviewBlock);
				ApexRoadSynth::Render(State, Stop.Levels, PreviewSampleRate, Frames.GetData() + Offset, PreviewBlock);
			}
		}
		return Frames;
	}

	void RenderCars(const TArray<FString>& Args)
	{
		const FString Dir = Args.Num() > 0 ? Args[0] : FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Audio"));
		const UDataTable* Table = LoadObject<UDataTable>(nullptr, TEXT("/Game/Data/DT_CarCatalog.DT_CarCatalog"));
		if (!Table || Table->GetRowStruct() != FApexCarCatalogRow::StaticStruct())
		{
			UE_LOG(LogApexSim, Error, TEXT("apexsim.audio.RenderCars: no car catalog at /Game/Data/DT_CarCatalog"));
			return;
		}
		for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
		{
			const FApexCarCatalogRow* Row = reinterpret_cast<const FApexCarCatalogRow*>(Pair.Value);
			const ApexEngineSynth::FEngineSpec Spec = ApexEngineAudio::MakeSpec(Row->EngineSound, Row->CarClass);
			const int32 Gears = Row->CarClass.Equals(TEXT("F1"), ESearchCase::IgnoreCase) ? 8 : 6;
			const FString Name = Row->FolderName.IsEmpty() ? Pair.Key.ToString() : Row->FolderName;
			const FString Path = FPaths::Combine(Dir, Name + TEXT(".wav"));
			const TArray<float> Frames = RenderDrive(Spec, Gears);
			const bool bFormula = Row->CarClass.Equals(TEXT("F1"), ESearchCase::IgnoreCase);
			const TArray<float> Own = HeardFrom(bFormula ? ApexSpace::ESeat::OpenCockpit : ApexSpace::ESeat::Cabin, Frames);
			const bool bSaved = SaveWav(Path, Frames)
				&& SaveWav(FPaths::Combine(Dir, Name + TEXT("_own.wav")), Own, 2);
			UE_LOG(LogApexSim, Display, TEXT("%s: %d cyl%s%s, %.0f-%.0f rpm, pipe %.2f m, muffling %.2f, pops %.2f%s -> %s (%.1f s)%s"),
				*Row->DisplayName, Spec.Cylinders, (Spec.BankMask & 0xFFu) == 0xB2u ? TEXT(" crossplane") : TEXT(""),
				Spec.bTurbo ? TEXT(" turbo") : TEXT(""), Spec.IdleRpm, Spec.RedlineRpm, Spec.ExhaustLengthM, Spec.Muffling,
				Spec.Pops, Row->EngineSound.Cylinders > 0 ? TEXT("") : TEXT(" [no [sound] on the row: class default]"), *Path,
				static_cast<float>(Frames.Num()) / PreviewSampleRate, bSaved ? TEXT("") : TEXT(" FAILED TO WRITE"));
		}

		FString Order;
		const TArray<float> Road = RenderRoadTour(Order);
		const FString RoadPath = FPaths::Combine(Dir, TEXT("road.wav"));
		const bool bRoadSaved = SaveWav(RoadPath, Road);
		UE_LOG(LogApexSim, Display, TEXT("road, 1.5 s each: %s -> %s%s"), *Order, *RoadPath,
			bRoadSaved ? TEXT("") : TEXT(" FAILED TO WRITE"));
	}

	FAutoConsoleCommand RenderCarsCommand(
		TEXT("apexsim.audio.RenderCars"),
		TEXT("Render every catalog car's engine through a scripted drive, and the road voices, to WAV files. Optional: output directory (default Saved/Audio)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RenderCars));
}

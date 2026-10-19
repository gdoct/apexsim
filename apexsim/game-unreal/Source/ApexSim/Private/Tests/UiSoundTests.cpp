#include "ApexTestCommon.h"
#include "Audio/ApexUiSound.h"
#include "Audio/ApexWavFile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString CueName(EApexUiSound Sound)
	{
		return StaticEnum<EApexUiSound>()->GetNameStringByValue(static_cast<int64>(Sound));
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUiSoundRenderTest,
	"ApexSim.UI.SoundCuesRender",
	ApexTestFlags)

bool FApexUiSoundRenderTest::RunTest(const FString& Parameters)
{
	// Every cue has to be a real, short, click-free tone: the mixer will play
	// whatever comes out of Render straight into the player's headphones, and a
	// menu tick that pops or peaks is worse than no sound at all.
	for (int32 Index = 0; Index < static_cast<int32>(EApexUiSound::Count); ++Index)
	{
		const EApexUiSound Sound = static_cast<EApexUiSound>(Index);
		const FString Name = CueName(Sound);

		TArray<float> Samples;
		ApexUiSynth::Render(Sound, 48000.0f, Samples);

		TestTrue(*FString::Printf(TEXT("%s renders something"), *Name), Samples.Num() > 0);
		if (Samples.Num() == 0)
		{
			continue;
		}

		const float Seconds = Samples.Num() / 48000.0f;
		TestTrue(*FString::Printf(TEXT("%s is short (%.3fs)"), *Name, Seconds), Seconds > 0.01f && Seconds < 0.5f);
		TestTrue(*FString::Printf(TEXT("%s duration matches DurationSeconds"), *Name),
			FMath::IsNearlyEqual(Seconds, ApexUiSynth::DurationSeconds(Sound), 0.001f));

		float Peak = 0.0f;
		bool bFinite = true;
		for (float Sample : Samples)
		{
			bFinite = bFinite && FMath::IsFinite(Sample);
			Peak = FMath::Max(Peak, FMath::Abs(Sample));
		}
		TestTrue(*FString::Printf(TEXT("%s has no NaN or infinity"), *Name), bFinite);
		TestTrue(*FString::Printf(TEXT("%s is audible (peak %.2f)"), *Name, Peak), Peak > 0.05f);
		TestTrue(*FString::Printf(TEXT("%s does not clip (peak %.2f)"), *Name, Peak), Peak <= 1.0f);

		// No click at either end: the first and last samples sit at silence.
		TestTrue(*FString::Printf(TEXT("%s starts silent"), *Name), FMath::Abs(Samples[0]) < 0.01f);
		TestTrue(*FString::Printf(TEXT("%s ends silent"), *Name), FMath::Abs(Samples.Last()) < 0.01f);
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexUiSoundRateIndependenceTest,
	"ApexSim.UI.SoundCuesRateIndependent",
	ApexTestFlags)

bool FApexUiSoundRateIndependenceTest::RunTest(const FString& Parameters)
{
	// The cue is rendered at whatever rate the audio device runs — 44.1k on
	// some machines, 48k on most — and must last the same wall-clock time and
	// carry the same energy on both, or the menu would sound different per PC.
	for (int32 Index = 0; Index < static_cast<int32>(EApexUiSound::Count); ++Index)
	{
		const EApexUiSound Sound = static_cast<EApexUiSound>(Index);
		const FString Name = CueName(Sound);

		TArray<float> At44;
		TArray<float> At48;
		ApexUiSynth::Render(Sound, 44100.0f, At44);
		ApexUiSynth::Render(Sound, 48000.0f, At48);

		const float Seconds44 = At44.Num() / 44100.0f;
		const float Seconds48 = At48.Num() / 48000.0f;
		TestTrue(*FString::Printf(TEXT("%s lasts the same at both rates"), *Name),
			FMath::IsNearlyEqual(Seconds44, Seconds48, 0.001f));

		auto Rms = [](const TArray<float>& Samples)
		{
			double Sum = 0.0;
			for (float Sample : Samples)
			{
				Sum += Sample * Sample;
			}
			return Samples.Num() > 0 ? FMath::Sqrt(Sum / Samples.Num()) : 0.0;
		};
		const double Rms44 = Rms(At44);
		const double Rms48 = Rms(At48);
		TestTrue(*FString::Printf(TEXT("%s is as loud at both rates (%.3f vs %.3f)"), *Name, Rms44, Rms48),
			FMath::IsNearlyEqual(Rms44, Rms48, 0.02));
	}

	// A nonsense rate must not produce a buffer the mixer would try to play.
	TArray<float> Samples;
	ApexUiSynth::Render(EApexUiSound::Accept, 0.0f, Samples);
	TestEqual(TEXT("zero rate renders nothing"), Samples.Num(), 0);

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexWavFileTest,
	"ApexSim.Audio.Wav",
	ApexTestFlags)

bool FApexWavFileTest::RunTest(const FString& Parameters)
{
	// A recorded replay's sound, cut and padded to its pictures.
	const float Stereo[] = { 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, 0.0f };
	const TArray<uint8> Cut = ApexWav::Encode16(TConstArrayView<float>(Stereo, 6), 2, 48000, 2);
	TestEqual(TEXT("header and two frames"), Cut.Num(), 44 + 2 * 2 * 2);
	TestTrue(TEXT("a RIFF WAVE"), FMemory::Memcmp(Cut.GetData(), "RIFF", 4) == 0 && FMemory::Memcmp(Cut.GetData() + 8, "WAVE", 4) == 0);
	auto At16 = [&Cut](int32 Offset) { return static_cast<int16>(Cut[Offset] | (Cut[Offset + 1] << 8)); };
	auto At32 = [&Cut](int32 Offset) { return static_cast<int32>(Cut[Offset] | (Cut[Offset + 1] << 8) | (Cut[Offset + 2] << 16) | (Cut[Offset + 3] << 24)); };
	TestEqual(TEXT("two channels"), (int32)At16(22), 2);
	TestEqual(TEXT("48 kHz"), At32(24), 48000);
	TestEqual(TEXT("its data's size"), At32(40), 8);
	TestEqual(TEXT("a half"), (int32)At16(44), 16384);
	TestEqual(TEXT("full scale down"), (int32)At16(50), -32767);

	const TArray<uint8> Padded = ApexWav::Encode16(TConstArrayView<float>(Stereo, 6), 2, 48000, 5);
	TestEqual(TEXT("padded to five frames"), Padded.Num(), 44 + 5 * 4);
	TestEqual(TEXT("an over-range sample is clipped"), (int32)static_cast<int16>(Padded[52] | (Padded[53] << 8)), 32767);
	TestEqual(TEXT("silence after the end"), (int32)static_cast<int16>(Padded[60] | (Padded[61] << 8)), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

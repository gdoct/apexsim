#include "Misc/AutomationTest.h"

#include "Audio/ApexUiSound.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

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

#endif // WITH_DEV_AUTOMATION_TESTS

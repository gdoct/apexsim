#include "ApexTestCommon.h"
#include "Audio/ApexListenerSpace.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Mono through a seat's space; interleaved stereo back. The state is 150 KB: heap. */
	TArray<float> ThroughSpace(ApexSpace::ESeat Seat, const TArray<float>& Mono, float SampleRate = 48000.0f)
	{
		const TUniquePtr<ApexSpace::FState> State = MakeUnique<ApexSpace::FState>();
		const ApexSpace::FParams Params = ApexSpace::ForSeat(Seat);
		TArray<float> Stereo;
		Stereo.SetNumZeroed(Mono.Num() * 2);
		constexpr int32 Block = 480;
		for (int32 At = 0; At + Block <= Mono.Num(); At += Block)
		{
			ApexSpace::Process(*State, Params, SampleRate, Mono.GetData() + At, Stereo.GetData() + At * 2, Block);
		}
		return Stereo;
	}

	TArray<float> SpaceTone(float Hz, float Amplitude, float Seconds, float SampleRate = 48000.0f)
	{
		TArray<float> Mono;
		Mono.SetNumZeroed(static_cast<int32>(SampleRate * Seconds) / 480 * 480);
		for (int32 Index = 0; Index < Mono.Num(); ++Index)
		{
			Mono[Index] = Amplitude * FMath::Sin(2.0f * PI * Hz * static_cast<float>(Index) / SampleRate);
		}
		return Mono;
	}

	/** RMS of the left channel over the second half of the buffer: settled. */
	float SpaceLevel(const TArray<float>& Stereo)
	{
		double Sum = 0.0;
		const int32 Frames = Stereo.Num() / 2;
		for (int32 Frame = Frames / 2; Frame < Frames; ++Frame)
		{
			Sum += Stereo[Frame * 2] * Stereo[Frame * 2];
		}
		return static_cast<float>(FMath::Sqrt(Sum / FMath::Max(Frames / 2, 1)));
	}

	/** Seconds until a click's tail has fallen 60 dB from where it stood 20 ms in. */
	float SpaceRingSeconds(ApexSpace::ESeat Seat, float SampleRate)
	{
		TArray<float> Click;
		Click.SetNumZeroed(static_cast<int32>(SampleRate * 3.0f) / 480 * 480);
		Click[0] = 0.5f;
		const TArray<float> Stereo = ThroughSpace(Seat, Click, SampleRate);
		const int32 Window = static_cast<int32>(SampleRate / 100.0f);
		double Reference = 0.0;
		int32 LastAudible = 0;
		for (int32 At = Window * 2; At + Window <= Click.Num(); At += Window)
		{
			double Energy = 0.0;
			for (int32 Frame = At; Frame < At + Window; ++Frame)
			{
				Energy += Stereo[Frame * 2] * Stereo[Frame * 2];
			}
			if (At == Window * 2)
			{
				Reference = Energy;
			}
			else if (Energy > Reference * 1.0e-6)
			{
				LastAudible = At;
			}
		}
		return static_cast<float>(LastAudible) / SampleRate;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexListenerSpaceSeatsTest,
	"ApexSim.Audio.SpaceSeats",
	ApexTestFlags)

bool FApexListenerSpaceSeatsTest::RunTest(const FString& Parameters)
{
	// Nobody's seat: what goes in comes out, on both sides.
	const TArray<float> Tone = SpaceTone(440.0f, 0.1f, 0.5f);
	const TArray<float> Untouched = ThroughSpace(ApexSpace::ESeat::None, Tone);
	float WorstError = 0.0f;
	for (int32 Frame = 0; Frame < Tone.Num(); ++Frame)
	{
		WorstError = FMath::Max3(WorstError, FMath::Abs(Untouched[Frame * 2] - Tone[Frame]),
			FMath::Abs(Untouched[Frame * 2 + 1] - Tone[Frame]));
	}
	TestTrue(*FString::Printf(TEXT("no seat, no space (worst error %.5f)"), WorstError), WorstError < 0.001f);

	// The player's own car has weight: every seat lifts the low orders against
	// the top, and a closed cabin's bulkhead takes the top off as well.
	for (const ApexSpace::ESeat Seat : {ApexSpace::ESeat::Cabin, ApexSpace::ESeat::OpenCockpit, ApexSpace::ESeat::Chase})
	{
		// The low end is kept out of the reverb, so one tone measures it. The
		// top is heard through the combs, where any one frequency may sit on a
		// peak or in a notch: several, averaged.
		const float Low = SpaceLevel(ThroughSpace(Seat, SpaceTone(70.0f, 0.1f, 1.0f)));
		float High = 0.0f;
		const float Tops[] = {1530.0f, 1790.0f, 2110.0f, 2470.0f, 2930.0f};
		for (const float Hz : Tops)
		{
			High += SpaceLevel(ThroughSpace(Seat, SpaceTone(Hz, 0.1f, 1.0f))) / UE_ARRAY_COUNT(Tops);
		}
		TestTrue(*FString::Printf(TEXT("seat %d carries the low end (70 Hz %.3f against 1.5-3 kHz %.3f)"), static_cast<int32>(Seat), Low, High),
			Low > High * 1.4f);
	}
	const float CabinTop = SpaceLevel(ThroughSpace(ApexSpace::ESeat::Cabin, SpaceTone(8000.0f, 0.1f, 1.0f)));
	const float OpenTop = SpaceLevel(ThroughSpace(ApexSpace::ESeat::OpenCockpit, SpaceTone(8000.0f, 0.1f, 1.0f)));
	TestTrue(*FString::Printf(TEXT("a bulkhead dulls what an open cockpit does not (%.3f against %.3f)"), CabinTop, OpenTop),
		CabinTop < OpenTop * 0.7f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexListenerSpaceReverbTest,
	"ApexSim.Audio.SpaceReverb",
	ApexTestFlags)

bool FApexListenerSpaceReverbTest::RunTest(const FString& Parameters)
{
	// Each space rings for about as long as it says, at either device rate: a
	// cabin is over in a third of a second, the trackside takes a full one.
	for (const float SampleRate : {48000.0f, 44100.0f})
	{
		for (const ApexSpace::ESeat Seat : {ApexSpace::ESeat::Cabin, ApexSpace::ESeat::Chase})
		{
			const float Wanted = ApexSpace::ForSeat(Seat).DecaySeconds;
			const float Rings = SpaceRingSeconds(Seat, SampleRate);
			TestTrue(*FString::Printf(TEXT("seat %d rings %.2f s of %.2f at %.0f Hz"), static_cast<int32>(Seat), Rings, Wanted, SampleRate),
				Rings > Wanted * 0.6f && Rings < Wanted * 1.4f);
		}
	}
	TestEqual(TEXT("nobody's seat does not ring"), SpaceRingSeconds(ApexSpace::ESeat::None, 48000.0f), 0.0f);

	// The two ears get different echoes: after a click the channels part company.
	TArray<float> Click;
	Click.SetNumZeroed(48000);
	Click[0] = 0.5f;
	const TArray<float> Stereo = ThroughSpace(ApexSpace::ESeat::Chase, Click);
	double Cross = 0.0;
	double Left = 0.0;
	double Right = 0.0;
	for (int32 Frame = 2400; Frame < 48000; ++Frame)
	{
		Cross += Stereo[Frame * 2] * Stereo[Frame * 2 + 1];
		Left += Stereo[Frame * 2] * Stereo[Frame * 2];
		Right += Stereo[Frame * 2 + 1] * Stereo[Frame * 2 + 1];
	}
	const double Correlation = Cross / FMath::Sqrt(FMath::Max(Left * Right, 1.0e-30));
	TestTrue(*FString::Printf(TEXT("the tail is stereo (correlation %.2f)"), Correlation), FMath::Abs(Correlation) < 0.6);
	TestTrue(TEXT("and there is a tail"), Left > 0.0 && Right > 0.0);

	// Flat out into every seat, louder than the synth ever is: finite, inside the rails.
	for (const ApexSpace::ESeat Seat : {ApexSpace::ESeat::Cabin, ApexSpace::ESeat::OpenCockpit, ApexSpace::ESeat::Chase})
	{
		const TArray<float> Loud = ThroughSpace(Seat, SpaceTone(110.0f, 1.0f, 1.0f));
		bool bFinite = true;
		float Peak = 0.0f;
		for (const float Sample : Loud)
		{
			bFinite = bFinite && FMath::IsFinite(Sample);
			Peak = FMath::Max(Peak, FMath::Abs(Sample));
		}
		TestTrue(*FString::Printf(TEXT("seat %d is finite and does not clip (peak %.2f)"), static_cast<int32>(Seat), Peak),
			bFinite && Peak <= 1.0f);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#include "ApexProtocolTypes.h"

bool FApexGhostLap::SampleAt(float TimeMs, FApexGhostSample& Out) const
{
	if (Samples.Num() == 0)
	{
		return false;
	}
	if (TimeMs <= Samples[0].TimeMs)
	{
		Out = Samples[0];
		return true;
	}
	const FApexGhostSample& Last = Samples.Last();
	if (TimeMs >= Last.TimeMs)
	{
		Out = Last;
		return false;
	}

	// Binary search for the first sample past the time; the one before it
	// is where the blend starts.
	int32 Lo = 0;
	int32 Hi = Samples.Num() - 1;
	while (Hi - Lo > 1)
	{
		const int32 Mid = (Lo + Hi) / 2;
		if (Samples[Mid].TimeMs <= TimeMs)
		{
			Lo = Mid;
		}
		else
		{
			Hi = Mid;
		}
	}
	const FApexGhostSample& A = Samples[Lo];
	const FApexGhostSample& B = Samples[Hi];
	const float Span = static_cast<float>(B.TimeMs - A.TimeMs);
	const float T = Span > 0.0f ? FMath::Clamp((TimeMs - A.TimeMs) / Span, 0.0f, 1.0f) : 0.0f;

	Out.TimeMs = FMath::RoundToInt(TimeMs);
	Out.Position = FMath::Lerp(A.Position, B.Position, T);
	// Angles the short way round: a lap crosses the ±π seam somewhere.
	auto LerpAngle = [T](float From, float To)
	{
		const float Delta = FMath::UnwindRadians(To - From);
		return FMath::UnwindRadians(From + Delta * T);
	};
	Out.YawRad = LerpAngle(A.YawRad, B.YawRad);
	Out.PitchRad = LerpAngle(A.PitchRad, B.PitchRad);
	Out.RollRad = LerpAngle(A.RollRad, B.RollRad);
	Out.SpeedMps = FMath::Lerp(A.SpeedMps, B.SpeedMps, T);
	Out.Steering = FMath::Lerp(A.Steering, B.Steering, T);
	Out.EngineRpm = FMath::Lerp(A.EngineRpm, B.EngineRpm, T);
	// A gear is a step, not a blend: it changes when the sample does.
	Out.Gear = T < 0.5f ? A.Gear : B.Gear;
	return true;
}

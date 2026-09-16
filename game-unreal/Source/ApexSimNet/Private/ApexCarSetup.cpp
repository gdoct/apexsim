#include "ApexProtocolTypes.h"

// Mirrors `KNOBS` and the per-click constants in server/src/car_setup.rs.
// The order is the wire order; the ranges and click sizes are the server's
// and are shown to the player, so change both sides together.

namespace ApexCarSetup
{
	namespace
	{
		constexpr int32 MaxClicks = 5;

		const FKnob Knobs[FApexCarSetup::KnobCount] = {
			{ "tyre_pressure_front", -MaxClicks, MaxClicks, 5.0f,   TEXT(" kPa") },
			{ "tyre_pressure_rear",  -MaxClicks, MaxClicks, 5.0f,   TEXT(" kPa") },
			{ "rev_limiter",         -MaxClicks, 0,         100.0f, TEXT(" rpm") },
			{ "engine_braking",      -MaxClicks, MaxClicks, 15.0f,  TEXT("%") },
			{ "final_drive",         -MaxClicks, MaxClicks, 2.0f,   TEXT("%") },
			{ "gear_spread",         -MaxClicks, MaxClicks, 2.0f,   TEXT("%") },
			{ "torque_map",          -MaxClicks, 0,         4.0f,   TEXT("%") },
			{ "brake_bias",          -MaxClicks, MaxClicks, 1.0f,   TEXT("% front") },
			{ "spring_front",        -MaxClicks, MaxClicks, 4.0f,   TEXT("%") },
			{ "spring_rear",         -MaxClicks, MaxClicks, 4.0f,   TEXT("%") },
			{ "damper_front",        -MaxClicks, MaxClicks, 5.0f,   TEXT("%") },
			{ "damper_rear",         -MaxClicks, MaxClicks, 5.0f,   TEXT("%") },
			{ "anti_roll_front",     -MaxClicks, MaxClicks, 8.0f,   TEXT("%") },
			{ "anti_roll_rear",      -MaxClicks, MaxClicks, 8.0f,   TEXT("%") },
		};
	}

	const FKnob& Knob(int32 Index)
	{
		return Knobs[FMath::Clamp(Index, 0, FApexCarSetup::KnobCount - 1)];
	}

	FString Describe(int32 Index, int32 Clicks)
	{
		if (Clicks == 0)
		{
			return TEXT("0");
		}
		const FKnob& K = Knob(Index);
		const float Effect = Clicks * K.PerClick;
		const bool bWhole = FMath::IsNearlyEqual(Effect, FMath::RoundToFloat(Effect));
		const FString EffectText = bWhole
			? FString::Printf(TEXT("%+d"), FMath::RoundToInt(Effect))
			: FString::Printf(TEXT("%+.1f"), Effect);
		return FString::Printf(TEXT("%+d  (%s%s)"), Clicks, *EffectText, K.Unit);
	}
}

bool FApexCarSetup::SetClick(int32 Knob, int32 Value)
{
	if (!Clicks.IsValidIndex(Knob))
	{
		return false;
	}
	const ApexCarSetup::FKnob& K = ApexCarSetup::Knob(Knob);
	const int32 Pinned = FMath::Clamp(Value, K.Min, K.Max);
	if (Clicks[Knob] == Pinned)
	{
		return false;
	}
	Clicks[Knob] = Pinned;
	return true;
}

void FApexCarSetup::Clamp()
{
	// A setup loaded from an older slot may be short or long; the wire
	// always carries exactly KnobCount.
	Clicks.SetNum(KnobCount);
	for (int32 Index = 0; Index < KnobCount; ++Index)
	{
		const ApexCarSetup::FKnob& K = ApexCarSetup::Knob(Index);
		Clicks[Index] = FMath::Clamp(Clicks[Index], K.Min, K.Max);
	}
}

bool FApexCarSetup::IsStock() const
{
	return CountChanged() == 0;
}

int32 FApexCarSetup::CountChanged() const
{
	int32 Count = 0;
	for (const int32 Click : Clicks)
	{
		Count += Click != 0 ? 1 : 0;
	}
	return Count;
}

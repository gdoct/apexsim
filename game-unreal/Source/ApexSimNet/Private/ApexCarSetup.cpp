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
			// Laps, not a figure of the car: the server fills the tank for
			// the run (a race's distance, three hotlap laps) plus these.
			{ "fuel_load",           -MaxClicks, MaxClicks, 1.0f,   TEXT(" laps") },
			// Aero: each wing scales its axle's downforce (and the drag);
			// the ride heights move the car on its aero map.
			{ "front_wing",          -MaxClicks, MaxClicks, 5.0f,   TEXT("%") },
			{ "rear_wing",           -MaxClicks, MaxClicks, 5.0f,   TEXT("%") },
			{ "ride_height_front",   -MaxClicks, MaxClicks, 2.0f,   TEXT(" mm") },
			{ "ride_height_rear",    -MaxClicks, MaxClicks, 2.0f,   TEXT(" mm") },
			// Not a figure but the next set's compound: read out by name. The
			// range is wide on purpose: a car's own list may hold more than the
			// five defaults, the sheet says how many (its Lo/Hi), and the
			// server clamps to the car's anyway.
			{ "tyre_compound",       -9,         9,         1.0f,   TEXT("") },
			{ "brake_ducts",         -MaxClicks, MaxClicks, 10.0f,  TEXT("% air") },
			{ "camber_front",        -MaxClicks, MaxClicks, -0.25f,  TEXT("°") },
			{ "camber_rear",         -MaxClicks, MaxClicks, -0.25f,  TEXT("°") },
			{ "toe_front",           -MaxClicks, MaxClicks, 0.05f,  TEXT("°") },
			{ "toe_rear",            -MaxClicks, MaxClicks, 0.05f,  TEXT("°") },
			// The rear ducts, appended after the geometry; "brake_ducts"
			// above is the front pair.
			{ "brake_ducts_rear",    -MaxClicks, MaxClicks, 10.0f,  TEXT("% air") },
			// Not a figure but the pad compound: read out by name.
			{ "brake_pads",          -1,         1,         1.0f,   TEXT("") },
			// The radiator inlet: cooling for a little drag per click open.
			{ "radiator",            -MaxClicks, MaxClicks, 8.0f,   TEXT("%") },
			// The hybrid: the mode a run starts in (read out by name), the
			// motor's recovery power (down only), and the deploy map (read
			// out by name).
			{ "ers_start_mode",      -1,         1,         1.0f,   TEXT("") },
			{ "ers_regen",           -MaxClicks, 0,         10.0f,  TEXT("%") },
			{ "ers_deploy_map",      -MaxClicks, MaxClicks, 1.0f,   TEXT("") },
		};
	}

	const FKnob& Knob(int32 Index)
	{
		return Knobs[FMath::Clamp(Index, 0, FApexCarSetup::KnobCount - 1)];
	}

	FString Describe(int32 Index, int32 Clicks)
	{
		if (Index == TyreCompound)
		{
			return Clicks > 0 ? TEXT("Soft") : Clicks == 0 ? TEXT("Medium") : Clicks == -1 ? TEXT("Hard")
				: Clicks == -2 ? TEXT("Intermediate") : TEXT("Wet");
		}
		if (Index == BrakePads)
		{
			return Clicks > 0 ? TEXT("Sprint") : Clicks == 0 ? TEXT("Standard") : TEXT("Endurance");
		}
		if (Index == ErsStartMode)
		{
			return Clicks > 0 ? TEXT("Attack") : Clicks == 0 ? TEXT("Balanced") : TEXT("Harvest");
		}
		if (Index == ErsDeployMap)
		{
			return Clicks == 0 ? FString(TEXT("Even"))
				: FString::Printf(TEXT("%s %d"), Clicks > 0 ? TEXT("Early") : TEXT("Late"), FMath::Abs(Clicks));
		}
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
